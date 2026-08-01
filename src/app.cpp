#include "app.h"

#include "layout.h"
#include "resource.h"
#include "settings_store.h"
#include "ui_theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

namespace nekodrag {
namespace {

constexpr wchar_t kMainWindowClass[] = L"NekoDrag.HiddenWindow";
constexpr wchar_t kMainWindowTitle[] = L"NekoDrag.MessageWindow";
constexpr wchar_t kSettingsWindowClass[] = L"NekoDrag.SettingsWindow";
constexpr wchar_t kInstanceMutex[] = L"Local\\NekoDrag.SingleInstance";

// Legacy identifiers are retained only to prevent duplicate hooks during an
// in-place upgrade and to focus an already-running legacy instance.
constexpr wchar_t kLegacyMainWindowClass[] = L"SuperDrag.HiddenWindow";
constexpr wchar_t kLegacyMainWindowTitle[] = L"SuperDrag.MessageWindow";
constexpr wchar_t kLegacyInstanceMutex[] =
    L"Local\\SuperDrag.SingleInstance";

constexpr UINT kMessageTray = WM_APP + 1;
constexpr UINT kMessageBeginDrag = WM_APP + 2;
constexpr UINT kMessageUpdateDrag = WM_APP + 3;
constexpr UINT kMessageOpenSettings = WM_APP + 4;
constexpr UINT kMessagePrivilegeHint = WM_APP + 5;
constexpr UINT kMessageEnsureMouseHook = WM_APP + 6;
constexpr UINT kMessageMoveCompleted = WM_APP + 7;
constexpr UINT kMessageNativeMoveCompleted = WM_APP + 8;
constexpr UINT kMessageNativeMoveStarted = WM_APP + 9;
constexpr UINT kMessageNativeMoveEnded = WM_APP + 10;
constexpr UINT kMessageNativeButtonReleased = WM_APP + 11;
constexpr UINT kMessageStartNativeMove = WM_APP + 12;
constexpr UINT_PTR kRestoreTimer = 1;
constexpr UINT_PTR kHookRetryTimer = 3;
constexpr UINT_PTR kDragReleaseTimer = 4;
constexpr UINT_PTR kNativeFallbackTimer = 5;
constexpr UINT_PTR kNativeCompletionTimer = 6;
constexpr UINT kDragReleaseTimeoutMs = 500;
constexpr UINT kNativeFallbackGraceMs = 100;
constexpr UINT kNativeCompletionTimeoutMs = 1000;
constexpr ULONG_PTR kNativeReleaseReplayExtraInfo =
    static_cast<ULONG_PTR>(0x4E4B4452UL);  // "NKDR"
constexpr std::array<UINT, 3> kHookRetryDelaysMs{250, 1000, 3000};
constexpr unsigned kMaxRestoreAttempts = 30;

constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuToggleEnabled = 2001;
constexpr UINT kMenuSettings = 2002;
constexpr UINT kMenuToggleStartup = 2003;
constexpr UINT kMenuExit = 2004;

constexpr int kControlEnabled = 1001;
constexpr int kControlWin = 1002;
constexpr int kControlControl = 1003;
constexpr int kControlAlt = 1004;
constexpr int kControlShift = 1005;
constexpr int kControlStartup = 1006;
constexpr int kControlStatus = 1007;
constexpr int kControlSave = 1008;
constexpr int kControlCancel = 1009;
constexpr int kControlModifierGroup = 1010;
constexpr int kControlHelp = 1011;
constexpr int kControlDragModeGroup = 1012;
constexpr int kControlDragModeAutomatic = 1013;
constexpr int kControlDragModeNative = 1014;
constexpr int kControlDragModeCompatibility = 1015;
constexpr std::array<int, 3> kDragModeControlIds{
    kControlDragModeCompatibility,
    kControlDragModeAutomatic,
    kControlDragModeNative,
};

NekoDragApp* gApp = nullptr;

#ifdef NEKODRAG_TRACE
const wchar_t* DragEngineModeTraceName(DragEngineMode mode) noexcept {
    switch (mode) {
        case DragEngineMode::Automatic:
            return L"automatic";
        case DragEngineMode::NativeOnly:
            return L"native-only";
        case DragEngineMode::CompatibilityOnly:
            return L"compatibility-only";
    }
    return L"invalid";
}

const wchar_t* NativeMoveStrategyTraceName(
    NativeMoveStrategy strategy) noexcept {
    switch (strategy) {
        case NativeMoveStrategy::NonClientCaption:
            return L"non-client-caption";
        case NativeMoveStrategy::SystemCommand:
            return L"system-command";
    }
    return L"invalid";
}

void TraceDragState(const wchar_t* format, ...) {
    wchar_t message[256]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, std::size(message), _TRUNCATE, format, args);
    va_end(args);

    wchar_t line[288]{};
    swprintf_s(line, std::size(line), L"[NekoDrag] %ls\n", message);
    OutputDebugStringW(line);
}
#define ND_TRACE(...) TraceDragState(__VA_ARGS__)
#else
#define ND_TRACE(...) ((void)0)
#endif

std::wstring ErrorWithCode(const wchar_t* message, DWORD errorCode) {
    std::wstring result(message);
    result.append(L"（错误代码 ");
    result.append(std::to_wstring(errorCode));
    result.append(L"）");
    return result;
}

bool IsKeyDown(int virtualKey) noexcept {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

Point PointFromNative(POINT point) noexcept {
    return {static_cast<std::int32_t>(point.x),
            static_cast<std::int32_t>(point.y)};
}

bool QueryProcessIntegrity(HANDLE process, DWORD* integrityLevel) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
        return false;
    }

    DWORD requiredSize = 0;
    GetTokenInformation(rawToken, TokenIntegrityLevel, nullptr, 0,
                        &requiredSize);
    if (requiredSize == 0) {
        CloseHandle(rawToken);
        return false;
    }

    std::vector<BYTE> buffer(requiredSize);
    const BOOL queried = GetTokenInformation(
        rawToken, TokenIntegrityLevel, buffer.data(), requiredSize,
        &requiredSize);
    CloseHandle(rawToken);
    if (!queried) {
        return false;
    }

    const auto* label =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const PUCHAR count = GetSidSubAuthorityCount(label->Label.Sid);
    if (count == nullptr || *count == 0) {
        return false;
    }
    *integrityLevel =
        *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count - 1));
    return true;
}

bool QueryWindowIntegrity(HWND window, DWORD* integrityLevel) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 processId);
    if (process == nullptr) {
        return false;
    }
    const bool result = QueryProcessIntegrity(process, integrityLevel);
    CloseHandle(process);
    return result;
}

bool IsExcludedWindowClass(HWND window) {
    wchar_t className[128]{};
    if (GetClassNameW(window, className,
                      static_cast<int>(std::size(className))) == 0) {
        return false;
    }
    return _wcsicmp(className, L"Progman") == 0 ||
           _wcsicmp(className, L"WorkerW") == 0 ||
           _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0 ||
           _wcsicmp(className, L"tooltips_class32") == 0 ||
           _wcsicmp(className, L"#32768") == 0;
}

void SetCheckbox(HWND parent, int controlId, bool checked) {
    HWND control = GetDlgItem(parent, controlId);
    if (control == nullptr) {
        return;
    }
    SetWindowLongPtrW(control, GWLP_USERDATA,
                      checked ? BST_CHECKED : BST_UNCHECKED);
    InvalidateRect(control, nullptr, FALSE);
    UpdateWindow(control);
}

bool IsCheckboxChecked(HWND parent, int controlId) {
    HWND control = GetDlgItem(parent, controlId);
    return control != nullptr &&
           GetWindowLongPtrW(control, GWLP_USERDATA) == BST_CHECKED;
}

int DragModeControlId(DragEngineMode mode) noexcept {
    switch (mode) {
        case DragEngineMode::Automatic:
            return kControlDragModeAutomatic;
        case DragEngineMode::NativeOnly:
            return kControlDragModeNative;
        case DragEngineMode::CompatibilityOnly:
            return kControlDragModeCompatibility;
    }
    return kControlDragModeCompatibility;
}

void SetDragModeSelection(HWND parent, int selectedControlId) {
    for (const int controlId : kDragModeControlIds) {
        SetCheckbox(parent, controlId, controlId == selectedControlId);
    }
}

DragEngineMode SelectedDragEngineMode(HWND parent) noexcept {
    if (IsCheckboxChecked(parent, kControlDragModeNative)) {
        return DragEngineMode::NativeOnly;
    }
    if (IsCheckboxChecked(parent, kControlDragModeAutomatic)) {
        return DragEngineMode::Automatic;
    }
    return DragEngineMode::CompatibilityOnly;
}

void NotifyExistingInstance(const wchar_t* windowClass,
                            const wchar_t* windowTitle) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        HWND existing = FindWindowW(windowClass, windowTitle);
        if (existing != nullptr) {
            PostMessageW(existing, kMessageOpenSettings, 0, 0);
            return;
        }
        Sleep(25);
    }
}

}  // namespace

NekoDragApp::NekoDragApp(HINSTANCE instance) noexcept : instance_(instance) {
    gApp = this;
}

NekoDragApp::~NekoDragApp() {
    Shutdown();
    if (gApp == this) {
        gApp = nullptr;
    }
}

int NekoDragApp::Run(int) {
    if (!EnsureSingleInstance()) {
        return 0;
    }

    std::wstring error;
    if (!Initialize(&error)) {
        MessageBoxW(nullptr, error.c_str(), L"NekoDrag 启动失败",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (settingsWindow_ != nullptr &&
            IsWindowVisible(settingsWindow_) &&
            IsDialogMessageW(settingsWindow_, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}

bool NekoDragApp::EnsureSingleInstance() {
    instanceMutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (instanceMutex_ == nullptr) {
        MessageBoxW(nullptr, L"无法创建单实例锁。", L"NekoDrag 启动失败",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        NotifyExistingInstance(kMainWindowClass, kMainWindowTitle);
        return false;
    }

    legacyInstanceMutex_ = CreateMutexW(nullptr, TRUE, kLegacyInstanceMutex);
    if (legacyInstanceMutex_ == nullptr) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        MessageBoxW(nullptr, L"无法创建升级兼容单实例锁。",
                    L"NekoDrag 启动失败", MB_OK | MB_ICONERROR);
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(legacyInstanceMutex_);
        legacyInstanceMutex_ = nullptr;
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        NotifyExistingInstance(kLegacyMainWindowClass,
                               kLegacyMainWindowTitle);
        return false;
    }

    ReleaseMutex(instanceMutex_);
    ReleaseMutex(legacyInstanceMutex_);
    return true;
}

bool NekoDragApp::Initialize(std::wstring* error) {
    SettingsLoadInfo settingsLoadInfo;
    if (!LoadSettings(&settings_, &settingsLoadInfo, error)) {
        return false;
    }
    const bool firstLaunch =
        !settingsLoadInfo.settingsKeyExists ||
        (!settingsLoadInfo.importedLegacySettings &&
         !settings_.firstRunCompleted);

    if (!QueryProcessIntegrity(GetCurrentProcess(), &ownIntegrityLevel_)) {
        ownIntegrityLevel_ = SECURITY_MANDATORY_MEDIUM_RID;
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterWindowClasses(error) || !CreateMainWindow(error) ||
        !StartMoveWorker(error)) {
        return false;
    }
    InitializeNativeMoveInfrastructure();
    if (!AddTrayIcon()) {
        *error = L"无法创建系统托盘图标。";
        return false;
    }

    if (settingsLoadInfo.importedLegacySettings) {
        settings_.firstRunCompleted = true;
        std::wstring migrationError;
        if (!SaveSettings(settings_, &migrationError)) {
            std::wstring message = L"无法迁移旧 SuperDrag 设置";
            if (!migrationError.empty()) {
                message.append(L"：");
                message.append(migrationError);
            }
            ShowTrayNotification(L"NekoDrag", message.c_str(), NIIF_WARNING);
        }
    }

    std::wstring startupError;
    if (!ReconcileStartupPath(settingsLoadInfo.importedLegacySettings,
                              &startupError)) {
        ShowTrayNotification(L"NekoDrag",
                             startupError.empty()
                                 ? L"无法更新开机启动路径。"
                                 : startupError.c_str(),
                             NIIF_WARNING);
    }

    if (firstLaunch) {
        settings_.firstRunCompleted = true;
        std::wstring saveError;
        if (!SaveSettings(settings_, &saveError)) {
            ShowTrayNotification(L"NekoDrag", saveError.c_str(),
                                 NIIF_WARNING);
        }
    }

    if (settings_.enabled) {
        RequestMouseHookInstall(true);
    } else {
        hookRuntimeState_ = HookRuntimeState::Stopped;
    }

    if (firstLaunch) {
        ShowSettingsWindow();
    }
    return true;
}

bool NekoDragApp::StartMoveWorker(std::wstring* error) {
    moveWorker_ =
        std::make_unique<WindowMoveWorker>(mainWindow_, kMessageMoveCompleted);
    if (!moveWorker_->Start()) {
        moveWorker_.reset();
        if (error != nullptr) {
            *error = L"无法启动窗口移动线程。";
        }
        return false;
    }
    return true;
}

void NekoDragApp::InitializeNativeMoveInfrastructure() {
    nativeMoveAvailable_ = false;
    nativeEventCompletionWindow_.store(mainWindow_,
                                       std::memory_order_release);
    if (!InstallNativeMoveEventHook()) {
        nativeEventCompletionWindow_.store(nullptr,
                                           std::memory_order_release);
        return;
    }
    if (!StartNativeMoveWorker()) {
        RemoveNativeMoveEventHook();
        return;
    }
    nativeMoveAvailable_ = true;
}

bool NekoDragApp::StartNativeMoveWorker() {
    nativeMoveWorker_ = std::make_unique<NativeMoveWorker>(
        mainWindow_, kMessageNativeMoveCompleted);
    if (!nativeMoveWorker_->Start()) {
        nativeMoveWorker_.reset();
        ND_TRACE(L"native move worker failed to start");
        return false;
    }
    return true;
}

bool NekoDragApp::InstallNativeMoveEventHook() {
    if (nativeMoveEventHook_ != nullptr) {
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    nativeMoveEventHook_ = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
        NativeMoveEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (nativeMoveEventHook_ == nullptr) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        ND_TRACE(L"native move event hook install failed error=%lu",
                 static_cast<unsigned long>(error));
        static_cast<void>(error);
        return false;
    }
    return true;
}

void NekoDragApp::RemoveNativeMoveEventHook() {
    nativeMoveAvailable_ = false;
    nativeEventToken_.store(0, std::memory_order_release);
    nativeEventAttemptStartedAt_.store(0, std::memory_order_release);
    nativeEventCompletionWindow_.store(nullptr, std::memory_order_release);
    if (nativeMoveEventHook_ != nullptr) {
        UnhookWinEvent(nativeMoveEventHook_);
        nativeMoveEventHook_ = nullptr;
    }
}

void NekoDragApp::SuspendNativeMoveWorker() {
    nativeMoveAvailable_ = false;
}

bool NekoDragApp::RegisterWindowClasses(std::wstring* error) {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.lpszClassName = kMainWindowClass;
    if (RegisterClassExW(&mainClass) == 0) {
        *error = ErrorWithCode(L"注册后台窗口失败", GetLastError());
        return false;
    }

    WNDCLASSEXW settingsClass{};
    settingsClass.cbSize = sizeof(settingsClass);
    settingsClass.lpfnWndProc = SettingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = nullptr;
    settingsClass.lpszClassName = kSettingsWindowClass;
    if (RegisterClassExW(&settingsClass) == 0) {
        *error = ErrorWithCode(L"注册设置窗口失败", GetLastError());
        return false;
    }
    return true;
}

bool NekoDragApp::CreateMainWindow(std::wstring* error) {
    mainWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kMainWindowClass, kMainWindowTitle, WS_POPUP, 0, 0,
        0, 0, nullptr, nullptr, instance_, this);
    if (mainWindow_ == nullptr) {
        *error = ErrorWithCode(L"创建后台窗口失败", GetLastError());
        return false;
    }
    return true;
}

bool NekoDragApp::CreateSettingsWindow(std::wstring* error) {
    if (settingsWindow_ != nullptr) {
        return true;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_THICKFRAME | WS_CLIPCHILDREN;
    const DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        *error = ErrorWithCode(L"无法获取显示器信息", GetLastError());
        return false;
    }

    auto windowRectForDpi = [style, extendedStyle](UINT dpi) {
        const int clientWidth = ui::SettingsLayout::Scale(
            ui::SettingsLayout::kMinClientWidth, dpi);
        const int clientHeight = ui::SettingsLayout::Scale(
            ui::SettingsLayout::kMinClientHeight, dpi);
        RECT rect{0, 0, clientWidth, clientHeight};
        AdjustWindowRectExForDpi(&rect, style, FALSE, extendedStyle, dpi);
        return rect;
    };

    const UINT bootstrapDpi = GetDpiForSystem();
    RECT windowRect = windowRectForDpi(bootstrapDpi);
    const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
    const int workTop = static_cast<int>(monitorInfo.rcWork.top);
    const int workWidth = static_cast<int>(
        monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = static_cast<int>(
        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    const int bootstrapWidth =
        static_cast<int>(windowRect.right - windowRect.left);
    const int bootstrapHeight =
        static_cast<int>(windowRect.bottom - windowRect.top);
    const int width = std::max(1, std::min(bootstrapWidth, workWidth));
    const int height = std::max(1, std::min(bootstrapHeight, workHeight));
    const int x = workLeft + (workWidth - width) / 2;
    const int y = workTop + (workHeight - height) / 2;

    settingsWindow_ = CreateWindowExW(
        extendedStyle, kSettingsWindowClass, L"NekoDrag 设置", style, x, y,
        width, height, mainWindow_, nullptr, instance_, this);
    if (settingsWindow_ == nullptr) {
        *error = ErrorWithCode(L"创建设置窗口失败", GetLastError());
        return false;
    }

    // The proposed coordinates place the hidden window on the cursor's
    // monitor. Query its actual per-monitor DPI, then size and center it using
    // that DPI before the first ShowWindow call.
    const UINT queriedDpi = GetDpiForWindow(settingsWindow_);
    const UINT windowDpi = queriedDpi != 0 ? queriedDpi : bootstrapDpi;
    windowRect = windowRectForDpi(windowDpi);
    const int desiredWidth =
        static_cast<int>(windowRect.right - windowRect.left);
    const int desiredHeight =
        static_cast<int>(windowRect.bottom - windowRect.top);
    const int desiredX = workLeft + (workWidth - desiredWidth) / 2;
    const int desiredY = workTop + (workHeight - desiredHeight) / 2;
    if (!SetWindowPos(settingsWindow_, nullptr, desiredX, desiredY,
                      desiredWidth, desiredHeight,
                      SWP_NOZORDER | SWP_NOACTIVATE)) {
        const DWORD resizeError = GetLastError();
        DestroyWindow(settingsWindow_);
        *error = ErrorWithCode(L"调整设置窗口大小失败", resizeError);
        return false;
    }
    return true;
}

void NekoDragApp::Shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;

    if (moveWorker_ != nullptr) {
        moveWorker_->StopAccepting();
    }
    if (nativeMoveWorker_ != nullptr) {
        nativeMoveWorker_->StopAccepting();
    }
    nativeEventToken_.store(0, std::memory_order_release);
    EndDrag(L"shutdown");
    ++dragGenerationCounter_;
    CancelMouseHookRetry();
    RemoveMouseHook();
    RemoveNativeMoveEventHook();
    hookRuntimeState_ = HookRuntimeState::Stopped;
    if (nativeMoveWorker_ != nullptr) {
        nativeMoveWorker_->Stop();
        nativeMoveWorker_.reset();
    }
    if (moveWorker_ != nullptr) {
        moveWorker_->Stop();
        moveWorker_.reset();
    }
    RemoveTrayIcon();
    if (settingsWindow_ != nullptr && IsWindow(settingsWindow_)) {
        DestroyWindow(settingsWindow_);
        settingsWindow_ = nullptr;
    }
    if (settingsFont_ != nullptr) {
        DeleteObject(settingsFont_);
        settingsFont_ = nullptr;
    }
    if (instanceMutex_ != nullptr) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
    if (legacyInstanceMutex_ != nullptr) {
        CloseHandle(legacyInstanceMutex_);
        legacyInstanceMutex_ = nullptr;
    }
}

LRESULT CALLBACK NekoDragApp::MainWindowProc(HWND window, UINT message,
                                               WPARAM wParam,
                                               LPARAM lParam) {
    NekoDragApp* app = reinterpret_cast<NekoDragApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<NekoDragApp*>(create->lpCreateParams);
        app->mainWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr
               ? app->OnMainMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK NekoDragApp::SettingsWindowProc(HWND window, UINT message,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
    NekoDragApp* app = reinterpret_cast<NekoDragApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<NekoDragApp*>(create->lpCreateParams);
        app->settingsWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr
               ? app->OnSettingsMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK NekoDragApp::MouseHookProc(int code, WPARAM message,
                                              LPARAM lParam) {
    if (gApp == nullptr) {
        return CallNextHookEx(nullptr, code, message, lParam);
    }
    return gApp->HandleMouseHook(
        code, message, reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
}

void CALLBACK NekoDragApp::NativeMoveEventProc(
    HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD,
    DWORD eventTime) {
    if (gApp == nullptr || window == nullptr ||
        (event != EVENT_SYSTEM_MOVESIZESTART &&
         event != EVENT_SYSTEM_MOVESIZEEND)) {
        return;
    }
    const std::uint64_t eventToken =
        gApp->nativeEventToken_.load(std::memory_order_acquire);
    const HWND completionWindow =
        gApp->nativeEventCompletionWindow_.load(std::memory_order_acquire);
    const std::uint32_t attemptStartedAt =
        gApp->nativeEventAttemptStartedAt_.load(
            std::memory_order_acquire);
    if (eventToken == 0 || completionWindow == nullptr ||
        !IsNativeMoveEventTimeCurrent(
            static_cast<std::uint32_t>(eventTime), attemptStartedAt)) {
        return;
    }
    const UINT message = event == EVENT_SYSTEM_MOVESIZESTART
                             ? kMessageNativeMoveStarted
                             : kMessageNativeMoveEnded;
    PostMessageW(completionWindow, message,
                 static_cast<WPARAM>(eventToken),
                 reinterpret_cast<LPARAM>(window));
}

LRESULT NekoDragApp::OnMainMessage(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        trayIconAdded_ = false;
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case kMessageTray: {
            const UINT event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                POINT point{};
                GetCursorPos(&point);
                ShowTrayMenu(point);
            } else if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
                ShowSettingsWindow();
            }
            return 0;
        }
        case kMessageBeginDrag:
            BeginDragOnMessageThread();
            return 0;
        case kMessageUpdateDrag:
            ApplyLatestDragPosition();
            return 0;
        case kMessageMoveCompleted:
            HandleMoveCompleted();
            return 0;
        case kMessageNativeMoveCompleted:
            HandleNativeMoveCompleted();
            return 0;
        case kMessageNativeMoveStarted:
            HandleNativeMoveEvent(
                true, static_cast<std::uint64_t>(wParam),
                reinterpret_cast<HWND>(lParam));
            return 0;
        case kMessageNativeMoveEnded:
            HandleNativeMoveEvent(
                false, static_cast<std::uint64_t>(wParam),
                reinterpret_cast<HWND>(lParam));
            return 0;
        case kMessageNativeButtonReleased:
            HandleNativeButtonReleased(
                static_cast<std::uint64_t>(wParam),
                reinterpret_cast<HWND>(lParam));
            return 0;
        case kMessageStartNativeMove:
            HandleNativeMoveStartRequested(
                static_cast<std::uint64_t>(wParam),
                reinterpret_cast<HWND>(lParam));
            return 0;
        case kMessageOpenSettings:
            ShowSettingsWindow();
            return 0;
        case kMessagePrivilegeHint:
            ShowPrivilegeHintOnce();
            return 0;
        case kMessageEnsureMouseHook:
            hookInstallMessagePending_ = false;
            AttemptMouseHookInstall();
            return 0;
        case WM_TIMER:
            if (wParam == kRestoreTimer) {
                KillTimer(window, kRestoreTimer);
                ScheduleDragUpdate();
                return 0;
            }
            if (wParam == kHookRetryTimer) {
                KillTimer(window, kHookRetryTimer);
                AttemptMouseHookInstall();
                return 0;
            }
            if (wParam == kDragReleaseTimer) {
                KillTimer(window, kDragReleaseTimer);
                if (drag_.active && drag_.releasePending) {
                    ND_TRACE(L"end drag: final move timed out");
                    EndDrag(L"final-move-timeout");
                }
                return 0;
            }
            if (wParam == kNativeFallbackTimer) {
                KillTimer(window, kNativeFallbackTimer);
                HandleNativeFallbackTimeout();
                return 0;
            }
            if (wParam == kNativeCompletionTimer) {
                KillTimer(window, kNativeCompletionTimer);
                if (drag_.active && IsNativeDrag()) {
                    ND_TRACE(L"native move completion timed out generation=%llu",
                             static_cast<unsigned long long>(
                                 drag_.generation));
                    CancelTargetNativeMove();
                    SuspendNativeMoveWorker();
                    if (drag_.engineMode == DragEngineMode::NativeOnly) {
                        FailNativeOnlyDrag(L"native-only-move-timeout",
                                           ERROR_TIMEOUT, true);
                    } else {
                        CompleteNativeDrag(L"native-move-timeout");
                    }
                }
                return 0;
            }
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam != FALSE) {
                DestroyWindow(window);
            }
            return 0;
        case WM_DESTROY:
            Shutdown();
            mainWindow_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT NekoDragApp::OnSettingsMessage(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            theme_ = std::make_unique<ui::UiTheme>();
            CreateSettingsControls();
            LayoutSettingsControls(GetDpiForWindow(window));
            ApplyThemeToSettingsWindow();
            return 0;
        case DM_GETDEFID:
            return MAKELRESULT(kControlSave, DC_HASDEFID);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDCANCEL) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case kControlDragModeAutomatic:
                    case kControlDragModeNative:
                    case kControlDragModeCompatibility:
                        SetDragModeSelection(window, id);
                        return 0;
                    case kControlEnabled:
                    case kControlWin:
                    case kControlControl:
                    case kControlAlt:
                    case kControlShift:
                    case kControlStartup: {
                        SetCheckbox(window, id,
                                    !IsCheckboxChecked(window, id));
                        return 0;
                    }
                    case kControlSave:
                        SaveSettingsFromControls();
                        return 0;
                    case kControlCancel:
                        ShowWindow(window, SW_HIDE);
                        return 0;
                    default:
                        break;
                }
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                SendDlgItemMessageW(window, kControlSave, BM_CLICK, 0, 0);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_ERASEBKGND: {
            if (theme_ != nullptr) {
                RECT clientRect{};
                GetClientRect(window, &clientRect);
                FillRect(reinterpret_cast<HDC>(wParam), &clientRect,
                         theme_->BackgroundBrush());
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (theme_ != nullptr) {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                const HWND control = reinterpret_cast<HWND>(lParam);
                const int control_id = GetDlgCtrlID(control);
                if (control_id == kControlStatus && settingsStatusError_) {
                    SetTextColor(hdc, theme_->ErrorColor());
                } else if (control_id == kControlHelp) {
                    SetTextColor(hdc, theme_->SecondaryTextColor());
                } else {
                    SetTextColor(hdc, theme_->TextColor());
                }
                SetBkMode(hdc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(theme_->BackgroundBrush());
            }
            break;
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            switch (dis->CtlID) {
                case kControlEnabled:
                case kControlWin:
                case kControlControl:
                case kControlAlt:
                case kControlShift:
                case kControlStartup: {
                    DRAWITEMSTRUCT checkbox_state = *dis;
                    if (IsCheckboxChecked(window,
                                          static_cast<int>(dis->CtlID))) {
                        checkbox_state.itemState |= ODS_CHECKED;
                    } else {
                        checkbox_state.itemState &= ~ODS_CHECKED;
                    }
                    const bool on_surface =
                        dis->CtlID == kControlWin ||
                        dis->CtlID == kControlControl ||
                        dis->CtlID == kControlAlt ||
                        dis->CtlID == kControlShift;
                    ui::DrawThemedCheckbox(&checkbox_state, *theme_,
                                           on_surface);
                    return TRUE;
                }
                case kControlDragModeAutomatic:
                case kControlDragModeNative:
                case kControlDragModeCompatibility: {
                    DRAWITEMSTRUCT radio_state = *dis;
                    if (IsCheckboxChecked(window,
                                          static_cast<int>(dis->CtlID))) {
                        radio_state.itemState |= ODS_CHECKED;
                    } else {
                        radio_state.itemState &= ~ODS_CHECKED;
                    }
                    ui::DrawThemedRadioButton(&radio_state, *theme_, true);
                    return TRUE;
                }
                case kControlModifierGroup:
                case kControlDragModeGroup:
                    ui::DrawThemedGroupBox(dis, *theme_);
                    return TRUE;
                case kControlSave:
                case kControlCancel:
                    if (dis->CtlID == kControlSave) {
                        DRAWITEMSTRUCT dis_copy = *dis;
                        dis_copy.itemState |= ODS_DEFAULT;
                        ui::DrawThemedPushButton(&dis_copy, *theme_);
                    } else {
                        ui::DrawThemedPushButton(dis, *theme_);
                    }
                    return TRUE;
                default:
                    break;
            }
            break;
        }
        case WM_SIZE:
            if (theme_ != nullptr && wParam != SIZE_MINIMIZED) {
                LayoutSettingsControls(GetDpiForWindow(window));
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(window);
            const int minClientWidth = ui::SettingsLayout::Scale(
                ui::SettingsLayout::kMinClientWidth, dpi);
            const int minClientHeight = ui::SettingsLayout::Scale(
                ui::SettingsLayout::kMinClientHeight, dpi);
            RECT rect{0, 0, minClientWidth, minClientHeight};
            const DWORD style = static_cast<DWORD>(
                GetWindowLongPtrW(window, GWL_STYLE));
            const DWORD extendedStyle = static_cast<DWORD>(
                GetWindowLongPtrW(window, GWL_EXSTYLE));
            AdjustWindowRectExForDpi(&rect, style, FALSE, extendedStyle, dpi);
            info->ptMinTrackSize.x = rect.right - rect.left;
            info->ptMinTrackSize.y = rect.bottom - rect.top;
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutSettingsControls(HIWORD(wParam));
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (theme_ != nullptr && lParam != 0 &&
                wcscmp(reinterpret_cast<LPCWSTR>(lParam),
                       L"ImmersiveColorSet") == 0) {
                theme_->Refresh();
                ApplyThemeToSettingsWindow();
            }
            return 0;
        case WM_THEMECHANGED:
            if (theme_ != nullptr) {
                theme_->Refresh();
                ApplyThemeToSettingsWindow();
            }
            return 0;
        case WM_NCDESTROY:
            theme_.reset();
            settingsWindow_ = nullptr;
            return DefWindowProcW(window, message, wParam, lParam);
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool NekoDragApp::InstallMouseHook(std::wstring* error, DWORD* errorCode) {
    if (mouseHook_ != nullptr) {
        if (errorCode != nullptr) {
            *errorCode = ERROR_SUCCESS;
        }
        return true;
    }
    SetLastError(ERROR_SUCCESS);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance_, 0);
    if (mouseHook_ == nullptr) {
        DWORD code = GetLastError();
        if (code == ERROR_SUCCESS) {
            code = ERROR_GEN_FAILURE;
        }
        if (errorCode != nullptr) {
            *errorCode = code;
        }
        if (error != nullptr) {
            *error = ErrorWithCode(L"无法启用全局鼠标监听", code);
        }
        return false;
    }
    if (errorCode != nullptr) {
        *errorCode = ERROR_SUCCESS;
    }
    return true;
}

void NekoDragApp::RemoveMouseHook() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void NekoDragApp::RequestMouseHookInstall(bool resetRetries) {
    if (resetRetries) {
        hookRetryIndex_ = 0;
    }
    if (shuttingDown_ || !settings_.enabled || mainWindow_ == nullptr) {
        CancelMouseHookRetry();
        hookRuntimeState_ = HookRuntimeState::Stopped;
        return;
    }
    if (mouseHook_ != nullptr) {
        hookRuntimeState_ = HookRuntimeState::Active;
        UpdateHookStatusInSettings();
        return;
    }

    KillTimer(mainWindow_, kHookRetryTimer);
    hookRuntimeState_ = HookRuntimeState::Starting;
    UpdateHookStatusInSettings();
    if (hookInstallMessagePending_) {
        return;
    }
    if (PostMessageW(mainWindow_, kMessageEnsureMouseHook, 0, 0)) {
        hookInstallMessagePending_ = true;
        ND_TRACE(L"mouse hook install queued");
        return;
    }

    DWORD code = GetLastError();
    if (code == ERROR_SUCCESS) {
        code = ERROR_GEN_FAILURE;
    }
    HandleMouseHookInstallFailure(
        code, ErrorWithCode(L"无法安排全局鼠标监听", code));
}

void NekoDragApp::AttemptMouseHookInstall() {
    if (shuttingDown_ || !settings_.enabled || mainWindow_ == nullptr) {
        hookRuntimeState_ = HookRuntimeState::Stopped;
        return;
    }
    if (mouseHook_ != nullptr) {
        hookRuntimeState_ = HookRuntimeState::Active;
        hookRetryIndex_ = 0;
        lastHookError_ = ERROR_SUCCESS;
        lastHookErrorText_.clear();
        UpdateHookStatusInSettings();
        return;
    }

    std::wstring error;
    DWORD errorCode = ERROR_SUCCESS;
    ND_TRACE(L"installing mouse hook attempt=%llu",
             static_cast<unsigned long long>(hookRetryIndex_ + 1));
    if (InstallMouseHook(&error, &errorCode)) {
        hookRuntimeState_ = HookRuntimeState::Active;
        hookRetryIndex_ = 0;
        lastHookError_ = ERROR_SUCCESS;
        lastHookErrorText_.clear();
        ND_TRACE(L"mouse hook installed handle=%p", mouseHook_);
        UpdateHookStatusInSettings();
        return;
    }
    HandleMouseHookInstallFailure(errorCode, error);
}

void NekoDragApp::HandleMouseHookInstallFailure(
    DWORD errorCode, const std::wstring& error) {
    lastHookError_ = errorCode;
    lastHookErrorText_ = error;
    ND_TRACE(L"mouse hook install failed error=%lu retryIndex=%llu",
             static_cast<unsigned long>(errorCode),
             static_cast<unsigned long long>(hookRetryIndex_));

    if (!shuttingDown_ && settings_.enabled && mainWindow_ != nullptr &&
        hookRetryIndex_ < kHookRetryDelaysMs.size()) {
        const UINT delay = kHookRetryDelaysMs[hookRetryIndex_++];
        if (SetTimer(mainWindow_, kHookRetryTimer, delay, nullptr) != 0) {
            hookRuntimeState_ = HookRuntimeState::Starting;
            ND_TRACE(L"mouse hook retry scheduled delay=%u", delay);
            SetHookStatus(L"正在重试启用窗口拖动；上次失败：" + error,
                          true);
            return;
        }
    }

    hookRuntimeState_ = HookRuntimeState::Failed;
    SetHookStatus(error + L"。请从托盘菜单重试启用。", true);
    ShowTrayNotification(L"NekoDrag", lastHookErrorText_.c_str(),
                         NIIF_ERROR);
}

void NekoDragApp::CancelMouseHookRetry() {
    if (mainWindow_ != nullptr) {
        KillTimer(mainWindow_, kHookRetryTimer);
    }
    hookInstallMessagePending_ = false;
    hookRetryIndex_ = 0;
}

bool NekoDragApp::IsMouseHookActive() const noexcept {
    return hookRuntimeState_ == HookRuntimeState::Active &&
           mouseHook_ != nullptr;
}

std::uint32_t NekoDragApp::CurrentModifierMask() const noexcept {
    std::uint32_t mask = 0;
    if (IsKeyDown(VK_MENU)) {
        mask |= kModifierAlt;
    }
    if (IsKeyDown(VK_CONTROL)) {
        mask |= kModifierControl;
    }
    if (IsKeyDown(VK_SHIFT)) {
        mask |= kModifierShift;
    }
    if (IsKeyDown(VK_LWIN) || IsKeyDown(VK_RWIN)) {
        mask |= kModifierWin;
    }
    return mask;
}

bool NekoDragApp::IsConfiguredChordDown() const noexcept {
    return IsExactModifierMatch(settings_.modifierMask,
                                CurrentModifierMask());
}

HWND NekoDragApp::ResolveDragTarget(POINT cursor, bool* restricted,
                                    HWND* initialPressWindow) const {
    *restricted = false;
    if (initialPressWindow != nullptr) {
        *initialPressWindow = nullptr;
    }
    HWND target = WindowFromPoint(cursor);
    if (target == nullptr) {
        return nullptr;
    }
    if (initialPressWindow != nullptr) {
        *initialPressWindow = target;
    }
    target = GetAncestor(target, GA_ROOT);
    if (target == nullptr) {
        return nullptr;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(target, &processId);
    const LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
    DWORD cloaked = 0;
    const HRESULT cloakResult = DwmGetWindowAttribute(
        target, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    WindowTraits traits;
    traits.exists = IsWindow(target) != FALSE;
    traits.visible = IsWindowVisible(target) != FALSE;
    traits.enabled = IsWindowEnabled(target) != FALSE;
    traits.minimized = IsIconic(target) != FALSE;
    traits.topLevel = (style & WS_CHILD) == 0 &&
                      GetAncestor(target, GA_ROOT) == target;
    traits.ownProcess = processId == GetCurrentProcessId();
    traits.shellSurface = target == GetDesktopWindow() ||
                          target == GetShellWindow() ||
                          IsExcludedWindowClass(target);
    traits.cloaked = SUCCEEDED(cloakResult) && cloaked != 0;
    traits.transientSurface = (GetWindowLongPtrW(target, GWL_EXSTYLE) &
                               WS_EX_NOACTIVATE) != 0;
    if (!IsMovableWindowCandidate(traits)) {
        return nullptr;
    }

    if (IsTargetHigherIntegrity(target)) {
        *restricted = true;
        return nullptr;
    }
    return target;
}

HWND NekoDragApp::ResolveTargetCaptureWindow(HWND target) const {
    if (target == nullptr || !IsWindow(target)) {
        return nullptr;
    }
    const DWORD threadId = GetWindowThreadProcessId(target, nullptr);
    if (threadId == 0) {
        return nullptr;
    }
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(threadId, &info) || info.hwndCapture == nullptr ||
        !IsWindow(info.hwndCapture) ||
        GetAncestor(info.hwndCapture, GA_ROOT) != target) {
        return nullptr;
    }
    return info.hwndCapture;
}

bool NekoDragApp::IsTargetHigherIntegrity(HWND target) const {
    DWORD targetIntegrity = 0;
    if (!QueryWindowIntegrity(target, &targetIntegrity)) {
        return true;
    }
    return targetIntegrity > ownIntegrityLevel_;
}

bool NekoDragApp::BeginDragFromHook(HWND target, HWND initialPressWindow,
                                    Point cursor) {
    RECT windowRect{};
    if (!GetWindowRect(target, &windowRect)) {
        return false;
    }

    drag_ = DragState{};
    drag_.active = true;
    drag_.beginPending = true;
    drag_.engineMode = settings_.dragEngineMode;
    drag_.startAction = SelectDragStartAction(
        drag_.engineMode,
        nativeMoveAvailable_ && nativeMoveWorker_ != nullptr);
    drag_.nativePressForwarded =
        ShouldForwardInitialPress(drag_.startAction);
    drag_.generation = ++dragGenerationCounter_;
    if (drag_.generation == 0) {
        drag_.generation = ++dragGenerationCounter_;
    }
    drag_.target = target;
    drag_.initialPressWindow = initialPressWindow;
    drag_.startCursor = cursor;
    drag_.latestCursor = cursor;
    const Point windowOrigin = PointFromNative(
        POINT{windowRect.left, windowRect.top});
    drag_.grabOffset = {cursor.x - windowOrigin.x,
                        cursor.y - windowOrigin.y};
    drag_.lastAppliedOrigin = windowOrigin;
    drag_.lastRequestedOrigin = drag_.lastAppliedOrigin;
    drag_.maximizedRect = {
        static_cast<std::int32_t>(windowRect.left),
        static_cast<std::int32_t>(windowRect.top),
        static_cast<std::int32_t>(windowRect.right),
        static_cast<std::int32_t>(windowRect.bottom),
    };
    drag_.restoring = IsZoomed(target) != FALSE;

    if (!PostMessageW(mainWindow_, kMessageBeginDrag, 0, 0)) {
        EndDrag(L"begin-message-post-failed", false);
        return false;
    }
    return true;
}

LRESULT NekoDragApp::HandleMouseHook(
    int code, WPARAM message, const MSLLHOOKSTRUCT* mouseInfo) {
    if (code < 0 || mouseInfo == nullptr) {
        return CallNextHookEx(mouseHook_, code, message,
                              reinterpret_cast<LPARAM>(mouseInfo));
    }
    // Ignore synthetic input, including our marked one-shot release replay,
    // so an injected stream cannot fight our drag state machine.
    if ((mouseInfo->flags &
         (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0) {
        return CallNextHookEx(mouseHook_, code, message,
                              reinterpret_cast<LPARAM>(mouseInfo));
    }

    const bool buttonsSwapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const WPARAM primaryDownMessage =
        buttonsSwapped ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const WPARAM primaryUpMessage =
        buttonsSwapped ? WM_RBUTTONUP : WM_LBUTTONUP;

    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (message != primaryDownMessage) {
                break;
            }
            if (drag_.active) {
                const bool previousGestureReleased =
                    drag_.buttonReleased || drag_.releasePending;
                if (!previousGestureReleased) {
                    return 1;
                }
                if (drag_.nativePressForwarded &&
                    drag_.nativeReleaseSuppressed &&
                    !drag_.nativeReleaseReplayed) {
                    EndDrag(L"new-button-down-before-release-replay", false);
                    return 1;
                }
                // A completed gesture must not consume a subsequent click
                // merely because its final move is still in flight.
                EndDrag(L"new-button-down-after-release", false);
            }
            if (IsMouseHookActive() && IsConfiguredChordDown()) {
                bool restricted = false;
                HWND initialPressWindow = nullptr;
                const HWND target =
                    ResolveDragTarget(mouseInfo->pt, &restricted,
                                      &initialPressWindow);
                if (restricted) {
                    PostMessageW(mainWindow_, kMessagePrivilegeHint, 0, 0);
                } else if (target != nullptr &&
                           BeginDragFromHook(target, initialPressWindow,
                                             PointFromNative(mouseInfo->pt))) {
                    if (!drag_.nativePressForwarded) {
                        return 1;
                    }
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (drag_.active) {
                drag_.latestCursor = PointFromNative(mouseInfo->pt);
                drag_.nativeMovementObserved = true;
                if (drag_.mode == DragMode::ManualFallback &&
                    !drag_.releasePending) {
                    ScheduleDragUpdate();
                } else if (drag_.mode ==
                               DragMode::NativeAwaitingMovement &&
                           !drag_.nativeStartMessagePending &&
                           !drag_.buttonReleased) {
                    RequestNativeMoveStart();
                }
                // The low-level hook runs before Windows applies the pointer
                // movement. Suppressing WM_MOUSEMOVE keeps the system cursor
                // near its previous position, so later absolute hook points
                // oscillate around the drag origin and the window jitters.
                // Compatibility suppresses the press. Native routing forwards
                // it so Windows can establish the physical button state.
                break;
            }
            break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (message != primaryUpMessage) {
                break;
            }
            if (drag_.active) {
                drag_.latestCursor = PointFromNative(mouseInfo->pt);
                DragReleasePhase releasePhase = DragReleasePhase::Inactive;
                if (drag_.beginPending) {
                    releasePhase = DragReleasePhase::BeginPending;
                } else if (drag_.mode == DragMode::ManualFallback) {
                    releasePhase = DragReleasePhase::Manual;
                } else if (drag_.mode ==
                           DragMode::NativeAwaitingMovement) {
                    releasePhase =
                        DragReleasePhase::NativeAwaitingMovement;
                } else if (drag_.mode == DragMode::NativeStarting) {
                    releasePhase = DragReleasePhase::NativeStarting;
                } else if (drag_.mode == DragMode::NativeActive) {
                    releasePhase = DragReleasePhase::NativeActive;
                }

                if (drag_.nativePressForwarded) {
                    switch (DecideForwardedPressReleaseAction(
                        releasePhase)) {
                        case ForwardedPressReleaseAction::ForwardAndEnd:
                            drag_.buttonReleased = true;
                            EndDrag(L"forwarded-click-before-native-attempt");
                            return CallNextHookEx(
                                mouseHook_, code, message,
                                reinterpret_cast<LPARAM>(mouseInfo));
                        case ForwardedPressReleaseAction::SuppressAndReplay:
                            drag_.nativeReleaseSuppressed = true;
                            NoteNativeButtonReleased();
                            return 1;
                        case ForwardedPressReleaseAction::ForwardAndFinalize:
                            drag_.buttonReleased = true;
                            drag_.releasePending = true;
                            ScheduleDragUpdate();
                            return CallNextHookEx(
                                mouseHook_, code, message,
                                reinterpret_cast<LPARAM>(mouseInfo));
                        case ForwardedPressReleaseAction::ForwardToNative:
                            NoteNativeButtonReleased();
                            return CallNextHookEx(
                                mouseHook_, code, message,
                                reinterpret_cast<LPARAM>(mouseInfo));
                        case ForwardedPressReleaseAction::Ignore:
                            break;
                    }
                }

                switch (DecideDragReleaseAction(releasePhase)) {
                    case DragReleaseAction::SuppressAndFinalize:
                        drag_.buttonReleased = true;
                        drag_.releasePending = true;
                        if (drag_.mode ==
                            DragMode::NativeAwaitingMovement) {
                            RequestNativeMoveStart();
                        } else if (!drag_.beginPending) {
                            ScheduleDragUpdate();
                        }
                        return 1;
                    case DragReleaseAction::SuppressAndReplay:
                        drag_.nativeReleaseSuppressed = true;
                        NoteNativeButtonReleased();
                        return 1;
                    case DragReleaseAction::ForwardToNative:
                        NoteNativeButtonReleased();
                        break;
                    case DragReleaseAction::Ignore:
                        break;
                }
            }
            break;
        default:
            break;
    }
    return CallNextHookEx(mouseHook_, code, message,
                          reinterpret_cast<LPARAM>(mouseInfo));
}

void NekoDragApp::BeginDragOnMessageThread() {
    if (!drag_.active || !drag_.beginPending ||
        !IsWindow(drag_.target)) {
        EndDrag(L"target-invalid-before-begin");
        return;
    }
    drag_.beginPending = false;
    SetForegroundWindow(drag_.target);
    ND_TRACE(L"begin drag generation=%llu target=%p cursor=(%ld,%ld) "
             L"maximized=%d nativeAvailable=%d engine=%ls "
             L"pressForwarded=%d initialPress=%p",
             static_cast<unsigned long long>(drag_.generation), drag_.target,
             static_cast<long>(drag_.startCursor.x),
             static_cast<long>(drag_.startCursor.y),
             drag_.restoring ? 1 : 0, nativeMoveAvailable_ ? 1 : 0,
             DragEngineModeTraceName(drag_.engineMode),
             drag_.nativePressForwarded ? 1 : 0,
             drag_.initialPressWindow);

    const DragStartAction startAction = drag_.startAction;
    if (drag_.buttonReleased) {
        if (drag_.engineMode == DragEngineMode::NativeOnly) {
            EndDrag(L"native-only-released-before-begin");
        } else {
            ND_TRACE(L"compatibility-direct generation=%llu "
                     L"reason=button-released-before-begin",
                     static_cast<unsigned long long>(drag_.generation));
            BeginManualFallback(false);
        }
        return;
    }
    if (startAction == DragStartAction::Compatibility) {
        if (drag_.engineMode == DragEngineMode::CompatibilityOnly) {
            ND_TRACE(L"compatibility-direct generation=%llu",
                     static_cast<unsigned long long>(drag_.generation));
        } else {
            ND_TRACE(L"automatic-fallback generation=%llu "
                     L"reason=native-unavailable",
                     static_cast<unsigned long long>(drag_.generation));
        }
        BeginManualFallback(false);
        return;
    }
    if (startAction == DragStartAction::Reject) {
        FailNativeOnlyDrag(L"native-only-infrastructure-unavailable",
                           ERROR_NOT_READY, false);
        return;
    }

    drag_.mode = DragMode::NativeAwaitingMovement;
    drag_.nativeCancelRequested = std::make_shared<std::atomic_bool>(false);
    ND_TRACE(L"native move awaiting movement generation=%llu target=%p",
             static_cast<unsigned long long>(drag_.generation),
             drag_.target);
    if (drag_.nativeMovementObserved) {
        RequestNativeMoveStart();
    }
}

void NekoDragApp::RequestNativeMoveStart() {
    if (!drag_.active || drag_.mode != DragMode::NativeAwaitingMovement ||
        drag_.nativeStartMessagePending || mainWindow_ == nullptr) {
        return;
    }
    drag_.nativeStartMessagePending =
        PostMessageW(mainWindow_, kMessageStartNativeMove,
                     static_cast<WPARAM>(drag_.generation),
                     reinterpret_cast<LPARAM>(drag_.target)) != FALSE;
    if (!drag_.nativeStartMessagePending) {
        EndDrag(L"native-start-message-post-failed", false);
    }
}

bool NekoDragApp::IsObservedPrimaryButtonDown() const noexcept {
    // Track the real, non-injected hook pair independently from the async
    // state. Native routing requires both signals because its press is
    // forwarded to Windows; compatibility routing suppresses that press.
    return drag_.active && !drag_.buttonReleased;
}

bool NekoDragApp::IsLogicalPrimaryButtonAsyncDown() const noexcept {
    const PhysicalMouseButton physicalButton = SelectPhysicalPrimaryButton(
        GetSystemMetrics(SM_SWAPBUTTON) != 0);
    const int physicalKey = physicalButton == PhysicalMouseButton::Right
                                ? VK_RBUTTON
                                : VK_LBUTTON;
    return IsKeyDown(physicalKey);
}

void NekoDragApp::HandleNativeMoveStartRequested(
    std::uint64_t generation, HWND target) {
    if (!drag_.active || drag_.mode != DragMode::NativeAwaitingMovement ||
        generation != drag_.generation || target != drag_.target) {
        return;
    }
    drag_.nativeStartMessagePending = false;
    if (drag_.buttonReleased) {
        if (AllowsCompatibilityFallback(drag_.engineMode)) {
            ND_TRACE(L"automatic-fallback generation=%llu "
                     L"reason=released-before-first-movement",
                     static_cast<unsigned long long>(drag_.generation));
            BeginManualFallback(false);
        } else {
            EndDrag(L"native-only-released-before-first-movement");
        }
        return;
    }

    const bool primaryDownObserved = IsObservedPrimaryButtonDown();
    const bool primaryDownAsync = IsLogicalPrimaryButtonAsyncDown();
    const bool primaryReady = primaryDownObserved && primaryDownAsync;
    static_cast<void>(primaryDownAsync);
    ND_TRACE(L"native movement observed generation=%llu target=%p "
             L"observedPrimaryDown=%d asyncPrimaryDown=%d swapped=%d",
             static_cast<unsigned long long>(drag_.generation),
             drag_.target, primaryDownObserved ? 1 : 0,
             primaryDownAsync ? 1 : 0,
             GetSystemMetrics(SM_SWAPBUTTON) != 0 ? 1 : 0);
    if (!ShouldRequestNativeMoveOnMovement(
            true, drag_.nativeStartMessagePending, drag_.buttonReleased,
            primaryReady)) {
        ND_TRACE(L"native move still awaiting button state "
                 L"generation=%llu target=%p",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target);
        return;
    }
    SubmitNativeMoveAttempt(NativeMoveStrategy::NonClientCaption);
}

bool NekoDragApp::SubmitNativeMoveAttempt(NativeMoveStrategy strategy) {
    if (!drag_.active || drag_.buttonReleased ||
        drag_.nativeCancelRequested == nullptr) {
        return false;
    }
    if (drag_.nativeCancelRequested->load(std::memory_order_acquire)) {
        return false;
    }
    if (nativeMoveWorker_ == nullptr || !nativeMoveAvailable_) {
        const bool nativeAttempted = drag_.nativeAttempt != 0;
        if (drag_.nativePressForwarded &&
            !drag_.nativeInteractionCancelSucceeded) {
            if (drag_.engineMode == DragEngineMode::NativeOnly) {
                FailNativeOnlyDrag(L"native-only-worker-unavailable",
                                   ERROR_NOT_READY, nativeAttempted);
            } else {
                EndDrag(L"native-worker-unavailable-after-forwarded-press");
            }
            return false;
        }
        if (AllowsCompatibilityFallback(drag_.engineMode)) {
            BeginManualFallback(nativeAttempted);
        } else {
            FailNativeOnlyDrag(L"native-only-worker-unavailable",
                               ERROR_NOT_READY, nativeAttempted);
        }
        return false;
    }

    std::uint32_t attempt = 0;
    if (strategy == NativeMoveStrategy::NonClientCaption) {
        if (drag_.mode != DragMode::NativeAwaitingMovement ||
            drag_.nativeAttempt != 0) {
            return false;
        }
        attempt = 1;
    } else if (strategy == NativeMoveStrategy::SystemCommand) {
        if (drag_.mode != DragMode::NativeStarting ||
            drag_.nativeStrategy !=
                NativeMoveStrategy::NonClientCaption ||
            drag_.nativeAttempt != 1 || !drag_.nativeMoveReturned) {
            return false;
        }
        attempt = 2;
    } else {
        return false;
    }

    drag_.mode = DragMode::NativeStarting;
    drag_.nativeStrategy = strategy;
    drag_.nativeAttempt = attempt;
    drag_.nativeMoveStarted = false;
    drag_.nativeMoveReturned = false;
    drag_.nativeMoveEndObserved = false;

    const bool cancelInitialInteraction =
        strategy == NativeMoveStrategy::NonClientCaption &&
        drag_.nativePressForwarded &&
        !drag_.nativeInteractionCancelAttempted;
    if (cancelInitialInteraction) {
        drag_.nativeCaptureWindow =
            ResolveTargetCaptureWindow(drag_.target);
        drag_.nativeInteractionCancelAttempted = true;
    }

    drag_.nativeEventToken = ++nativeEventTokenCounter_;
    if (drag_.nativeEventToken == 0) {
        drag_.nativeEventToken = ++nativeEventTokenCounter_;
    }
    nativeEventAttemptStartedAt_.store(
        static_cast<std::uint32_t>(GetTickCount()),
        std::memory_order_release);
    nativeEventToken_.store(drag_.nativeEventToken,
                            std::memory_order_release);

    const NativeMoveWorker::Request request{
        drag_.generation, drag_.target, drag_.latestCursor,
        drag_.nativeCancelRequested, strategy, attempt,
        drag_.initialPressWindow, drag_.nativeCaptureWindow,
        cancelInitialInteraction};
    if (nativeMoveWorker_->Submit(request)) {
        ND_TRACE(L"native move requested generation=%llu target=%p "
                 L"strategy=%ls attempt=%lu cursor=(%ld,%ld) "
                 L"cancelInteraction=%d capture=%p initialPress=%p",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target, NativeMoveStrategyTraceName(strategy),
                 static_cast<unsigned long>(attempt),
                 static_cast<long>(drag_.latestCursor.x),
                 static_cast<long>(drag_.latestCursor.y),
                 cancelInitialInteraction ? 1 : 0,
                 drag_.nativeCaptureWindow,
                 drag_.initialPressWindow);
        return true;
    }

    nativeEventToken_.store(0, std::memory_order_release);
    nativeEventAttemptStartedAt_.store(0, std::memory_order_release);
    SuspendNativeMoveWorker();
    if (cancelInitialInteraction && drag_.nativePressForwarded) {
        ND_TRACE(L"native interaction cleanup was not submitted "
                 L"generation=%llu target=%p",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target);
        EndDrag(L"native-interaction-cleanup-submit-failed");
        return false;
    }
    if (AllowsCompatibilityFallback(drag_.engineMode)) {
        ND_TRACE(L"automatic-fallback generation=%llu "
                 L"reason=native-worker-unavailable strategy=%ls "
                 L"attempt=%lu",
                 static_cast<unsigned long long>(drag_.generation),
                 NativeMoveStrategyTraceName(strategy),
                 static_cast<unsigned long>(attempt));
        BeginManualFallback(attempt != 0);
    } else {
        FailNativeOnlyDrag(L"native-only-worker-unavailable", ERROR_BUSY,
                           attempt != 0);
    }
    return false;
}

void NekoDragApp::BeginManualFallback(bool nativeAttempted) {
    if (!drag_.active || !IsWindow(drag_.target)) {
        EndDrag(L"target-invalid-before-manual-fallback");
        return;
    }
    if (drag_.engineMode == DragEngineMode::NativeOnly) {
        FailNativeOnlyDrag(L"native-only-fallback-blocked",
                           ERROR_NOT_SUPPORTED, nativeAttempted);
        return;
    }
    if (IsHungAppWindow(drag_.target)) {
        ShowTrayNotification(L"NekoDrag",
                             L"目标窗口无响应，已停止当前拖动。",
                             NIIF_WARNING);
        if (nativeAttempted) {
            CompleteNativeDrag(L"native-fallback-target-hung");
        } else {
            EndDrag(L"manual-target-hung");
        }
        return;
    }

    KillTimer(mainWindow_, kNativeFallbackTimer);
    KillTimer(mainWindow_, kNativeCompletionTimer);
    nativeEventToken_.store(0, std::memory_order_release);
    nativeEventAttemptStartedAt_.store(0, std::memory_order_release);

    RECT currentRect{};
    if (!GetWindowRect(drag_.target, &currentRect)) {
        if (nativeAttempted) {
            CompleteNativeDrag(L"native-fallback-target-gone");
        } else {
            EndDrag(L"manual-target-gone");
        }
        return;
    }
    const Point currentOrigin = PointFromNative(
        POINT{currentRect.left, currentRect.top});
    const bool targetMoved =
        currentOrigin.x != drag_.lastAppliedOrigin.x ||
        currentOrigin.y != drag_.lastAppliedOrigin.y;
    if (drag_.nativePressForwarded && drag_.nativeReleaseSuppressed &&
        !drag_.nativeReleaseReplayed) {
        ReplayNativeButtonRelease();
    }
    if (nativeAttempted && targetMoved) {
        drag_.grabOffset = {
            drag_.latestCursor.x - currentOrigin.x,
            drag_.latestCursor.y - currentOrigin.y,
        };
    }
    drag_.lastAppliedOrigin = currentOrigin;
    drag_.lastRequestedOrigin = currentOrigin;
    drag_.moveRequested = false;
    drag_.finalMoveRequested = false;
    drag_.nativeMoveReturned = false;
    drag_.nativeMoveStarted = false;
    drag_.nativeMoveEndObserved = false;
    drag_.nativeReleaseSuppressed = false;
    drag_.nativeReleaseReplayed = false;
    drag_.nativeStartMessagePending = false;
    drag_.nativeAttempt = 0;
    drag_.nativeEventToken = 0;
    drag_.nativeCancelRequested.reset();
    drag_.releasePending = drag_.buttonReleased;
    drag_.mode = DragMode::ManualFallback;
    drag_.restoring = IsZoomed(drag_.target) != FALSE;
    if (drag_.restoring) {
        drag_.maximizedRect = {
            static_cast<std::int32_t>(currentRect.left),
            static_cast<std::int32_t>(currentRect.top),
            static_cast<std::int32_t>(currentRect.right),
            static_cast<std::int32_t>(currentRect.bottom),
        };
        if (!ShowWindowAsync(drag_.target, SW_RESTORE)) {
            FailCurrentDrag(true);
            return;
        }
    }
    if (nativeAttempted) {
        ND_TRACE(L"automatic-fallback generation=%llu "
                 L"reason=native-move-ignored "
                 L"targetMoved=%d",
                 static_cast<unsigned long long>(drag_.generation),
                 targetMoved ? 1 : 0);
    }
    ScheduleDragUpdate();
}

bool NekoDragApp::IsNativeDrag() const noexcept {
    return drag_.mode == DragMode::NativeAwaitingMovement ||
           drag_.mode == DragMode::NativeStarting ||
           drag_.mode == DragMode::NativeActive;
}

bool NekoDragApp::IsNativeMoveAttemptInFlight() const noexcept {
    return drag_.mode == DragMode::NativeStarting ||
           drag_.mode == DragMode::NativeActive;
}

void NekoDragApp::HandleNativeMoveCompleted() {
    if (nativeMoveWorker_ == nullptr) {
        return;
    }
    NativeMoveWorker::Result result;
    if (!nativeMoveWorker_->TakeLatestResult(&result)) {
        return;
    }
    if (!shuttingDown_ && nativeMoveEventHook_ != nullptr) {
        nativeMoveAvailable_ = true;
    }
    const bool currentResult =
        drag_.active && IsNativeMoveAttemptInFlight() &&
        result.generation == drag_.generation &&
        result.target == drag_.target &&
        result.strategy == drag_.nativeStrategy &&
        result.attempt == drag_.nativeAttempt;
    ND_TRACE(L"native move returned generation=%llu target=%p "
             L"strategy=%ls attempt=%lu current=%d dispatched=%d "
             L"releaseObserved=%d cancelAttempted=%d "
             L"cancelSucceeded=%d cancelError=%lu error=%lu "
             L"elapsedUs=%llu",
             static_cast<unsigned long long>(result.generation),
             result.target, NativeMoveStrategyTraceName(result.strategy),
             static_cast<unsigned long>(result.attempt),
             currentResult ? 1 : 0,
             result.dispatched ? 1 : 0,
             currentResult && drag_.buttonReleased ? 1 : 0,
             result.interactionCancelAttempted ? 1 : 0,
             result.interactionCancelSucceeded ? 1 : 0,
             static_cast<unsigned long>(result.interactionCancelError),
             static_cast<unsigned long>(result.error),
             static_cast<unsigned long long>(result.elapsedUs));
    if (!currentResult) {
        return;
    }

    if (result.interactionCancelAttempted) {
        drag_.nativeInteractionCancelAttempted = true;
        drag_.nativeInteractionCancelSucceeded =
            result.interactionCancelSucceeded;
        if (!result.interactionCancelSucceeded) {
            ND_TRACE(L"native interaction cleanup failed generation=%llu "
                     L"target=%p error=%lu",
                     static_cast<unsigned long long>(drag_.generation),
                     drag_.target,
                     static_cast<unsigned long>(
                         result.interactionCancelError));
            if (drag_.engineMode == DragEngineMode::NativeOnly) {
                FailNativeOnlyDrag(
                    L"native-only-interaction-cancel-failed",
                    result.interactionCancelError, true);
            } else {
                EndDrag(L"native-interaction-cancel-failed");
            }
            return;
        }
    }

    drag_.nativeMoveReturned = true;
    KillTimer(mainWindow_, kNativeCompletionTimer);
    if (drag_.nativeMoveStarted && !drag_.nativeMoveEndObserved) {
        // A successful synchronous dispatch returns only after DefWindowProc
        // leaves the move loop. The WinEvent end notification can still be
        // queued behind this completion message, so record the equivalent
        // terminal state here.
        drag_.nativeMoveEndObserved = true;
        ND_TRACE(L"native move ended generation=%llu target=%p "
                 L"strategy=%ls attempt=%lu source=worker-return",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target,
                 NativeMoveStrategyTraceName(drag_.nativeStrategy),
                 static_cast<unsigned long>(drag_.nativeAttempt));
    }
    if (!result.dispatched) {
        if (!drag_.nativeMoveStarted && drag_.buttonReleased) {
            if (AllowsCompatibilityFallback(drag_.engineMode)) {
                BeginManualFallback(true);
            } else {
                CompleteNativeDrag(
                    L"native-only-released-before-start");
            }
            return;
        }
        if (drag_.engineMode == DragEngineMode::NativeOnly) {
            FailNativeOnlyDrag(L"native-only-dispatch-failed",
                               result.error, true);
            return;
        }
        if (result.error == ERROR_ACCESS_DENIED ||
            result.error == ERROR_PRIVILEGE_NOT_HELD) {
            ShowPrivilegeHintOnce();
        } else if (result.error == ERROR_TIMEOUT) {
            ShowTrayNotification(L"NekoDrag",
                                 L"目标窗口无响应，已停止当前拖动。",
                                 NIIF_WARNING);
        }
        BeginManualFallback(true);
        return;
    }

    const NativeMoveCompletionAction action = DecideNativeMoveCompletion(
        drag_.nativeMoveStarted, drag_.buttonReleased, false);
    if (action == NativeMoveCompletionAction::Complete) {
        CompleteNativeDrag(drag_.nativeMoveStarted
                               ? L"native-move-complete"
                               : L"native-move-quick-release");
        return;
    }
    if (action == NativeMoveCompletionAction::UseManualFallback) {
        if (AllowsCompatibilityFallback(drag_.engineMode)) {
            BeginManualFallback(true);
        } else {
            CompleteNativeDrag(L"native-only-released-before-start");
        }
        return;
    }
    if (SetTimer(mainWindow_, kNativeFallbackTimer,
                 kNativeFallbackGraceMs, nullptr) == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        const bool cancelled =
            drag_.nativeCancelRequested != nullptr &&
            drag_.nativeCancelRequested->load(std::memory_order_acquire);
        const bool primaryReady = IsObservedPrimaryButtonDown() &&
                                  IsLogicalPrimaryButtonAsyncDown();
        if (drag_.nativeStrategy ==
                NativeMoveStrategy::NonClientCaption &&
            primaryReady && !cancelled) {
            SubmitNativeMoveAttempt(NativeMoveStrategy::SystemCommand);
            return;
        }
        if (AllowsCompatibilityFallback(drag_.engineMode)) {
            BeginManualFallback(true);
        } else {
            FailNativeOnlyDrag(L"native-only-fallback-timer-failed", error,
                               true);
        }
    }
}

void NekoDragApp::HandleNativeMoveEvent(bool started,
                                         std::uint64_t eventToken,
                                         HWND target) {
    if (!drag_.active || !IsNativeMoveAttemptInFlight() ||
        !IsNativeMoveEventMatch(drag_.nativeEventToken, eventToken,
                                target == drag_.target)) {
        return;
    }
    if (started) {
        drag_.nativeMoveStarted = true;
        drag_.mode = DragMode::NativeActive;
        KillTimer(mainWindow_, kNativeFallbackTimer);
        ND_TRACE(L"native move started generation=%llu target=%p "
                 L"strategy=%ls attempt=%lu eventToken=%llu",
                 static_cast<unsigned long long>(drag_.generation), target,
                 NativeMoveStrategyTraceName(drag_.nativeStrategy),
                 static_cast<unsigned long>(drag_.nativeAttempt),
                 static_cast<unsigned long long>(eventToken));
        if (ShouldReplayNativeButtonRelease(
                drag_.nativeMoveReturned, drag_.nativeMoveStarted,
                drag_.buttonReleased, drag_.nativeReleaseSuppressed,
                drag_.nativeReleaseReplayed,
                eventToken == drag_.nativeEventToken,
                target == drag_.target)) {
            ReplayNativeButtonRelease();
        }
        if (drag_.nativeMoveReturned) {
            CompleteNativeDrag(L"native-move-complete");
        }
        return;
    }

    drag_.nativeMoveEndObserved = true;
    ND_TRACE(L"native move ended generation=%llu target=%p "
             L"strategy=%ls attempt=%lu eventToken=%llu",
             static_cast<unsigned long long>(drag_.generation), target,
             NativeMoveStrategyTraceName(drag_.nativeStrategy),
             static_cast<unsigned long>(drag_.nativeAttempt),
             static_cast<unsigned long long>(eventToken));
}

void NekoDragApp::HandleNativeFallbackTimeout() {
    if (!drag_.active || !IsNativeMoveAttemptInFlight() ||
        !drag_.nativeMoveReturned) {
        return;
    }
    if (drag_.nativeMoveStarted) {
        CompleteNativeDrag(L"native-move-complete-after-grace");
        return;
    }

    const NativeMoveNoStartAction action = DecideNativeMoveNoStartAction(
        drag_.nativeStrategy, drag_.engineMode, drag_.buttonReleased);
    if (action == NativeMoveNoStartAction::TrySystemCommand) {
        const bool primaryDownObserved = IsObservedPrimaryButtonDown();
        const bool primaryDownAsync = IsLogicalPrimaryButtonAsyncDown();
        const bool primaryReady = primaryDownObserved && primaryDownAsync;
        static_cast<void>(primaryDownAsync);
        const bool cancelled =
            drag_.nativeCancelRequested != nullptr &&
            drag_.nativeCancelRequested->load(std::memory_order_acquire);
        ND_TRACE(L"native retry decision generation=%llu target=%p "
                 L"nextStrategy=system-command observedPrimaryDown=%d "
                 L"asyncPrimaryDown=%d cancelled=%d",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target, primaryDownObserved ? 1 : 0,
                 primaryDownAsync ? 1 : 0,
                 cancelled ? 1 : 0);
        if (primaryReady && !cancelled) {
            SubmitNativeMoveAttempt(NativeMoveStrategy::SystemCommand);
            return;
        }
        if (AllowsCompatibilityFallback(drag_.engineMode)) {
            BeginManualFallback(true);
        } else {
            CompleteNativeDrag(L"native-only-primary-button-not-down");
        }
        return;
    }
    if (action == NativeMoveNoStartAction::UseManualFallback) {
        ND_TRACE(L"automatic-fallback generation=%llu "
                 L"reason=native-strategies-not-started",
                 static_cast<unsigned long long>(drag_.generation));
        BeginManualFallback(true);
        return;
    }
    if (action == NativeMoveNoStartAction::Complete) {
        CompleteNativeDrag(L"native-only-released-before-start");
        return;
    }
    FailNativeOnlyDrag(L"native-only-move-not-started",
                       ERROR_NOT_SUPPORTED, true);
}

void NekoDragApp::NoteNativeButtonReleased() {
    if (!drag_.active || !IsNativeDrag() || drag_.buttonReleased) {
        return;
    }
    drag_.buttonReleased = true;
    if (drag_.nativeCancelRequested != nullptr) {
        drag_.nativeCancelRequested->store(true, std::memory_order_release);
    }
    PostMessageW(mainWindow_, kMessageNativeButtonReleased,
                 static_cast<WPARAM>(drag_.generation),
                 reinterpret_cast<LPARAM>(drag_.target));
}

void NekoDragApp::HandleNativeButtonReleased(
    std::uint64_t generation, HWND target) {
    if (!drag_.active || !IsNativeDrag() ||
        generation != drag_.generation || target != drag_.target) {
        return;
    }
    if (SetTimer(mainWindow_, kNativeCompletionTimer,
                 kNativeCompletionTimeoutMs, nullptr) == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_GEN_FAILURE;
        }
        CancelTargetNativeMove();
        SuspendNativeMoveWorker();
        if (drag_.engineMode == DragEngineMode::NativeOnly) {
            FailNativeOnlyDrag(L"native-only-completion-timer-failed",
                               error, true);
        } else {
            CompleteNativeDrag(L"native-completion-timer-failed");
        }
    }
}

bool NekoDragApp::ReplayNativeButtonRelease() {
    if (!drag_.active || !drag_.nativePressForwarded ||
        !drag_.buttonReleased ||
        !drag_.nativeReleaseSuppressed || drag_.nativeReleaseReplayed) {
        return false;
    }

    drag_.nativeReleaseReplayed = true;
    INPUT input{};
    input.type = INPUT_MOUSE;
    const PhysicalMouseButton physicalButton = SelectPhysicalPrimaryButton(
        GetSystemMetrics(SM_SWAPBUTTON) != 0);
    input.mi.dwFlags = physicalButton == PhysicalMouseButton::Right
                           ? MOUSEEVENTF_RIGHTUP
                           : MOUSEEVENTF_LEFTUP;
    input.mi.dwExtraInfo = kNativeReleaseReplayExtraInfo;
    SetLastError(ERROR_SUCCESS);
    if (SendInput(1, &input, static_cast<int>(sizeof(input))) == 1) {
        ND_TRACE(L"native release replayed generation=%llu target=%p",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target);
        return true;
    }

    DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) {
        error = ERROR_ACCESS_DENIED;
    }
    ND_TRACE(L"native release replay failed generation=%llu target=%p "
             L"error=%lu",
             static_cast<unsigned long long>(drag_.generation), drag_.target,
             static_cast<unsigned long>(error));
    CancelTargetNativeMove();
    return false;
}

void NekoDragApp::CancelTargetNativeMove() {
    if (!drag_.active || !IsNativeMoveAttemptInFlight() ||
        drag_.target == nullptr || !IsWindow(drag_.target)) {
        return;
    }
    if (!PostMessageW(drag_.target, WM_CANCELMODE, 0, 0)) {
        ND_TRACE(L"native cancel message failed generation=%llu target=%p "
                 L"error=%lu",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target, static_cast<unsigned long>(GetLastError()));
    }
}

void NekoDragApp::CompleteNativeDrag(const wchar_t* reason) {
    EndDrag(reason);
    RestoreMouseHookAfterNativeDrag();
}

void NekoDragApp::FailNativeOnlyDrag(const wchar_t* reason, DWORD error,
                                      bool nativeAttempted) {
    static_cast<void>(reason);
    ND_TRACE(L"native-only-failed generation=%llu target=%p reason=%ls "
             L"error=%lu",
             static_cast<unsigned long long>(drag_.generation), drag_.target,
             reason != nullptr ? reason : L"unspecified",
             static_cast<unsigned long>(error));
    if (!nativeOnlyFailureNotified_) {
        nativeOnlyFailureNotified_ = true;
        std::wstring message = L"“仅原生（诊断）”模式的原生拖动失败";
        if (error != ERROR_SUCCESS) {
            message.append(L"（错误代码 ");
            message.append(std::to_wstring(error));
            message.append(L"）");
        }
        message.append(
            L"。请在设置中切换到“自动（实验）”或“兼容（推荐）”。");
        ShowTrayNotification(L"NekoDrag", message.c_str(), NIIF_WARNING);
    }
    if (nativeAttempted) {
        CompleteNativeDrag(reason);
    } else {
        EndDrag(reason);
    }
}

void NekoDragApp::RestoreMouseHookAfterNativeDrag() {
    if (shuttingDown_ || !settings_.enabled) {
        return;
    }
    RemoveMouseHook();
    RequestMouseHookInstall(true);
}

void NekoDragApp::ScheduleDragUpdate() {
    if (!drag_.active || drag_.updatePending) {
        return;
    }
    drag_.updatePending =
        PostMessageW(mainWindow_, kMessageUpdateDrag, 0, 0) != FALSE;
    if (!drag_.updatePending) {
        drag_.movementFailed = true;
    }
}

bool NekoDragApp::SubmitLatestDragPosition(bool finalRequest) {
    if (!IsWindow(drag_.target)) {
        ND_TRACE(L"end drag: target window gone");
        EndDrag(L"target-window-gone");
        return false;
    }
    if (moveWorker_ == nullptr) {
        FailCurrentDrag(false, ERROR_INVALID_STATE);
        return false;
    }

    const Point origin =
        ComputeDraggedOrigin(drag_.latestCursor, drag_.grabOffset);
    const bool matchesLastApplied =
        origin.x == drag_.lastAppliedOrigin.x &&
        origin.y == drag_.lastAppliedOrigin.y;
    const bool matchesLastRequested =
        drag_.moveRequested && origin.x == drag_.lastRequestedOrigin.x &&
        origin.y == drag_.lastRequestedOrigin.y;

    if (finalRequest) {
        drag_.finalRequestedOrigin = origin;
        if (matchesLastApplied &&
            (!drag_.moveRequested || matchesLastRequested)) {
            ND_TRACE(L"end drag: final position already applied");
            EndDrag(L"final-already-applied");
            return true;
        }
        drag_.finalMoveRequested = true;
        if (matchesLastRequested) {
            if (SetTimer(mainWindow_, kDragReleaseTimer,
                         kDragReleaseTimeoutMs, nullptr) == 0) {
                ND_TRACE(L"end drag: unable to start release timer");
                EndDrag(L"release-timer-failed");
                return false;
            }
            return true;
        }
    } else if ((!drag_.moveRequested && matchesLastApplied) ||
               matchesLastRequested) {
        return true;
    }

    const WindowMoveWorker::Request request{drag_.generation, drag_.target,
                                            origin};
    if (!moveWorker_->Submit(request)) {
        FailCurrentDrag(false, ERROR_INVALID_STATE);
        return false;
    }
    drag_.moveRequested = true;
    drag_.lastRequestedOrigin = origin;
    ++drag_.manualRequests;

    if (finalRequest &&
        SetTimer(mainWindow_, kDragReleaseTimer, kDragReleaseTimeoutMs,
                 nullptr) == 0) {
        ND_TRACE(L"end drag: unable to start release timer");
        EndDrag(L"release-timer-failed");
        return false;
    }
    return true;
}

void NekoDragApp::HandleMoveCompleted() {
    if (moveWorker_ == nullptr) {
        return;
    }
    WindowMoveWorker::Result result;
    if (!moveWorker_->TakeLatestResult(&result)) {
        return;
    }

    const bool currentResult =
        drag_.active && result.generation == drag_.generation &&
        result.target == drag_.target;
    const bool completesFinalMove =
        currentResult && drag_.releasePending && drag_.finalMoveRequested &&
        result.requestedOrigin.x == drag_.finalRequestedOrigin.x &&
        result.requestedOrigin.y == drag_.finalRequestedOrigin.y;

    if (!currentResult) {
        return;
    }
    ++drag_.manualCompletions;
    drag_.manualCoalescedRequests += result.coalescedRequests;
    drag_.manualMaxElapsedUs =
        std::max(drag_.manualMaxElapsedUs, result.elapsedUs);
    if (!result.success) {
        ND_TRACE(L"manual move failed generation=%llu error=%lu "
                 L"elapsedUs=%llu",
                 static_cast<unsigned long long>(result.generation),
                 static_cast<unsigned long>(result.error),
                 static_cast<unsigned long long>(result.elapsedUs));
        const bool privilegeFailure =
            result.error == ERROR_ACCESS_DENIED ||
            result.error == ERROR_PRIVILEGE_NOT_HELD;
        if (!privilegeFailure) {
            std::wstring message;
            if (result.error == ERROR_TIMEOUT) {
                message = L"目标窗口无响应，已停止当前拖动。";
            } else {
                message = ErrorWithCode(L"无法移动目标窗口", result.error);
            }
            ShowTrayNotification(L"NekoDrag", message.c_str(), NIIF_WARNING);
        }
        FailCurrentDrag(privilegeFailure, result.error);
        return;
    }

    drag_.lastAppliedOrigin = result.actualPositionKnown
                                  ? result.actualOrigin
                                  : result.requestedOrigin;
    if (completesFinalMove) {
        KillTimer(mainWindow_, kDragReleaseTimer);
        EndDrag(L"final-move-complete");
    }
}

void NekoDragApp::ApplyLatestDragPosition() {
    drag_.updatePending = false;
    if (!drag_.active || drag_.mode != DragMode::ManualFallback) {
        return;
    }
    if (!IsWindow(drag_.target)) {
        EndDrag(L"target-window-gone");
        return;
    }
    if (drag_.movementFailed) {
        if (drag_.releasePending) {
            EndDrag(L"move-failed");
        }
        return;
    }

    if (drag_.restoring) {
        if (IsZoomed(drag_.target)) {
            if (++drag_.restoreAttempts >= kMaxRestoreAttempts) {
                FailCurrentDrag(true);
                return;
            }
            SetTimer(mainWindow_, kRestoreTimer, 16, nullptr);
            return;
        }

        RECT restoredRect{};
        if (!GetWindowRect(drag_.target, &restoredRect)) {
            FailCurrentDrag(true);
            return;
        }
        const Size restoredSize{
            static_cast<std::int32_t>(restoredRect.right - restoredRect.left),
            static_cast<std::int32_t>(restoredRect.bottom - restoredRect.top),
        };
        const Point restoredOrigin = ComputeRestoredOrigin(
            drag_.startCursor, drag_.maximizedRect, restoredSize);
        drag_.grabOffset = {drag_.startCursor.x - restoredOrigin.x,
                            drag_.startCursor.y - restoredOrigin.y};
        drag_.lastAppliedOrigin = PointFromNative(
            POINT{restoredRect.left, restoredRect.top});
        drag_.lastRequestedOrigin = drag_.lastAppliedOrigin;
        drag_.moveRequested = false;
        drag_.restoring = false;
        ND_TRACE(L"maximized window restored actual=(%ld,%ld) target=(%ld,%ld)",
                 static_cast<long>(restoredRect.left),
                 static_cast<long>(restoredRect.top),
                 static_cast<long>(restoredOrigin.x),
                 static_cast<long>(restoredOrigin.y));
    }

    if (drag_.releasePending) {
        SubmitLatestDragPosition(true);
    } else {
        SubmitLatestDragPosition(false);
    }
}

void NekoDragApp::EndDrag(const wchar_t* reason, bool trace) {
    static_cast<void>(reason);
    const std::uint64_t generation = drag_.generation;
    if (drag_.active && drag_.nativePressForwarded &&
        drag_.nativeReleaseSuppressed &&
        !drag_.nativeReleaseReplayed) {
        ReplayNativeButtonRelease();
    }
    if (drag_.active && trace) {
        if (drag_.mode == DragMode::ManualFallback) {
            ND_TRACE(L"end manual drag generation=%llu target=%p "
                     L"requests=%llu completions=%llu coalesced=%llu "
                     L"maxElapsedUs=%llu reason=%ls",
                     static_cast<unsigned long long>(generation),
                     drag_.target,
                     static_cast<unsigned long long>(drag_.manualRequests),
                     static_cast<unsigned long long>(
                         drag_.manualCompletions),
                     static_cast<unsigned long long>(
                         drag_.manualCoalescedRequests),
                     static_cast<unsigned long long>(
                         drag_.manualMaxElapsedUs),
                     reason != nullptr ? reason : L"unspecified");
        } else {
            ND_TRACE(L"end native drag generation=%llu target=%p "
                     L"started=%d ended=%d reason=%ls",
                     static_cast<unsigned long long>(generation),
                     drag_.target, drag_.nativeMoveStarted ? 1 : 0,
                     drag_.nativeMoveEndObserved ? 1 : 0,
                     reason != nullptr ? reason : L"unspecified");
        }
    }
    if (mainWindow_ != nullptr) {
        KillTimer(mainWindow_, kRestoreTimer);
        KillTimer(mainWindow_, kDragReleaseTimer);
        KillTimer(mainWindow_, kNativeFallbackTimer);
        KillTimer(mainWindow_, kNativeCompletionTimer);
    }
    nativeEventToken_.store(0, std::memory_order_release);
    nativeEventAttemptStartedAt_.store(0, std::memory_order_release);
    if (drag_.nativeCancelRequested != nullptr) {
        drag_.nativeCancelRequested->store(true, std::memory_order_release);
    }
    if (moveWorker_ != nullptr && generation != 0) {
        moveWorker_->CancelGeneration(generation);
    }
    if (nativeMoveWorker_ != nullptr && generation != 0) {
        nativeMoveWorker_->CancelGeneration(generation);
    }
    drag_ = DragState{};
}

void NekoDragApp::FailCurrentDrag(bool showPrivilegeHint, DWORD error) {
    static_cast<void>(error);
    ND_TRACE(L"drag failed, hint=%d error=%lu",
             showPrivilegeHint ? 1 : 0,
             static_cast<unsigned long>(error));
    drag_.movementFailed = true;
    if (moveWorker_ != nullptr) {
        moveWorker_->CancelGeneration(drag_.generation);
    }
    if (showPrivilegeHint && mainWindow_ != nullptr) {
        PostMessageW(mainWindow_, kMessagePrivilegeHint, 0, 0);
    }
    if (drag_.releasePending) {
        EndDrag(L"move-failed");
    }
}

bool NekoDragApp::AddTrayIcon() {
    if (mainWindow_ == nullptr) {
        return false;
    }

    const UINT dpi = GetDpiForWindow(mainWindow_);
    const int icon_size = MulDiv(16, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    HICON icon = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_TRAYICON), IMAGE_ICON, icon_size,
        icon_size, LR_DEFAULTCOLOR));
    if (icon == nullptr) {
        icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
    }

    NOTIFYICONDATAW iconData{};
    iconData.cbSize = sizeof(iconData);
    iconData.hWnd = mainWindow_;
    iconData.uID = kTrayIconId;
    iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    iconData.uCallbackMessage = kMessageTray;
    iconData.hIcon = icon;
    wcscpy_s(iconData.szTip, L"NekoDrag");
    if (!Shell_NotifyIconW(NIM_ADD, &iconData)) {
        DestroyIcon(icon);
        return false;
    }
    iconData.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &iconData);
    trayIconAdded_ = true;
    DestroyIcon(icon);
    return true;
}

void NekoDragApp::RemoveTrayIcon() {
    if (!trayIconAdded_) {
        return;
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = mainWindow_;
    icon.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    trayIconAdded_ = false;
}

void NekoDragApp::ShowTrayMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    UINT toggleFlags = MF_STRING;
    const wchar_t* toggleLabel = L"启用";
    if (IsMouseHookActive()) {
        toggleFlags |= MF_CHECKED;
        toggleLabel = L"暂停";
    } else if (settings_.enabled &&
               hookRuntimeState_ == HookRuntimeState::Starting) {
        toggleFlags |= MF_GRAYED;
        toggleLabel = L"正在启用…";
    } else if (settings_.enabled) {
        toggleLabel = L"重试启用";
    }
    AppendMenuW(menu, toggleFlags, kMenuToggleEnabled, toggleLabel);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
    bool startupEnabled = false;
    std::wstring ignoredError;
    QueryStartupEnabled(&startupEnabled, nullptr, &ignoredError);
    AppendMenuW(menu, MF_STRING | (startupEnabled ? MF_CHECKED : 0),
                kMenuToggleStartup, L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);

    SetForegroundWindow(mainWindow_);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPoint.x,
        screenPoint.y, 0, mainWindow_, nullptr);
    DestroyMenu(menu);
    PostMessageW(mainWindow_, WM_NULL, 0, 0);

    switch (command) {
        case kMenuToggleEnabled:
            ToggleEnabledFromTray();
            break;
        case kMenuSettings:
            ShowSettingsWindow();
            break;
        case kMenuToggleStartup:
            ToggleStartupFromTray();
            break;
        case kMenuExit:
            DestroyWindow(mainWindow_);
            break;
        default:
            break;
    }
}

void NekoDragApp::ShowTrayNotification(const wchar_t* title,
                                         const wchar_t* message,
                                         DWORD iconFlags) {
    if (!trayIconAdded_) {
        return;
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = mainWindow_;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_INFO;
    wcsncpy_s(icon.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(icon.szInfo, message, _TRUNCATE);
    icon.dwInfoFlags = iconFlags;
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void NekoDragApp::ShowPrivilegeHintOnce() {
    if (settings_.privilegeHintShown) {
        return;
    }
    settings_.privilegeHintShown = true;
    std::wstring ignoredError;
    SaveSettings(settings_, &ignoredError);
    ShowTrayNotification(
        L"无法移动此窗口",
        L"目标窗口可能以管理员权限运行或受到系统保护。NekoDrag 不会请求管理员权限。",
        NIIF_WARNING);
}

void NekoDragApp::ShowSettingsWindow() {
    std::wstring error;
    if (!CreateSettingsWindow(&error)) {
        ShowTrayNotification(L"NekoDrag", error.c_str(), NIIF_ERROR);
        return;
    }
    LoadSettingsIntoControls();
    ShowWindow(settingsWindow_, SW_SHOWNORMAL);
    SetForegroundWindow(settingsWindow_);
}

void NekoDragApp::ApplyThemeToSettingsWindow() {
    if (settingsWindow_ == nullptr || theme_ == nullptr) {
        return;
    }
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif
    const BOOL dark = theme_->IsDark() ? TRUE : FALSE;
    DwmSetWindowAttribute(settingsWindow_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof(dark));
    RedrawWindow(settingsWindow_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW |
                     RDW_ALLCHILDREN);
}

void NekoDragApp::CreateSettingsControls() {
    auto create = [this](DWORD extendedStyle, const wchar_t* className,
                         const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(
            extendedStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0,
            0, 0, 0, settingsWindow_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_,
            nullptr);
    };

    create(0, L"BUTTON", L"启用窗口拖动(&E)",
           BS_OWNERDRAW | WS_TABSTOP, kControlEnabled);
    // The group overlaps its checkbox siblings, so it must not behave like a
    // button or paint into their rectangles.
    create(0, L"STATIC", L"启动快捷键（选择 1 至 3 个）",
           SS_OWNERDRAW | WS_CLIPSIBLINGS, kControlModifierGroup);
    create(0, L"BUTTON", L"Win(&W)",
           BS_OWNERDRAW | WS_TABSTOP, kControlWin);
    create(0, L"BUTTON", L"Ctrl(&C)",
           BS_OWNERDRAW | WS_TABSTOP, kControlControl);
    create(0, L"BUTTON", L"Alt(&A)",
           BS_OWNERDRAW | WS_TABSTOP, kControlAlt);
    create(0, L"BUTTON", L"Shi&ft",
           BS_OWNERDRAW | WS_TABSTOP, kControlShift);
    create(0, L"STATIC",
           L"拖动模式（原生路径为实验功能）",
           SS_OWNERDRAW | WS_CLIPSIBLINGS, kControlDragModeGroup);
    create(0, L"BUTTON", L"兼容（推荐）",
           BS_OWNERDRAW | WS_TABSTOP | WS_GROUP,
           kControlDragModeCompatibility);
    create(0, L"BUTTON", L"自动（实验）",
           BS_OWNERDRAW | WS_TABSTOP, kControlDragModeAutomatic);
    create(0, L"BUTTON", L"仅原生（诊断）",
           BS_OWNERDRAW | WS_TABSTOP, kControlDragModeNative);
    create(0, L"BUTTON", L"登录 Windows 时自动启动(&U)",
           BS_OWNERDRAW | WS_TABSTOP, kControlStartup);
    create(0, L"STATIC",
           L"按住所选修饰键并按下鼠标左键即可拖动。拖动开始后可以松开修饰键。",
           SS_LEFT, kControlHelp);
    create(0, L"STATIC", L"", SS_LEFT, kControlStatus);
    create(0, L"BUTTON", L"保存(&S)",
           BS_OWNERDRAW | WS_TABSTOP, kControlSave);
    create(0, L"BUTTON", L"取消(&X)",
           BS_OWNERDRAW | WS_TABSTOP, kControlCancel);
}

void NekoDragApp::LayoutSettingsControls(UINT dpi) {
    if (settingsWindow_ == nullptr) {
        return;
    }

    RECT clientRect{};
    GetClientRect(settingsWindow_, &clientRect);
    const int client_width = static_cast<int>(clientRect.right);
    const int client_height = static_cast<int>(clientRect.bottom);

    HFONT previous_font = settingsFont_;
    settingsFont_ = nullptr;
    RecreateSettingsFont(dpi);

    using Layout = ui::SettingsLayout;

    const RECT group_rc = Layout::ModifierGroup(dpi, client_width);
    const RECT drag_mode_group_rc =
        Layout::DragModeGroup(dpi, client_width, group_rc.bottom);
    const RECT startup_rc =
        Layout::StartupCheckbox(dpi, drag_mode_group_rc.bottom);
    const RECT help_rc =
        Layout::HelpLabel(dpi, client_width, startup_rc.bottom);
    struct ControlPlacement {
        int id;
        RECT rect;
    };
    const std::array<ControlPlacement, 15> placements{{
        {kControlEnabled, Layout::EnabledCheckbox(dpi)},
        {kControlModifierGroup, group_rc},
        {kControlWin,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 0)},
        {kControlControl,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 1)},
        {kControlAlt,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 2)},
        {kControlShift,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 3)},
        {kControlDragModeGroup, drag_mode_group_rc},
        {kControlDragModeCompatibility,
         Layout::DragModeOption(dpi, drag_mode_group_rc.left,
                                drag_mode_group_rc.top, 0)},
        {kControlDragModeAutomatic,
         Layout::DragModeOption(dpi, drag_mode_group_rc.left,
                                drag_mode_group_rc.top, 1)},
        {kControlDragModeNative,
         Layout::DragModeOption(dpi, drag_mode_group_rc.left,
                                drag_mode_group_rc.top, 2)},
        {kControlStartup, startup_rc},
        {kControlHelp, help_rc},
        {kControlStatus,
         Layout::StatusLabel(dpi, client_width, help_rc.bottom)},
        {kControlSave,
         Layout::SaveButton(dpi, client_width, client_height)},
        {kControlCancel,
         Layout::CancelButton(dpi, client_width, client_height)},
    }};

    HDWP dwp = BeginDeferWindowPos(static_cast<int>(placements.size()));
    for (const ControlPlacement& placement : placements) {
        if (dwp == nullptr) {
            break;
        }
        const HWND control = GetDlgItem(settingsWindow_, placement.id);
        if (control == nullptr) {
            continue;
        }
        const RECT& rc = placement.rect;
        dwp = DeferWindowPos(dwp, control, nullptr, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    }
    bool layoutApplied = false;
    if (dwp != nullptr) {
        layoutApplied = EndDeferWindowPos(dwp) != FALSE;
    }

    for (const ControlPlacement& placement : placements) {
        const HWND control = GetDlgItem(settingsWindow_, placement.id);
        if (control == nullptr) {
            continue;
        }
        if (!layoutApplied) {
            const RECT& rc = placement.rect;
            SetWindowPos(control, nullptr, rc.left, rc.top, rc.right - rc.left,
                         rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    }

    if (previous_font != nullptr) {
        DeleteObject(previous_font);
    }
}

void NekoDragApp::RecreateSettingsFont(UINT dpi) {
    settingsFont_ = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void NekoDragApp::LoadSettingsIntoControls() {
    SetCheckbox(settingsWindow_, kControlEnabled, settings_.enabled);
    SetCheckbox(settingsWindow_, kControlWin,
                (settings_.modifierMask & kModifierWin) != 0);
    SetCheckbox(settingsWindow_, kControlControl,
                (settings_.modifierMask & kModifierControl) != 0);
    SetCheckbox(settingsWindow_, kControlAlt,
                (settings_.modifierMask & kModifierAlt) != 0);
    SetCheckbox(settingsWindow_, kControlShift,
                (settings_.modifierMask & kModifierShift) != 0);
    SetDragModeSelection(settingsWindow_,
                         DragModeControlId(settings_.dragEngineMode));

    bool startupEnabled = false;
    std::wstring error;
    if (!QueryStartupEnabled(&startupEnabled, nullptr, &error)) {
        SetSettingsStatus(error, true);
    } else {
        SetSettingsStatus(L"", false);
        UpdateHookStatusInSettings();
    }
    SetCheckbox(settingsWindow_, kControlStartup, startupEnabled);
}

void NekoDragApp::SetSettingsStatus(const std::wstring& text, bool is_error) {
    hookStatusVisible_ = false;
    settingsStatusError_ = is_error;
    if (settingsWindow_ != nullptr) {
        SetDlgItemTextW(settingsWindow_, kControlStatus, text.c_str());
        HWND status = GetDlgItem(settingsWindow_, kControlStatus);
        if (status != nullptr) {
            InvalidateRect(status, nullptr, TRUE);
        }
    }
}

void NekoDragApp::SetHookStatus(const std::wstring& text, bool isError) {
    SetSettingsStatus(text, isError);
    hookStatusVisible_ = true;
}

void NekoDragApp::UpdateHookStatusInSettings() {
    if (settingsWindow_ == nullptr) {
        return;
    }
    switch (hookRuntimeState_) {
        case HookRuntimeState::Starting:
            if (lastHookErrorText_.empty()) {
                SetHookStatus(L"正在启用窗口拖动…", false);
            } else {
                SetHookStatus(L"正在重试启用窗口拖动；上次失败：" +
                                  lastHookErrorText_,
                              true);
            }
            break;
        case HookRuntimeState::Failed:
            SetHookStatus(lastHookErrorText_ +
                              L"。请从托盘菜单重试启用。",
                          true);
            break;
        case HookRuntimeState::Active:
        case HookRuntimeState::Stopped:
            if (hookStatusVisible_) {
                SetSettingsStatus(L"", false);
            }
            break;
    }
}

void NekoDragApp::SaveSettingsFromControls() {
    UserSettings requested = settings_;
    requested.enabled =
        IsCheckboxChecked(settingsWindow_, kControlEnabled);
    requested.modifierMask = 0;
    if (IsCheckboxChecked(settingsWindow_, kControlWin)) {
        requested.modifierMask |= kModifierWin;
    }
    if (IsCheckboxChecked(settingsWindow_, kControlControl)) {
        requested.modifierMask |= kModifierControl;
    }
    if (IsCheckboxChecked(settingsWindow_, kControlAlt)) {
        requested.modifierMask |= kModifierAlt;
    }
    if (IsCheckboxChecked(settingsWindow_, kControlShift)) {
        requested.modifierMask |= kModifierShift;
    }
    requested.dragEngineMode = SelectedDragEngineMode(settingsWindow_);

    if (!IsValidModifierMask(requested.modifierMask)) {
        SetSettingsStatus(L"请选择 1 至 3 个修饰键。", true);
        return;
    }

    const bool startupEnabled =
        IsCheckboxChecked(settingsWindow_, kControlStartup);
    std::wstring error;
    if (!CommitSettings(requested, startupEnabled, &error)) {
        SetSettingsStatus(error, true);
        return;
    }
    ShowWindow(settingsWindow_, SW_HIDE);
}

bool NekoDragApp::CommitSettings(const UserSettings& requested,
                                  bool startupEnabled,
                                  std::wstring* error) {
    bool oldStartupEnabled = false;
    if (!QueryStartupEnabled(&oldStartupEnabled, nullptr, error)) {
        return false;
    }

    const UserSettings previous = settings_;
    if (!SaveSettings(requested, error)) {
        return false;
    }

    if (startupEnabled != oldStartupEnabled &&
        !SetStartupEnabled(startupEnabled, error)) {
        std::wstring rollbackError;
        if (!SaveSettings(previous, &rollbackError) && error != nullptr) {
            error->append(L"；恢复原设置失败：");
            error->append(rollbackError);
        }
        return false;
    }

    settings_ = requested;
    if (!settings_.enabled) {
        CancelMouseHookRetry();
        EndDrag(L"disabled");
        RemoveMouseHook();
        hookRuntimeState_ = HookRuntimeState::Stopped;
        lastHookError_ = ERROR_SUCCESS;
        lastHookErrorText_.clear();
        UpdateHookStatusInSettings();
    } else if (!IsMouseHookActive()) {
        RequestMouseHookInstall(true);
    }
    return true;
}

void NekoDragApp::ToggleEnabledFromTray() {
    if (settings_.enabled && !IsMouseHookActive()) {
        RequestMouseHookInstall(true);
        return;
    }

    bool startupEnabled = false;
    std::wstring error;
    if (!QueryStartupEnabled(&startupEnabled, nullptr, &error)) {
        ShowTrayNotification(L"NekoDrag", error.c_str(), NIIF_ERROR);
        return;
    }
    UserSettings requested = settings_;
    requested.enabled = !requested.enabled;
    if (!CommitSettings(requested, startupEnabled, &error)) {
        ShowTrayNotification(L"NekoDrag", error.c_str(), NIIF_ERROR);
    }
}

void NekoDragApp::ToggleStartupFromTray() {
    bool startupEnabled = false;
    std::wstring error;
    if (!QueryStartupEnabled(&startupEnabled, nullptr, &error) ||
        !CommitSettings(settings_, !startupEnabled, &error)) {
        ShowTrayNotification(L"NekoDrag", error.c_str(), NIIF_ERROR);
    }
}

}  // namespace nekodrag
