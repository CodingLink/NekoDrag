#pragma once

#include "core.h"
#include "ui_theme.h"
#include "window_move_worker.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace superdrag {

class SuperDragApp {
  public:
    explicit SuperDragApp(HINSTANCE instance) noexcept;
    ~SuperDragApp();

    SuperDragApp(const SuperDragApp&) = delete;
    SuperDragApp& operator=(const SuperDragApp&) = delete;

    int Run(int showCommand);
    LRESULT HandleMouseHook(int code, WPARAM message,
                            const MSLLHOOKSTRUCT* mouseInfo);

  private:
    enum class HookRuntimeState {
        Stopped,
        Starting,
        Active,
        Failed,
    };

    struct DragState {
        bool active = false;
        bool beginPending = false;
        bool updatePending = false;
        bool releasePending = false;
        bool finalMoveRequested = false;
        bool restoring = false;
        bool movementFailed = false;
        bool moveRequested = false;
        unsigned restoreAttempts = 0;
        std::uint64_t generation = 0;
        HWND target = nullptr;
        Point startCursor{};
        Point latestCursor{};
        Point grabOffset{};
        Point lastAppliedOrigin{};
        Point lastRequestedOrigin{};
        Point finalRequestedOrigin{};
        Rect maximizedRect{};
    };

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message,
                                               WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM message,
                                          LPARAM lParam);

    bool Initialize(std::wstring* error);
    bool EnsureSingleInstance();
    bool RegisterWindowClasses(std::wstring* error);
    bool CreateMainWindow(std::wstring* error);
    bool CreateSettingsWindow(std::wstring* error);
    bool StartMoveWorker(std::wstring* error);
    void Shutdown();

    LRESULT OnMainMessage(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam);
    LRESULT OnSettingsMessage(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam);

    bool InstallMouseHook(std::wstring* error, DWORD* errorCode = nullptr);
    void RemoveMouseHook();
    void RequestMouseHookInstall(bool resetRetries);
    void AttemptMouseHookInstall();
    void HandleMouseHookInstallFailure(DWORD errorCode,
                                       const std::wstring& error);
    void CancelMouseHookRetry();
    bool IsMouseHookActive() const noexcept;
    bool IsConfiguredChordDown() const noexcept;
    std::uint32_t CurrentModifierMask() const noexcept;

    HWND ResolveDragTarget(POINT cursor, bool* restricted) const;
    bool IsTargetHigherIntegrity(HWND target) const;
    bool BeginDragFromHook(HWND target, Point cursor);
    void BeginDragOnMessageThread();
    bool IsDragPositionReady() const noexcept;
    bool SubmitLatestDragPosition(bool finalRequest);
    void BeginDragRelease();
    void HandleMoveCompleted();
    void ScheduleDragUpdate();
    void ApplyLatestDragPosition();
    void EndDrag(const wchar_t* reason);
    void FailCurrentDrag(bool showPrivilegeHint,
                         DWORD error = ERROR_SUCCESS);

    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void ShowTrayNotification(const wchar_t* title, const wchar_t* message,
                              DWORD iconFlags);
    void ShowPrivilegeHintOnce();

    void ShowSettingsWindow();
    void ApplyThemeToSettingsWindow();
    void LoadSettingsIntoControls();
    void CreateSettingsControls();
    void LayoutSettingsControls(UINT dpi);
    void RecreateSettingsFont(UINT dpi);
    void SetSettingsStatus(const std::wstring& text, bool is_error = false);
    void SetHookStatus(const std::wstring& text, bool isError);
    void UpdateHookStatusInSettings();
    void SaveSettingsFromControls();
    bool CommitSettings(const UserSettings& requested, bool startupEnabled,
                        std::wstring* error);
    void ToggleEnabledFromTray();
    void ToggleStartupFromTray();

    HINSTANCE instance_ = nullptr;
    HANDLE instanceMutex_ = nullptr;
    HWND mainWindow_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HFONT settingsFont_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    DWORD ownIntegrityLevel_ = 0;
    UserSettings settings_{};
    std::unique_ptr<ui::UiTheme> theme_;
    std::unique_ptr<WindowMoveWorker> moveWorker_;
    HookRuntimeState hookRuntimeState_ = HookRuntimeState::Stopped;
    std::size_t hookRetryIndex_ = 0;
    DWORD lastHookError_ = ERROR_SUCCESS;
    std::wstring lastHookErrorText_;
    bool hookInstallMessagePending_ = false;
    bool hookStatusVisible_ = false;
    bool settingsStatusError_ = false;
    DragState drag_{};
    std::uint64_t dragGenerationCounter_ = 0;
    DWORD lastMoveTraceTick_ = 0;
    DWORD lastMoveCompletionTraceTick_ = 0;
    bool trayIconAdded_ = false;
    bool shuttingDown_ = false;
};

}  // namespace superdrag
