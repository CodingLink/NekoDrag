#pragma once

#include <cstdint>
#include <string_view>

namespace nekodrag {

// These values intentionally match the Win32 MOD_* constants.
constexpr std::uint32_t kModifierAlt = 0x0001;
constexpr std::uint32_t kModifierControl = 0x0002;
constexpr std::uint32_t kModifierShift = 0x0004;
constexpr std::uint32_t kModifierWin = 0x0008;
constexpr std::uint32_t kAllModifiers =
    kModifierAlt | kModifierControl | kModifierShift | kModifierWin;
constexpr std::uint32_t kDefaultModifiers = kModifierWin | kModifierAlt;

// Persisted as the DragMode DWORD under HKCU\Software\NekoDrag.
enum class DragEngineMode : std::uint32_t {
    Automatic = 0,
    NativeOnly = 1,
    CompatibilityOnly = 2,
};

enum class DragStartAction {
    Native,
    Compatibility,
    Reject,
};

struct UserSettings {
    bool enabled = true;
    std::uint32_t modifierMask = kDefaultModifiers;
    DragEngineMode dragEngineMode = DragEngineMode::CompatibilityOnly;
    bool firstRunCompleted = false;
    bool privilegeHintShown = false;
};

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct Rect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct WindowTraits {
    bool exists = false;
    bool visible = false;
    bool enabled = false;
    bool minimized = false;
    bool topLevel = false;
    bool ownProcess = false;
    bool shellSurface = false;
    bool cloaked = false;
    bool transientSurface = false;
};

enum class NativeMoveCompletionAction {
    Complete,
    WaitForStartEvent,
    UseManualFallback,
};

enum class NativeMoveStrategy : std::uint32_t {
    NonClientCaption = 0,
    SystemCommand = 1,
};

enum class NativeMoveNoStartAction {
    TrySystemCommand,
    UseManualFallback,
    Complete,
    FailNativeOnly,
};

enum class DragReleasePhase {
    Inactive,
    BeginPending,
    Manual,
    NativeAwaitingMovement,
    NativeStarting,
    NativeActive,
};

enum class DragReleaseAction {
    Ignore,
    SuppressAndFinalize,
    SuppressAndReplay,
    ForwardToNative,
};

enum class ForwardedPressReleaseAction {
    Ignore,
    ForwardAndEnd,
    SuppressAndReplay,
    ForwardAndFinalize,
    ForwardToNative,
};

enum class PhysicalMouseButton {
    Left,
    Right,
};

bool IsValidModifierMask(std::uint32_t mask) noexcept;
bool IsValidDragEngineMode(std::uint32_t value) noexcept;
DragEngineMode NormalizeDragEngineMode(std::uint32_t value) noexcept;
DragStartAction SelectDragStartAction(DragEngineMode mode,
                                      bool nativeAvailable) noexcept;
bool ShouldForwardInitialPress(DragStartAction action) noexcept;
bool AllowsCompatibilityFallback(DragEngineMode mode) noexcept;
bool IsExactModifierMatch(std::uint32_t configured,
                          std::uint32_t currentlyDown) noexcept;
bool ShouldMigrateLegacyStartup(bool legacySettingsImported,
                                std::wstring_view command) noexcept;
Point ComputeDraggedOrigin(Point cursor, Point grabOffset) noexcept;
Point ComputeRestoredOrigin(Point cursor, Rect maximizedRect,
                            Size restoredSize) noexcept;
bool IsMovableWindowCandidate(const WindowTraits& traits) noexcept;
NativeMoveCompletionAction DecideNativeMoveCompletion(
    bool moveSizeStarted, bool buttonReleaseObserved,
    bool startGraceExpired) noexcept;
bool ShouldRequestNativeMoveOnMovement(
    bool awaitingMovement, bool requestPending, bool buttonReleased,
    bool primaryButtonDownObserved) noexcept;
NativeMoveNoStartAction DecideNativeMoveNoStartAction(
    NativeMoveStrategy strategy, DragEngineMode mode,
    bool buttonReleased) noexcept;
PhysicalMouseButton SelectPhysicalPrimaryButton(bool buttonsSwapped) noexcept;
DragReleaseAction DecideDragReleaseAction(DragReleasePhase phase) noexcept;
ForwardedPressReleaseAction DecideForwardedPressReleaseAction(
    DragReleasePhase phase) noexcept;
bool ShouldReplayNativeButtonRelease(
    bool workerReturned, bool moveStarted, bool realReleaseObserved,
    bool releaseSuppressed, bool releaseAlreadyReplayed,
    bool generationMatches, bool targetMatches) noexcept;
bool IsNativeMoveEventTimeCurrent(
    std::uint32_t eventTime, std::uint32_t generationStartedAt) noexcept;
bool IsNativeMoveEventMatch(std::uint64_t currentAttemptToken,
                            std::uint64_t eventAttemptToken,
                            bool targetMatches) noexcept;

}  // namespace nekodrag
