#pragma once

#include "core.h"

#include <windows.h>

#include <cstdint>
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
    struct DragState {
        bool active = false;
        bool beginPending = false;
        bool updatePending = false;
        bool releasePending = false;
        bool restoring = false;
        bool movementFailed = false;
        unsigned restoreAttempts = 0;
        HWND target = nullptr;
        Point startCursor{};
        Point latestCursor{};
        Point grabOffset{};
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
    void Shutdown();

    LRESULT OnMainMessage(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam);
    LRESULT OnSettingsMessage(HWND window, UINT message, WPARAM wParam,
                              LPARAM lParam);

    bool InstallMouseHook(std::wstring* error);
    void RemoveMouseHook();
    bool IsConfiguredChordDown() const noexcept;
    std::uint32_t CurrentModifierMask() const noexcept;

    HWND ResolveDragTarget(POINT cursor, bool* restricted) const;
    bool IsTargetHigherIntegrity(HWND target) const;
    bool BeginDragFromHook(HWND target, Point cursor);
    void BeginDragOnMessageThread();
    void ScheduleDragUpdate();
    void ApplyLatestDragPosition();
    void EndDrag();
    void FailCurrentDrag(bool showPrivilegeHint);

    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void ShowTrayNotification(const wchar_t* title, const wchar_t* message,
                              DWORD iconFlags);
    void ShowPrivilegeHintOnce();

    void ShowSettingsWindow();
    void LoadSettingsIntoControls();
    void CreateSettingsControls();
    void LayoutSettingsControls(UINT dpi);
    void RecreateSettingsFont(UINT dpi);
    void SetSettingsStatus(const std::wstring& text);
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
    DragState drag_{};
    bool trayIconAdded_ = false;
    bool shuttingDown_ = false;
};

}  // namespace superdrag
