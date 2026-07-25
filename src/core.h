#pragma once

#include <cstdint>

namespace superdrag {

// These values intentionally match the Win32 MOD_* constants.
constexpr std::uint32_t kModifierAlt = 0x0001;
constexpr std::uint32_t kModifierControl = 0x0002;
constexpr std::uint32_t kModifierShift = 0x0004;
constexpr std::uint32_t kModifierWin = 0x0008;
constexpr std::uint32_t kAllModifiers =
    kModifierAlt | kModifierControl | kModifierShift | kModifierWin;
constexpr std::uint32_t kDefaultModifiers = kModifierWin | kModifierAlt;

// Persisted as the DragMode DWORD under HKCU\Software\SuperDrag.
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
    DragEngineMode dragEngineMode = DragEngineMode::Automatic;
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

bool IsValidModifierMask(std::uint32_t mask) noexcept;
bool IsValidDragEngineMode(std::uint32_t value) noexcept;
DragEngineMode NormalizeDragEngineMode(std::uint32_t value) noexcept;
DragStartAction SelectDragStartAction(DragEngineMode mode,
                                      bool nativeAvailable) noexcept;
bool AllowsCompatibilityFallback(DragEngineMode mode) noexcept;
bool IsExactModifierMatch(std::uint32_t configured,
                          std::uint32_t currentlyDown) noexcept;
Point ComputeDraggedOrigin(Point cursor, Point grabOffset) noexcept;
Point ComputeRestoredOrigin(Point cursor, Rect maximizedRect,
                            Size restoredSize) noexcept;
bool IsMovableWindowCandidate(const WindowTraits& traits) noexcept;
NativeMoveCompletionAction DecideNativeMoveCompletion(
    bool moveSizeStarted, bool buttonReleaseObserved,
    bool startGraceExpired) noexcept;

}  // namespace superdrag
