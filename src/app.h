#pragma once

#include "core.h"
#include "native_move_worker.h"
#include "ui_theme.h"
#include "window_move_worker.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nekodrag {

class NekoDragApp {
  public:
    explicit NekoDragApp(HINSTANCE instance) noexcept;
    ~NekoDragApp();

    NekoDragApp(const NekoDragApp&) = delete;
    NekoDragApp& operator=(const NekoDragApp&) = delete;

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

    enum class DragMode {
        None,
        NativeAwaitingMovement,
        NativeStarting,
        NativeActive,
        ManualFallback,
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
        bool buttonReleased = false;
        bool nativeMoveStarted = false;
        bool nativeMoveReturned = false;
        bool nativeMoveEndObserved = false;
        bool nativeReleaseSuppressed = false;
        bool nativeReleaseReplayed = false;
        bool nativeMovementObserved = false;
        bool nativeStartMessagePending = false;
        bool nativePressForwarded = false;
        bool nativeInteractionCancelAttempted = false;
        bool nativeInteractionCancelSucceeded = false;
        bool nativeStartedMaximized = false;
        bool nativeMaximizedRestoreRequested = false;
        bool nativeMaximizedRestoreAttempted = false;
        bool nativeMaximizedRestoreSucceeded = false;
        bool nativeMaximizedRollbackRequired = false;
        bool nativeEscapeObserved = false;
        unsigned restoreAttempts = 0;
        std::uint32_t nativeAttempt = 0;
        std::uint64_t generation = 0;
        std::uint64_t nativeEventToken = 0;
        std::uint64_t manualRequests = 0;
        std::uint64_t manualCompletions = 0;
        std::uint64_t manualCoalescedRequests = 0;
        std::uint64_t manualMaxElapsedUs = 0;
        DragEngineMode engineMode = DragEngineMode::CompatibilityOnly;
        DragStartAction startAction = DragStartAction::Reject;
        DragMode mode = DragMode::None;
        NativeMoveStrategy nativeStrategy =
            NativeMoveStrategy::NonClientCaption;
        HWND target = nullptr;
        HWND initialPressWindow = nullptr;
        HWND nativeCaptureWindow = nullptr;
        Point startCursor{};
        Point latestCursor{};
        Point grabOffset{};
        Point lastAppliedOrigin{};
        Point lastRequestedOrigin{};
        Point finalRequestedOrigin{};
        Point nativeRestoredOrigin{};
        Rect maximizedRect{};
        std::shared_ptr<std::atomic_bool> nativeCancelRequested;
    };

    static LRESULT CALLBACK MainWindowProc(HWND window, UINT message,
                                           WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsWindowProc(HWND window, UINT message,
                                               WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MouseHookProc(int code, WPARAM message,
                                          LPARAM lParam);
    static LRESULT CALLBACK NativeEscapeHookProc(int code, WPARAM message,
                                                 LPARAM lParam);
    static void CALLBACK NativeMoveEventProc(
        HWINEVENTHOOK eventHook, DWORD event, HWND window, LONG objectId,
        LONG childId, DWORD eventThread, DWORD eventTime);

    bool Initialize(std::wstring* error);
    bool EnsureSingleInstance();
    bool RegisterWindowClasses(std::wstring* error);
    bool CreateMainWindow(std::wstring* error);
    bool CreateSettingsWindow(std::wstring* error);
    bool StartMoveWorker(std::wstring* error);
    void InitializeNativeMoveInfrastructure();
    bool StartNativeMoveWorker();
    bool InstallNativeMoveEventHook();
    void RemoveNativeMoveEventHook();
    void SuspendNativeMoveWorker();
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
    bool InstallNativeEscapeHook();
    void RemoveNativeEscapeHook();
    void ObserveNativeEscape();
    void HandleNativeEscapeObserved(std::uint64_t generation,
                                    HWND target);
    bool IsConfiguredChordDown() const noexcept;
    std::uint32_t CurrentModifierMask() const noexcept;

    HWND ResolveDragTarget(POINT cursor, bool* restricted,
                           HWND* initialPressWindow) const;
    HWND ResolveTargetCaptureWindow(HWND target) const;
    bool IsTargetHigherIntegrity(HWND target) const;
    bool BeginDragFromHook(HWND target, HWND initialPressWindow,
                           Point cursor);
    void BeginDragOnMessageThread();
    void RequestNativeMoveStart();
    void HandleNativeMoveStartRequested(std::uint64_t generation,
                                        HWND target);
    bool SubmitNativeMoveAttempt(NativeMoveStrategy strategy);
    void BeginManualFallback(bool nativeAttempted);
    bool IsNativeDrag() const noexcept;
    bool IsNativeMoveAttemptInFlight() const noexcept;
    bool IsObservedPrimaryButtonDown() const noexcept;
    bool IsLogicalPrimaryButtonAsyncDown() const noexcept;
    void HandleNativeMoveCompleted();
    void HandleNativeMoveEvent(bool started, std::uint64_t eventToken,
                               HWND target);
    void HandleNativeFallbackTimeout();
    void NoteNativeButtonReleased();
    void HandleNativeButtonReleased(std::uint64_t generation, HWND target);
    bool ReplayNativeButtonRelease();
    void CancelTargetNativeMove();
    void RestartNativeMoveWorker();
    void RollbackMaximizedNativeRestoreIfNeeded(
        bool compatibilityFallbackContinues);
    void CompleteNativeDrag(const wchar_t* reason);
    void FailNativeOnlyDrag(const wchar_t* reason, DWORD error,
                            bool nativeAttempted);
    void RestoreMouseHookAfterNativeDrag();
    bool SubmitLatestDragPosition(bool finalRequest);
    void HandleMoveCompleted();
    void ScheduleDragUpdate();
    void ApplyLatestDragPosition();
    void EndDrag(const wchar_t* reason, bool trace = true);
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
    HANDLE legacyInstanceMutex_ = nullptr;
    HWND mainWindow_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HHOOK nativeEscapeHook_ = nullptr;
    HFONT settingsFont_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    DWORD ownIntegrityLevel_ = 0;
    UserSettings settings_{};
    std::unique_ptr<ui::UiTheme> theme_;
    std::unique_ptr<WindowMoveWorker> moveWorker_;
    std::unique_ptr<NativeMoveWorker> nativeMoveWorker_;
    HWINEVENTHOOK nativeMoveEventHook_ = nullptr;
    std::atomic<HWND> nativeEventCompletionWindow_{nullptr};
    std::atomic<std::uint64_t> nativeEventToken_{0};
    std::atomic<std::uint32_t> nativeEventAttemptStartedAt_{0};
    bool nativeMoveAvailable_ = false;
    HookRuntimeState hookRuntimeState_ = HookRuntimeState::Stopped;
    std::size_t hookRetryIndex_ = 0;
    DWORD lastHookError_ = ERROR_SUCCESS;
    std::wstring lastHookErrorText_;
    bool hookInstallMessagePending_ = false;
    bool hookStatusVisible_ = false;
    bool settingsStatusError_ = false;
    bool nativeOnlyFailureNotified_ = false;
    DragState drag_{};
    std::uint64_t dragGenerationCounter_ = 0;
    std::uint64_t nativeEventTokenCounter_ = 0;
    bool trayIconAdded_ = false;
    bool shuttingDown_ = false;
};

}  // namespace nekodrag
