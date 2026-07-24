#include "app.h"

#include "layout.h"
#include "resource.h"
#include "settings_store.h"
#include "ui_theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
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
constexpr UINT_PTR kRestoreTimer = 1;
constexpr UINT_PTR kDragWatchdogTimer = 2;
constexpr UINT kDragWatchdogIntervalMs = 500;
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
    wchar_t buffer[256];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    OutputDebugStringW(L"[SuperDrag] ");
    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
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
    if (!RegisterWindowClasses(error) || !CreateMainWindow(error)) {
        return false;
    }
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
        std::wstring hookError;
        if (!InstallMouseHook(&hookError)) {
            settings_.enabled = false;
            ShowTrayNotification(L"SuperDrag", hookError.c_str(), NIIF_ERROR);
        }
    }

    if (firstLaunch) {
        ShowSettingsWindow();
    }
    return true;
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

    const UINT dpi = GetDpiForSystem();
    const int client_width = ui::SettingsLayout::Scale(
        ui::SettingsLayout::kMinClientWidth, dpi);
    const int client_height = ui::SettingsLayout::Scale(
        ui::SettingsLayout::kMinClientHeight, dpi);
    RECT windowRect{0, 0, client_width, client_height};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_THICKFRAME | WS_CLIPCHILDREN;
    const DWORD extendedStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectExForDpi(&windowRect, style, FALSE, extendedStyle, dpi);

    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = monitorInfo.rcWork.left +
                  (monitorInfo.rcWork.right - monitorInfo.rcWork.left - width) /
                      2;
    const int y = monitorInfo.rcWork.top +
                  (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - height) /
                      2;

    settingsWindow_ = CreateWindowExW(
        extendedStyle, kSettingsWindowClass, L"SuperDrag 设置", style, x, y,
        width, height, mainWindow_, nullptr, instance_, this);
    if (settingsWindow_ == nullptr) {
        *error = ErrorWithCode(L"创建设置窗口失败", GetLastError());
        return false;
    }
    return true;
}

void SuperDragApp::Shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;

    EndDrag();
    RemoveMouseHook();
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
        case kMessageOpenSettings:
            ShowSettingsWindow();
            return 0;
        case kMessagePrivilegeHint:
            ShowPrivilegeHintOnce();
            return 0;
        case WM_TIMER:
            if (wParam == kRestoreTimer) {
                KillTimer(window, kRestoreTimer);
                ScheduleDragUpdate();
                return 0;
            }
            if (wParam == kDragWatchdogTimer) {
                // Self-healing: if the physical button was released but the
                // release event never reached the hook (e.g. the hook was
                // skipped after a timeout), end the drag instead of
                // swallowing clicks forever.
                if (drag_.active && !IsKeyDown(VK_LBUTTON)) {
                    SD_TRACE(L"watchdog: button already up, ending drag");
                    EndDrag();
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
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
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

bool SuperDragApp::InstallMouseHook(std::wstring* error) {
    if (mouseHook_ != nullptr) {
        return true;
    }
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance_, 0);
    if (mouseHook_ == nullptr) {
        *error = ErrorWithCode(L"无法启用全局鼠标监听", GetLastError());
        return false;
    }
    return true;
}

void SuperDragApp::RemoveMouseHook() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
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
    drag_.target = target;
    drag_.startCursor = cursor;
    drag_.latestCursor = cursor;
    drag_.grabOffset = {cursor.x - windowRect.left,
                        cursor.y - windowRect.top};
    drag_.lastAppliedOrigin = {windowRect.left, windowRect.top};
    drag_.maximizedRect = {windowRect.left, windowRect.top, windowRect.right,
                           windowRect.bottom};
    drag_.restoring = IsZoomed(target) != FALSE;

    if (!PostMessageW(mainWindow_, kMessageBeginDrag, 0, 0)) {
        drag_ = DragState{};
        return false;
    }
    SD_TRACE(L"begin drag target=%p cursor=(%ld,%ld) maximized=%d", target,
             static_cast<long>(cursor.x), static_cast<long>(cursor.y),
             drag_.restoring ? 1 : 0);
    SetTimer(mainWindow_, kDragWatchdogTimer, kDragWatchdogIntervalMs,
             nullptr);
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
                return 1;
            }
            if (settings_.enabled && IsConfiguredChordDown()) {
                bool restricted = false;
                const HWND target =
                    ResolveDragTarget(mouseInfo->pt, &restricted);
                if (restricted) {
                    PostMessageW(mainWindow_, kMessagePrivilegeHint, 0, 0);
                } else if (target != nullptr &&
                           BeginDragFromHook(
                               target, {mouseInfo->pt.x, mouseInfo->pt.y})) {
                    return 1;
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (drag_.active) {
                drag_.latestCursor = {mouseInfo->pt.x, mouseInfo->pt.y};
                if (IsDragPositionReady()) {
                    // Apply immediately at input rate. The async request
                    // never blocks this thread on a busy target, and the
                    // per-target FIFO queue keeps positions ordered.
                    MoveTargetToLatestCursor();
                } else {
                    ScheduleDragUpdate();
                }
                return 1;
            }
            break;
        case WM_LBUTTONUP:
            if (drag_.active) {
                drag_.latestCursor = {mouseInfo->pt.x, mouseInfo->pt.y};
                if (IsDragPositionReady()) {
                    MoveTargetToLatestCursor();
                    SD_TRACE(L"end drag (button up) at (%ld,%ld)",
                             static_cast<long>(mouseInfo->pt.x),
                             static_cast<long>(mouseInfo->pt.y));
                    EndDrag();
                } else {
                    drag_.releasePending = true;
                    ScheduleDragUpdate();
                }
                return 1;
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
        EndDrag();
        return;
    }
    drag_.beginPending = false;
    SetForegroundWindow(drag_.target);

    if (drag_.restoring && !ShowWindowAsync(drag_.target, SW_RESTORE)) {
        FailCurrentDrag(true);
        return;
    }
    ScheduleDragUpdate();
}

void SuperDragApp::ScheduleDragUpdate() {
    if (!drag_.active || drag_.updatePending) {
        return;
    }
    drag_.updatePending =
        PostMessageW(mainWindow_, kMessageUpdateDrag, 0, 0) != FALSE;
    if (!drag_.updatePending) {
        FailCurrentDrag(false);
    }
}

bool SuperDragApp::IsDragPositionReady() const noexcept {
    return drag_.active && !drag_.beginPending && !drag_.restoring &&
           !drag_.releasePending && !drag_.movementFailed;
}

void SuperDragApp::MoveTargetToLatestCursor() {
    if (!IsWindow(drag_.target)) {
        SD_TRACE(L"end drag: target window gone");
        EndDrag();
        return;
    }
    const Point origin =
        ComputeDraggedOrigin(drag_.latestCursor, drag_.grabOffset);
    if (origin.x == drag_.lastAppliedOrigin.x &&
        origin.y == drag_.lastAppliedOrigin.y) {
        return;
    }
    if (!SetWindowPos(drag_.target, nullptr, origin.x, origin.y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS |
                          SWP_DEFERERASE)) {
        SD_TRACE(L"SetWindowPos failed, error=%lu", GetLastError());
        FailCurrentDrag(true);
        return;
    }
    drag_.lastAppliedOrigin = origin;
#ifdef SUPERDRAG_TRACE
    const DWORD now = GetTickCount();
    if (now - lastMoveTraceTick_ >= 100) {
        lastMoveTraceTick_ = now;
        SD_TRACE(L"move cursor=(%ld,%ld) origin=(%ld,%ld)",
                 static_cast<long>(drag_.latestCursor.x),
                 static_cast<long>(drag_.latestCursor.y),
                 static_cast<long>(origin.x), static_cast<long>(origin.y));
    }
#endif
}

void SuperDragApp::ApplyLatestDragPosition() {
    drag_.updatePending = false;
    if (!drag_.active) {
        return;
    }
    if (!IsWindow(drag_.target)) {
        EndDrag();
        return;
    }
    if (drag_.movementFailed) {
        if (drag_.releasePending) {
            EndDrag();
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
        const Size restoredSize{restoredRect.right - restoredRect.left,
                                restoredRect.bottom - restoredRect.top};
        const Point restoredOrigin = ComputeRestoredOrigin(
            drag_.startCursor, drag_.maximizedRect, restoredSize);
        drag_.grabOffset = {drag_.startCursor.x - restoredOrigin.x,
                            drag_.startCursor.y - restoredOrigin.y};
        drag_.lastAppliedOrigin = restoredOrigin;
        drag_.restoring = false;
        SD_TRACE(L"maximized window restored at (%ld,%ld)",
                 static_cast<long>(restoredOrigin.x),
                 static_cast<long>(restoredOrigin.y));
    }

    MoveTargetToLatestCursor();

    if (drag_.releasePending) {
        SD_TRACE(L"end drag (pending release) at (%ld,%ld)",
                 static_cast<long>(drag_.latestCursor.x),
                 static_cast<long>(drag_.latestCursor.y));
        EndDrag();
    }
}

void SuperDragApp::EndDrag() {
    if (mainWindow_ != nullptr) {
        KillTimer(mainWindow_, kRestoreTimer);
        KillTimer(mainWindow_, kDragWatchdogTimer);
    }
    drag_ = DragState{};
}

void SuperDragApp::FailCurrentDrag(bool showPrivilegeHint) {
    SD_TRACE(L"drag failed, hint=%d", showPrivilegeHint ? 1 : 0);
    drag_.movementFailed = true;
    if (showPrivilegeHint && mainWindow_ != nullptr) {
        PostMessageW(mainWindow_, kMessagePrivilegeHint, 0, 0);
    }
    if (drag_.releasePending) {
        EndDrag();
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

    AppendMenuW(menu, MF_STRING | (settings_.enabled ? MF_CHECKED : 0),
                kMenuToggleEnabled,
                settings_.enabled ? L"暂停" : L"启用");
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
           BS_OWNERDRAW | BS_DEFPUSHBUTTON | WS_TABSTOP, kControlSave);
    create(0, L"BUTTON", L"取消(&X)",
           BS_OWNERDRAW | BS_PUSHBUTTON | WS_TABSTOP, kControlCancel);
}

void SuperDragApp::LayoutSettingsControls(UINT dpi) {
    if (settingsWindow_ == nullptr) {
        return;
    }

    RECT clientRect{};
    GetClientRect(settingsWindow_, &clientRect);
    const int client_width = clientRect.right;
    const int client_height = clientRect.bottom;

    HFONT previous_font = settingsFont_;
    settingsFont_ = nullptr;
    RecreateSettingsFont(dpi);

    using Layout = ui::SettingsLayout;

    HDWP dwp = BeginDeferWindowPos(11);
    auto move = [&](int id, const RECT& rc) {
        HWND control = GetDlgItem(settingsWindow_, id);
        DeferWindowPos(dwp, control, nullptr, rc.left, rc.top,
                       rc.right - rc.left, rc.bottom - rc.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(settingsFont_), TRUE);
    };

    move(kControlEnabled, Layout::EnabledCheckbox(dpi));
    const RECT group_rc = Layout::ModifierGroup(dpi, client_width);
    move(kControlModifierGroup, group_rc);
    move(kControlWin,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 0));
    move(kControlControl,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 1));
    move(kControlAlt,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 2));
    move(kControlShift,
         Layout::ModifierCheckbox(dpi, group_rc.left, group_rc.top, 3));
    const RECT startup_rc = Layout::StartupCheckbox(dpi, group_rc.bottom);
    move(kControlStartup, startup_rc);
    const RECT help_rc =
        Layout::HelpLabel(dpi, client_width, startup_rc.bottom);
    move(kControlHelp, help_rc);
    move(kControlStatus,
         Layout::StatusLabel(dpi, client_width, help_rc.bottom));
    move(kControlSave,
         Layout::SaveButton(dpi, client_width, client_height));
    move(kControlCancel,
         Layout::CancelButton(dpi, client_width, client_height));

    EndDeferWindowPos(dwp);

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
    }
    SetCheckbox(settingsWindow_, kControlStartup, startupEnabled);
}

void SuperDragApp::SetSettingsStatus(const std::wstring& text, bool is_error) {
    settingsStatusError_ = is_error;
    if (settingsWindow_ != nullptr) {
        SetDlgItemTextW(settingsWindow_, kControlStatus, text.c_str());
        HWND status = GetDlgItem(settingsWindow_, kControlStatus);
        if (status != nullptr) {
            InvalidateRect(status, nullptr, TRUE);
        }
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

    const bool hookWasInstalled = mouseHook_ != nullptr;
    if (requested.enabled && !hookWasInstalled &&
        !InstallMouseHook(error)) {
        return false;
    }

    const UserSettings previous = settings_;
    if (!SaveSettings(requested, error)) {
        if (!hookWasInstalled) {
            RemoveMouseHook();
        }
        return false;
    }

    if (startupEnabled != oldStartupEnabled &&
        !SetStartupEnabled(startupEnabled, error)) {
        std::wstring ignoredError;
        SaveSettings(previous, &ignoredError);
        if (!hookWasInstalled) {
            RemoveMouseHook();
        }
        return false;
    }

    settings_ = requested;
    if (!settings_.enabled) {
        EndDrag();
        RemoveMouseHook();
    }
    return true;
}

void SuperDragApp::ToggleEnabledFromTray() {
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
