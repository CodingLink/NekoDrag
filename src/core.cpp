#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nekodrag {
namespace {

unsigned CountBits(std::uint32_t value) noexcept {
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

double RelativePosition(std::int32_t position, std::int32_t start,
                        std::int32_t end) noexcept {
    const auto span = static_cast<std::int64_t>(end) - start;
    if (span <= 0) {
        return 0.5;
    }
    const auto offset = static_cast<std::int64_t>(position) - start;
    return std::clamp(static_cast<double>(offset) / static_cast<double>(span),
                      0.0, 1.0);
}

std::int32_t SaturatingSubtract(std::int32_t value,
                                std::int64_t offset) noexcept {
    const auto result = static_cast<std::int64_t>(value) - offset;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        result, INT32_MIN, INT32_MAX));
}

}  // namespace

bool IsValidModifierMask(std::uint32_t mask) noexcept {
    if ((mask & ~kAllModifiers) != 0) {
        return false;
    }
    const auto count = CountBits(mask);
    return count >= 1 && count <= 3;
}

bool IsValidDragEngineMode(std::uint32_t value) noexcept {
    return value <=
           static_cast<std::uint32_t>(DragEngineMode::CompatibilityOnly);
}

DragEngineMode NormalizeDragEngineMode(std::uint32_t value) noexcept {
    return IsValidDragEngineMode(value)
               ? static_cast<DragEngineMode>(value)
               : DragEngineMode::CompatibilityOnly;
}

DragStartAction SelectDragStartAction(DragEngineMode mode,
                                      bool nativeAvailable) noexcept {
    switch (mode) {
        case DragEngineMode::Automatic:
            return nativeAvailable ? DragStartAction::Native
                                   : DragStartAction::Compatibility;
        case DragEngineMode::NativeOnly:
            return nativeAvailable ? DragStartAction::Native
                                   : DragStartAction::Reject;
        case DragEngineMode::CompatibilityOnly:
            return DragStartAction::Compatibility;
    }
    return DragStartAction::Reject;
}

bool ShouldForwardInitialPress(DragStartAction action) noexcept {
    return action == DragStartAction::Native;
}

bool AllowsCompatibilityFallback(DragEngineMode mode) noexcept {
    return mode == DragEngineMode::Automatic;
}

bool IsExactModifierMatch(std::uint32_t configured,
                          std::uint32_t currentlyDown) noexcept {
    return IsValidModifierMask(configured) && configured == currentlyDown;
}

bool ShouldMigrateLegacyStartup(bool legacySettingsImported,
                                std::wstring_view command) noexcept {
    if (!legacySettingsImported) {
        return false;
    }
    if (command.size() < 4 || command.front() != L'"' ||
        command.back() != L'"') {
        return false;
    }

    const std::wstring_view path = command.substr(1, command.size() - 2);
    if (path.find(L'"') != std::wstring_view::npos) {
        return false;
    }
    const auto isSeparator = [](wchar_t character) {
        return character == L'\\' || character == L'/';
    };
    const bool driveAbsolute =
        path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && isSeparator(path[2]);
    const bool uncAbsolute = path.size() >= 3 && isSeparator(path[0]) &&
                             isSeparator(path[1]);
    if (!driveAbsolute && !uncAbsolute) {
        return false;
    }
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos || separator + 1 >= path.size()) {
        return false;
    }

    constexpr std::wstring_view expectedName = L"SuperDrag.exe";
    const std::wstring_view fileName = path.substr(separator + 1);
    if (fileName.size() != expectedName.size()) {
        return false;
    }
    for (std::size_t index = 0; index < fileName.size(); ++index) {
        wchar_t actual = fileName[index];
        if (actual >= L'A' && actual <= L'Z') {
            actual = static_cast<wchar_t>(actual - L'A' + L'a');
        }
        wchar_t expected = expectedName[index];
        if (expected >= L'A' && expected <= L'Z') {
            expected = static_cast<wchar_t>(expected - L'A' + L'a');
        }
        if (actual != expected) {
            return false;
        }
    }
    return true;
}

Point ComputeDraggedOrigin(Point cursor, Point grabOffset) noexcept {
    return {SaturatingSubtract(cursor.x, grabOffset.x),
            SaturatingSubtract(cursor.y, grabOffset.y)};
}

Point ComputeRestoredOrigin(Point cursor, Rect maximizedRect,
                            Size restoredSize) noexcept {
    const double relativeX =
        RelativePosition(cursor.x, maximizedRect.left, maximizedRect.right);
    const double relativeY =
        RelativePosition(cursor.y, maximizedRect.top, maximizedRect.bottom);
    const auto xOffset = static_cast<std::int64_t>(
        std::llround(std::max(restoredSize.width, 0) * relativeX));
    const auto yOffset = static_cast<std::int64_t>(
        std::llround(std::max(restoredSize.height, 0) * relativeY));
    return {SaturatingSubtract(cursor.x, xOffset),
            SaturatingSubtract(cursor.y, yOffset)};
}

bool IsMovableWindowCandidate(const WindowTraits& traits) noexcept {
    return traits.exists && traits.visible && traits.enabled &&
           !traits.minimized && traits.topLevel && !traits.ownProcess &&
           !traits.shellSurface && !traits.cloaked &&
           !traits.transientSurface;
}

NativeMoveCompletionAction DecideNativeMoveCompletion(
    bool moveSizeStarted, bool buttonReleaseObserved,
    bool startGraceExpired) noexcept {
    if (moveSizeStarted) {
        return NativeMoveCompletionAction::Complete;
    }
    if (buttonReleaseObserved && startGraceExpired) {
        return NativeMoveCompletionAction::UseManualFallback;
    }
    return startGraceExpired
               ? NativeMoveCompletionAction::UseManualFallback
               : NativeMoveCompletionAction::WaitForStartEvent;
}

bool ShouldRequestNativeMoveOnMovement(
    bool awaitingMovement, bool requestPending, bool buttonReleased,
    bool primaryButtonDownObserved) noexcept {
    return awaitingMovement && !requestPending && !buttonReleased &&
           primaryButtonDownObserved;
}

NativeMoveNoStartAction DecideNativeMoveNoStartAction(
    NativeMoveStrategy strategy, DragEngineMode mode,
    bool buttonReleased) noexcept {
    if (buttonReleased) {
        return AllowsCompatibilityFallback(mode)
                   ? NativeMoveNoStartAction::UseManualFallback
                   : NativeMoveNoStartAction::Complete;
    }
    if (strategy == NativeMoveStrategy::NonClientCaption) {
        return NativeMoveNoStartAction::TrySystemCommand;
    }
    return AllowsCompatibilityFallback(mode)
               ? NativeMoveNoStartAction::UseManualFallback
               : NativeMoveNoStartAction::FailNativeOnly;
}

PhysicalMouseButton SelectPhysicalPrimaryButton(
    bool buttonsSwapped) noexcept {
    return buttonsSwapped ? PhysicalMouseButton::Right
                          : PhysicalMouseButton::Left;
}

DragReleaseAction DecideDragReleaseAction(DragReleasePhase phase) noexcept {
    switch (phase) {
        case DragReleasePhase::BeginPending:
        case DragReleasePhase::Manual:
        case DragReleasePhase::NativeAwaitingMovement:
            return DragReleaseAction::SuppressAndFinalize;
        case DragReleasePhase::NativeStarting:
            return DragReleaseAction::SuppressAndReplay;
        case DragReleasePhase::NativeActive:
            return DragReleaseAction::ForwardToNative;
        case DragReleasePhase::Inactive:
            return DragReleaseAction::Ignore;
    }
    return DragReleaseAction::Ignore;
}

ForwardedPressReleaseAction DecideForwardedPressReleaseAction(
    DragReleasePhase phase) noexcept {
    switch (phase) {
        case DragReleasePhase::BeginPending:
        case DragReleasePhase::NativeAwaitingMovement:
            return ForwardedPressReleaseAction::ForwardAndEnd;
        case DragReleasePhase::NativeStarting:
            return ForwardedPressReleaseAction::SuppressAndReplay;
        case DragReleasePhase::Manual:
            return ForwardedPressReleaseAction::ForwardAndFinalize;
        case DragReleasePhase::NativeActive:
            return ForwardedPressReleaseAction::ForwardToNative;
        case DragReleasePhase::Inactive:
            return ForwardedPressReleaseAction::Ignore;
    }
    return ForwardedPressReleaseAction::Ignore;
}

bool ShouldReplayNativeButtonRelease(
    bool workerReturned, bool moveStarted, bool realReleaseObserved,
    bool releaseSuppressed, bool releaseAlreadyReplayed,
    bool generationMatches, bool targetMatches) noexcept {
    return !workerReturned && moveStarted && realReleaseObserved &&
           releaseSuppressed && !releaseAlreadyReplayed &&
           generationMatches && targetMatches;
}

bool IsNativeMoveEventTimeCurrent(
    std::uint32_t eventTime, std::uint32_t generationStartedAt) noexcept {
    constexpr std::uint32_t kHalfTickRange = 0x80000000U;
    return eventTime - generationStartedAt < kHalfTickRange;
}

bool IsNativeMoveEventMatch(std::uint64_t currentAttemptToken,
                            std::uint64_t eventAttemptToken,
                            bool targetMatches) noexcept {
    return currentAttemptToken != 0 &&
           eventAttemptToken == currentAttemptToken && targetMatches;
}

}  // namespace nekodrag
