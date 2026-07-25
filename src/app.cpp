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

namespace superdrag {
namespace {

constexpr wchar_t kMainWindowClass[] = L"SuperDrag.HiddenWindow";
constexpr wchar_t kMainWindowTitle[] = L"SuperDrag.MessageWindow";
constexpr wchar_t kSettingsWindowClass[] = L"SuperDrag.SettingsWindow";
constexpr wchar_t kInstanceMutex[] = L"Local\\SuperDrag.SingleInstance";

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
constexpr UINT_PTR kRestoreTimer = 1;
constexpr UINT_PTR kHookRetryTimer = 3;
constexpr UINT_PTR kDragReleaseTimer = 4;
constexpr UINT_PTR kNativeFallbackTimer = 5;
constexpr UINT_PTR kNativeCompletionTimer = 6;
constexpr UINT kDragReleaseTimeoutMs = 500;
constexpr UINT kNativeFallbackGraceMs = 100;
constexpr UINT kNativeCompletionTimeoutMs = 1000;
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

SuperDragApp* gApp = nullptr;

#ifdef SUPERDRAG_TRACE
void TraceDragState(const wchar_t* format, ...) {
    wchar_t message[256]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, std::size(message), _TRUNCATE, format, args);
    va_end(args);

    wchar_t line[288]{};
    swprintf_s(line, std::size(line), L"[SuperDrag] %ls\n", message);
    OutputDebugStringW(line);
}
#define SD_TRACE(...) TraceDragState(__VA_ARGS__)
#else
#define SD_TRACE(...) ((void)0)
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

}  // namespace

SuperDragApp::SuperDragApp(HINSTANCE instance) noexcept : instance_(instance) {
    gApp = this;
}

SuperDragApp::~SuperDragApp() {
    Shutdown();
    if (gApp == this) {
        gApp = nullptr;
    }
}

int SuperDragApp::Run(int) {
    if (!EnsureSingleInstance()) {
        return 0;
    }

    std::wstring error;
    if (!Initialize(&error)) {
        MessageBoxW(nullptr, error.c_str(), L"SuperDrag 启动失败",
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

bool SuperDragApp::EnsureSingleInstance() {
    instanceMutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (instanceMutex_ == nullptr) {
        MessageBoxW(nullptr, L"无法创建单实例锁。", L"SuperDrag 启动失败",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        for (int attempt = 0; attempt < 20; ++attempt) {
            HWND existing = FindWindowW(kMainWindowClass, kMainWindowTitle);
            if (existing != nullptr) {
                PostMessageW(existing, kMessageOpenSettings, 0, 0);
                break;
            }
            Sleep(25);
        }
        return false;
    }

    ReleaseMutex(instanceMutex_);
    return true;
}

bool SuperDragApp::Initialize(std::wstring* error) {
    bool settingsKeyExists = false;
    if (!LoadSettings(&settings_, &settingsKeyExists, error)) {
        return false;
    }
    const bool firstLaunch =
        !settingsKeyExists || !settings_.firstRunCompleted;

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

    std::wstring startupError;
    if (!ReconcileStartupPath(&startupError)) {
        ShowTrayNotification(L"SuperDrag",
                             startupError.empty()
                                 ? L"无法更新开机启动路径。"
                                 : startupError.c_str(),
                             NIIF_WARNING);
    }

    if (firstLaunch) {
        settings_.firstRunCompleted = true;
        std::wstring saveError;
        if (!SaveSettings(settings_, &saveError)) {
            ShowTrayNotification(L"SuperDrag", saveError.c_str(),
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

bool SuperDragApp::StartMoveWorker(std::wstring* error) {
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

void SuperDragApp::InitializeNativeMoveInfrastructure() {
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

bool SuperDragApp::StartNativeMoveWorker() {
    nativeMoveWorker_ = std::make_unique<NativeMoveWorker>(
        mainWindow_, kMessageNativeMoveCompleted);
    if (!nativeMoveWorker_->Start()) {
        nativeMoveWorker_.reset();
        SD_TRACE(L"native move worker failed to start");
        return false;
    }
    return true;
}

bool SuperDragApp::InstallNativeMoveEventHook() {
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
        SD_TRACE(L"native move event hook install failed error=%lu",
                 static_cast<unsigned long>(error));
        static_cast<void>(error);
        return false;
    }
    return true;
}

void SuperDragApp::RemoveNativeMoveEventHook() {
    nativeMoveAvailable_ = false;
    nativeEventGeneration_.store(0, std::memory_order_release);
    nativeEventCompletionWindow_.store(nullptr, std::memory_order_release);
    if (nativeMoveEventHook_ != nullptr) {
        UnhookWinEvent(nativeMoveEventHook_);
        nativeMoveEventHook_ = nullptr;
    }
}

void SuperDragApp::AbandonAndRestartNativeMoveWorker() {
    nativeMoveAvailable_ = false;
    if (nativeMoveWorker_ != nullptr) {
        nativeMoveWorker_->StopAccepting();
        nativeMoveWorker_->Stop(0);
        nativeMoveWorker_.reset();
    }
    if (!shuttingDown_ && nativeMoveEventHook_ != nullptr &&
        StartNativeMoveWorker()) {
        nativeMoveAvailable_ = true;
    }
}

bool SuperDragApp::RegisterWindowClasses(std::wstring* error) {
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

bool SuperDragApp::CreateMainWindow(std::wstring* error) {
    mainWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, kMainWindowClass, kMainWindowTitle, WS_POPUP, 0, 0,
        0, 0, nullptr, nullptr, instance_, this);
    if (mainWindow_ == nullptr) {
        *error = ErrorWithCode(L"创建后台窗口失败", GetLastError());
        return false;
    }
    return true;
}

bool SuperDragApp::CreateSettingsWindow(std::wstring* error) {
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
        extendedStyle, kSettingsWindowClass, L"SuperDrag 设置", style, x, y,
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

void SuperDragApp::Shutdown() {
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
    nativeEventGeneration_.store(0, std::memory_order_release);
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
}

LRESULT CALLBACK SuperDragApp::MainWindowProc(HWND window, UINT message,
                                               WPARAM wParam,
                                               LPARAM lParam) {
    SuperDragApp* app = reinterpret_cast<SuperDragApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<SuperDragApp*>(create->lpCreateParams);
        app->mainWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr
               ? app->OnMainMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK SuperDragApp::SettingsWindowProc(HWND window, UINT message,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
    SuperDragApp* app = reinterpret_cast<SuperDragApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<SuperDragApp*>(create->lpCreateParams);
        app->settingsWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr
               ? app->OnSettingsMessage(window, message, wParam, lParam)
               : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK SuperDragApp::MouseHookProc(int code, WPARAM message,
                                              LPARAM lParam) {
    if (gApp == nullptr) {
        return CallNextHookEx(nullptr, code, message, lParam);
    }
    return gApp->HandleMouseHook(
        code, message, reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam));
}

void CALLBACK SuperDragApp::NativeMoveEventProc(
    HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) {
    if (gApp == nullptr || window == nullptr ||
        (event != EVENT_SYSTEM_MOVESIZESTART &&
         event != EVENT_SYSTEM_MOVESIZEEND)) {
        return;
    }
    const std::uint64_t generation =
        gApp->nativeEventGeneration_.load(std::memory_order_acquire);
    const HWND completionWindow =
        gApp->nativeEventCompletionWindow_.load(std::memory_order_acquire);
    if (generation == 0 || completionWindow == nullptr) {
        return;
    }
    const UINT message = event == EVENT_SYSTEM_MOVESIZESTART
                             ? kMessageNativeMoveStarted
                             : kMessageNativeMoveEnded;
    PostMessageW(completionWindow, message,
                 static_cast<WPARAM>(generation),
                 reinterpret_cast<LPARAM>(window));
}

LRESULT SuperDragApp::OnMainMessage(HWND window, UINT message, WPARAM wParam,
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
                    SD_TRACE(L"end drag: final move timed out");
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
                    SD_TRACE(L"native move completion timed out generation=%llu",
                             static_cast<unsigned long long>(
                                 drag_.generation));
                    AbandonAndRestartNativeMoveWorker();
                    CompleteNativeDrag(L"native-move-timeout");
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

LRESULT SuperDragApp::OnSettingsMessage(HWND window, UINT message,
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
                case kControlModifierGroup:
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

bool SuperDragApp::InstallMouseHook(std::wstring* error, DWORD* errorCode) {
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

void SuperDragApp::RemoveMouseHook() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
}

void SuperDragApp::RequestMouseHookInstall(bool resetRetries) {
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
        SD_TRACE(L"mouse hook install queued");
        return;
    }

    DWORD code = GetLastError();
    if (code == ERROR_SUCCESS) {
        code = ERROR_GEN_FAILURE;
    }
    HandleMouseHookInstallFailure(
        code, ErrorWithCode(L"无法安排全局鼠标监听", code));
}

void SuperDragApp::AttemptMouseHookInstall() {
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
    SD_TRACE(L"installing mouse hook attempt=%llu",
             static_cast<unsigned long long>(hookRetryIndex_ + 1));
    if (InstallMouseHook(&error, &errorCode)) {
        hookRuntimeState_ = HookRuntimeState::Active;
        hookRetryIndex_ = 0;
        lastHookError_ = ERROR_SUCCESS;
        lastHookErrorText_.clear();
        SD_TRACE(L"mouse hook installed handle=%p", mouseHook_);
        UpdateHookStatusInSettings();
        return;
    }
    HandleMouseHookInstallFailure(errorCode, error);
}

void SuperDragApp::HandleMouseHookInstallFailure(
    DWORD errorCode, const std::wstring& error) {
    lastHookError_ = errorCode;
    lastHookErrorText_ = error;
    SD_TRACE(L"mouse hook install failed error=%lu retryIndex=%llu",
             static_cast<unsigned long>(errorCode),
             static_cast<unsigned long long>(hookRetryIndex_));

    if (!shuttingDown_ && settings_.enabled && mainWindow_ != nullptr &&
        hookRetryIndex_ < kHookRetryDelaysMs.size()) {
        const UINT delay = kHookRetryDelaysMs[hookRetryIndex_++];
        if (SetTimer(mainWindow_, kHookRetryTimer, delay, nullptr) != 0) {
            hookRuntimeState_ = HookRuntimeState::Starting;
            SD_TRACE(L"mouse hook retry scheduled delay=%u", delay);
            SetHookStatus(L"正在重试启用窗口拖动；上次失败：" + error,
                          true);
            return;
        }
    }

    hookRuntimeState_ = HookRuntimeState::Failed;
    SetHookStatus(error + L"。请从托盘菜单重试启用。", true);
    ShowTrayNotification(L"SuperDrag", lastHookErrorText_.c_str(),
                         NIIF_ERROR);
}

void SuperDragApp::CancelMouseHookRetry() {
    if (mainWindow_ != nullptr) {
        KillTimer(mainWindow_, kHookRetryTimer);
    }
    hookInstallMessagePending_ = false;
    hookRetryIndex_ = 0;
}

bool SuperDragApp::IsMouseHookActive() const noexcept {
    return hookRuntimeState_ == HookRuntimeState::Active &&
           mouseHook_ != nullptr;
}

std::uint32_t SuperDragApp::CurrentModifierMask() const noexcept {
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

bool SuperDragApp::IsConfiguredChordDown() const noexcept {
    return IsExactModifierMatch(settings_.modifierMask,
                                CurrentModifierMask());
}

HWND SuperDragApp::ResolveDragTarget(POINT cursor, bool* restricted) const {
    *restricted = false;
    HWND target = WindowFromPoint(cursor);
    if (target == nullptr) {
        return nullptr;
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

bool SuperDragApp::IsTargetHigherIntegrity(HWND target) const {
    DWORD targetIntegrity = 0;
    if (!QueryWindowIntegrity(target, &targetIntegrity)) {
        return true;
    }
    return targetIntegrity > ownIntegrityLevel_;
}

bool SuperDragApp::BeginDragFromHook(HWND target, Point cursor) {
    RECT windowRect{};
    if (!GetWindowRect(target, &windowRect)) {
        return false;
    }

    drag_ = DragState{};
    drag_.active = true;
    drag_.beginPending = true;
    drag_.generation = ++dragGenerationCounter_;
    if (drag_.generation == 0) {
        drag_.generation = ++dragGenerationCounter_;
    }
    drag_.target = target;
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

LRESULT SuperDragApp::HandleMouseHook(
    int code, WPARAM message, const MSLLHOOKSTRUCT* mouseInfo) {
    if (code < 0 || mouseInfo == nullptr) {
        return CallNextHookEx(mouseHook_, code, message,
                              reinterpret_cast<LPARAM>(mouseInfo));
    }
    // Ignore synthetic input from other tools so an injected stream cannot
    // fight our drag state machine.
    if ((mouseInfo->flags &
         (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0) {
        return CallNextHookEx(mouseHook_, code, message,
                              reinterpret_cast<LPARAM>(mouseInfo));
    }

    switch (message) {
        case WM_LBUTTONDOWN:
            if (drag_.active) {
                const bool previousGestureReleased =
                    drag_.releasePending || drag_.nativeButtonReleased;
                if (!previousGestureReleased) {
                    return 1;
                }
                // A completed gesture must not consume a subsequent click
                // merely because its final move is still in flight.
                EndDrag(L"new-button-down-after-release", false);
            }
            if (IsMouseHookActive() && IsConfiguredChordDown()) {
                bool restricted = false;
                const HWND target =
                    ResolveDragTarget(mouseInfo->pt, &restricted);
                if (restricted) {
                    PostMessageW(mainWindow_, kMessagePrivilegeHint, 0, 0);
                } else if (target != nullptr &&
                           BeginDragFromHook(target,
                                             PointFromNative(mouseInfo->pt))) {
                    return 1;
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (drag_.active) {
                drag_.latestCursor = PointFromNative(mouseInfo->pt);
                if (drag_.mode == DragMode::ManualFallback &&
                    !drag_.releasePending) {
                    ScheduleDragUpdate();
                }
                // The low-level hook runs before Windows applies the pointer
                // movement. Suppressing WM_MOUSEMOVE keeps the system cursor
                // near its previous position, so later absolute hook points
                // oscillate around the drag origin and the window jitters.
                // The button-down was already suppressed, so forwarding only
                // movement updates the cursor without starting a target
                // control interaction.
                break;
            }
            break;
        case WM_LBUTTONUP:
            if (drag_.active) {
                drag_.latestCursor = PointFromNative(mouseInfo->pt);
                if (IsNativeDrag()) {
                    NoteNativeButtonReleased();
                    break;
                }
                if (drag_.mode == DragMode::ManualFallback) {
                    drag_.releasePending = true;
                    ScheduleDragUpdate();
                    return 1;
                }
            }
            break;
        default:
            break;
    }
    return CallNextHookEx(mouseHook_, code, message,
                          reinterpret_cast<LPARAM>(mouseInfo));
}

void SuperDragApp::BeginDragOnMessageThread() {
    if (!drag_.active || !drag_.beginPending ||
        !IsWindow(drag_.target)) {
        EndDrag(L"target-invalid-before-begin");
        return;
    }
    drag_.beginPending = false;
    SetForegroundWindow(drag_.target);
    SD_TRACE(L"begin drag generation=%llu target=%p cursor=(%ld,%ld) "
             L"maximized=%d nativeAvailable=%d",
             static_cast<unsigned long long>(drag_.generation), drag_.target,
             static_cast<long>(drag_.startCursor.x),
             static_cast<long>(drag_.startCursor.y),
             drag_.restoring ? 1 : 0, nativeMoveAvailable_ ? 1 : 0);

    if (nativeMoveAvailable_ && nativeMoveWorker_ != nullptr) {
        drag_.mode = DragMode::NativeStarting;
        nativeEventGeneration_.store(drag_.generation,
                                     std::memory_order_release);
        const NativeMoveWorker::Request request{
            drag_.generation, drag_.target, drag_.startCursor};
        if (nativeMoveWorker_->Submit(request)) {
            SD_TRACE(L"native move requested generation=%llu target=%p",
                     static_cast<unsigned long long>(drag_.generation),
                     drag_.target);
            return;
        }
        AbandonAndRestartNativeMoveWorker();
        if (nativeMoveAvailable_ && nativeMoveWorker_ != nullptr &&
            nativeMoveWorker_->Submit(request)) {
            SD_TRACE(L"native move worker replaced and request retried "
                     L"generation=%llu target=%p",
                     static_cast<unsigned long long>(drag_.generation),
                     drag_.target);
            return;
        }
        nativeEventGeneration_.store(0, std::memory_order_release);
        SD_TRACE(L"native move worker busy, using manual fallback");
    }

    BeginManualFallback(false);
}

void SuperDragApp::BeginManualFallback(bool nativeAttempted) {
    if (!drag_.active || !IsWindow(drag_.target)) {
        EndDrag(L"target-invalid-before-manual-fallback");
        return;
    }
    if (IsHungAppWindow(drag_.target)) {
        ShowTrayNotification(L"SuperDrag",
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
    nativeEventGeneration_.store(0, std::memory_order_release);

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
    drag_.nativeButtonReleased = false;
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
        SD_TRACE(L"native move ignored; manual fallback generation=%llu "
                 L"targetMoved=%d",
                 static_cast<unsigned long long>(drag_.generation),
                 targetMoved ? 1 : 0);
    }
    ScheduleDragUpdate();
}

bool SuperDragApp::IsNativeDrag() const noexcept {
    return drag_.mode == DragMode::NativeStarting ||
           drag_.mode == DragMode::NativeActive;
}

void SuperDragApp::HandleNativeMoveCompleted() {
    if (nativeMoveWorker_ == nullptr) {
        return;
    }
    NativeMoveWorker::Result result;
    if (!nativeMoveWorker_->TakeLatestResult(&result)) {
        return;
    }
    const bool currentResult =
        drag_.active && IsNativeDrag() &&
        result.generation == drag_.generation &&
        result.target == drag_.target;
    SD_TRACE(L"native move returned generation=%llu target=%p "
             L"current=%d dispatched=%d releaseObserved=%d error=%lu "
             L"elapsedUs=%llu",
             static_cast<unsigned long long>(result.generation),
             result.target, currentResult ? 1 : 0,
             result.dispatched ? 1 : 0,
             currentResult && drag_.nativeButtonReleased ? 1 : 0,
             static_cast<unsigned long>(result.error),
             static_cast<unsigned long long>(result.elapsedUs));
    if (!currentResult) {
        return;
    }

    drag_.nativeMoveReturned = true;
    KillTimer(mainWindow_, kNativeCompletionTimer);
    if (drag_.nativeMoveStarted && !drag_.nativeMoveEndObserved) {
        // SendMessage returns only after DefWindowProc leaves the move loop.
        // The WinEvent end notification can still be queued behind this
        // completion message, so record the equivalent terminal state here.
        drag_.nativeMoveEndObserved = true;
        SD_TRACE(L"native move ended generation=%llu target=%p "
                 L"source=worker-return",
                 static_cast<unsigned long long>(drag_.generation),
                 drag_.target);
    }
    if (!result.dispatched) {
        if (result.error == ERROR_ACCESS_DENIED ||
            result.error == ERROR_PRIVILEGE_NOT_HELD) {
            ShowPrivilegeHintOnce();
        } else if (result.error == ERROR_TIMEOUT) {
            ShowTrayNotification(L"SuperDrag",
                                 L"目标窗口无响应，已停止当前拖动。",
                                 NIIF_WARNING);
        }
        CompleteNativeDrag(result.error == ERROR_CANCELLED
                               ? L"native-move-button-released-before-start"
                               : L"native-move-dispatch-failed");
        return;
    }

    const NativeMoveCompletionAction action = DecideNativeMoveCompletion(
        drag_.nativeMoveStarted, drag_.nativeButtonReleased, false);
    if (action == NativeMoveCompletionAction::Complete) {
        CompleteNativeDrag(drag_.nativeMoveStarted
                               ? L"native-move-complete"
                               : L"native-move-quick-release");
        return;
    }
    if (SetTimer(mainWindow_, kNativeFallbackTimer,
                 kNativeFallbackGraceMs, nullptr) == 0) {
        BeginManualFallback(true);
    }
}

void SuperDragApp::HandleNativeMoveEvent(bool started,
                                         std::uint64_t generation,
                                         HWND target) {
    if (!drag_.active || !IsNativeDrag() ||
        generation != drag_.generation || target != drag_.target) {
        return;
    }
    if (started) {
        drag_.nativeMoveStarted = true;
        drag_.mode = DragMode::NativeActive;
        KillTimer(mainWindow_, kNativeFallbackTimer);
        SD_TRACE(L"native move started generation=%llu target=%p",
                 static_cast<unsigned long long>(generation), target);
        if (drag_.nativeMoveReturned) {
            CompleteNativeDrag(L"native-move-complete");
        }
        return;
    }

    drag_.nativeMoveEndObserved = true;
    SD_TRACE(L"native move ended generation=%llu target=%p",
             static_cast<unsigned long long>(generation), target);
}

void SuperDragApp::HandleNativeFallbackTimeout() {
    if (!drag_.active || !IsNativeDrag() || !drag_.nativeMoveReturned) {
        return;
    }
    const NativeMoveCompletionAction action = DecideNativeMoveCompletion(
        drag_.nativeMoveStarted, drag_.nativeButtonReleased, true);
    if (action == NativeMoveCompletionAction::UseManualFallback) {
        BeginManualFallback(true);
    } else {
        CompleteNativeDrag(L"native-move-complete-after-grace");
    }
}

void SuperDragApp::NoteNativeButtonReleased() {
    if (!drag_.active || !IsNativeDrag() || drag_.nativeButtonReleased) {
        return;
    }
    drag_.nativeButtonReleased = true;
    PostMessageW(mainWindow_, kMessageNativeButtonReleased,
                 static_cast<WPARAM>(drag_.generation),
                 reinterpret_cast<LPARAM>(drag_.target));
}

void SuperDragApp::HandleNativeButtonReleased(
    std::uint64_t generation, HWND target) {
    if (!drag_.active || !IsNativeDrag() ||
        generation != drag_.generation || target != drag_.target) {
        return;
    }
    if (SetTimer(mainWindow_, kNativeCompletionTimer,
                 kNativeCompletionTimeoutMs, nullptr) == 0) {
        AbandonAndRestartNativeMoveWorker();
        CompleteNativeDrag(L"native-completion-timer-failed");
    }
}

void SuperDragApp::CompleteNativeDrag(const wchar_t* reason) {
    EndDrag(reason);
    RestoreMouseHookAfterNativeDrag();
}

void SuperDragApp::RestoreMouseHookAfterNativeDrag() {
    if (shuttingDown_ || !settings_.enabled) {
        return;
    }
    RemoveMouseHook();
    RequestMouseHookInstall(true);
}

void SuperDragApp::ScheduleDragUpdate() {
    if (!drag_.active || drag_.updatePending) {
        return;
    }
    drag_.updatePending =
        PostMessageW(mainWindow_, kMessageUpdateDrag, 0, 0) != FALSE;
    if (!drag_.updatePending) {
        drag_.movementFailed = true;
    }
}

bool SuperDragApp::SubmitLatestDragPosition(bool finalRequest) {
    if (!IsWindow(drag_.target)) {
        SD_TRACE(L"end drag: target window gone");
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
            SD_TRACE(L"end drag: final position already applied");
            EndDrag(L"final-already-applied");
            return true;
        }
        drag_.finalMoveRequested = true;
        if (matchesLastRequested) {
            if (SetTimer(mainWindow_, kDragReleaseTimer,
                         kDragReleaseTimeoutMs, nullptr) == 0) {
                SD_TRACE(L"end drag: unable to start release timer");
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
        SD_TRACE(L"end drag: unable to start release timer");
        EndDrag(L"release-timer-failed");
        return false;
    }
    return true;
}

void SuperDragApp::HandleMoveCompleted() {
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
        SD_TRACE(L"manual move failed generation=%llu error=%lu "
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
            ShowTrayNotification(L"SuperDrag", message.c_str(), NIIF_WARNING);
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

void SuperDragApp::ApplyLatestDragPosition() {
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
        SD_TRACE(L"maximized window restored actual=(%ld,%ld) target=(%ld,%ld)",
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

void SuperDragApp::EndDrag(const wchar_t* reason, bool trace) {
    static_cast<void>(reason);
    const std::uint64_t generation = drag_.generation;
    if (drag_.active && trace) {
        if (drag_.mode == DragMode::ManualFallback) {
            SD_TRACE(L"end manual drag generation=%llu target=%p "
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
            SD_TRACE(L"end native drag generation=%llu target=%p "
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
    nativeEventGeneration_.store(0, std::memory_order_release);
    if (moveWorker_ != nullptr && generation != 0) {
        moveWorker_->CancelGeneration(generation);
    }
    if (nativeMoveWorker_ != nullptr && generation != 0) {
        nativeMoveWorker_->CancelGeneration(generation);
    }
    drag_ = DragState{};
}

void SuperDragApp::FailCurrentDrag(bool showPrivilegeHint, DWORD error) {
    static_cast<void>(error);
    SD_TRACE(L"drag failed, hint=%d error=%lu",
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

bool SuperDragApp::AddTrayIcon() {
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
    wcscpy_s(iconData.szTip, L"SuperDrag");
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

void SuperDragApp::RemoveTrayIcon() {
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

void SuperDragApp::ShowTrayMenu(POINT screenPoint) {
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

void SuperDragApp::ShowTrayNotification(const wchar_t* title,
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

void SuperDragApp::ShowPrivilegeHintOnce() {
    if (settings_.privilegeHintShown) {
        return;
    }
    settings_.privilegeHintShown = true;
    std::wstring ignoredError;
    SaveSettings(settings_, &ignoredError);
    ShowTrayNotification(
        L"无法移动此窗口",
        L"目标窗口可能以管理员权限运行或受到系统保护。SuperDrag 不会请求管理员权限。",
        NIIF_WARNING);
}

void SuperDragApp::ShowSettingsWindow() {
    std::wstring error;
    if (!CreateSettingsWindow(&error)) {
        ShowTrayNotification(L"SuperDrag", error.c_str(), NIIF_ERROR);
        return;
    }
    LoadSettingsIntoControls();
    ShowWindow(settingsWindow_, SW_SHOWNORMAL);
    SetForegroundWindow(settingsWindow_);
}

void SuperDragApp::ApplyThemeToSettingsWindow() {
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

void SuperDragApp::CreateSettingsControls() {
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

void SuperDragApp::LayoutSettingsControls(UINT dpi) {
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
    const RECT startup_rc = Layout::StartupCheckbox(dpi, group_rc.bottom);
    const RECT help_rc =
        Layout::HelpLabel(dpi, client_width, startup_rc.bottom);
    struct ControlPlacement {
        int id;
        RECT rect;
    };
    const std::array<ControlPlacement, 11> placements{{
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

void SuperDragApp::RecreateSettingsFont(UINT dpi) {
    settingsFont_ = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SuperDragApp::LoadSettingsIntoControls() {
    SetCheckbox(settingsWindow_, kControlEnabled, settings_.enabled);
    SetCheckbox(settingsWindow_, kControlWin,
                (settings_.modifierMask & kModifierWin) != 0);
    SetCheckbox(settingsWindow_, kControlControl,
                (settings_.modifierMask & kModifierControl) != 0);
    SetCheckbox(settingsWindow_, kControlAlt,
                (settings_.modifierMask & kModifierAlt) != 0);
    SetCheckbox(settingsWindow_, kControlShift,
                (settings_.modifierMask & kModifierShift) != 0);

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

void SuperDragApp::SetSettingsStatus(const std::wstring& text, bool is_error) {
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

void SuperDragApp::SetHookStatus(const std::wstring& text, bool isError) {
    SetSettingsStatus(text, isError);
    hookStatusVisible_ = true;
}

void SuperDragApp::UpdateHookStatusInSettings() {
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

void SuperDragApp::SaveSettingsFromControls() {
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

bool SuperDragApp::CommitSettings(const UserSettings& requested,
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

void SuperDragApp::ToggleEnabledFromTray() {
    if (settings_.enabled && !IsMouseHookActive()) {
        RequestMouseHookInstall(true);
        return;
    }

    bool startupEnabled = false;
    std::wstring error;
    if (!QueryStartupEnabled(&startupEnabled, nullptr, &error)) {
        ShowTrayNotification(L"SuperDrag", error.c_str(), NIIF_ERROR);
        return;
    }
    UserSettings requested = settings_;
    requested.enabled = !requested.enabled;
    if (!CommitSettings(requested, startupEnabled, &error)) {
        ShowTrayNotification(L"SuperDrag", error.c_str(), NIIF_ERROR);
    }
}

void SuperDragApp::ToggleStartupFromTray() {
    bool startupEnabled = false;
    std::wstring error;
    if (!QueryStartupEnabled(&startupEnabled, nullptr, &error) ||
        !CommitSettings(settings_, !startupEnabled, &error)) {
        ShowTrayNotification(L"SuperDrag", error.c_str(), NIIF_ERROR);
    }
}

}  // namespace superdrag
