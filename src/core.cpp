#include "core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace superdrag {
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

bool IsExactModifierMatch(std::uint32_t configured,
                          std::uint32_t currentlyDown) noexcept {
    return IsValidModifierMask(configured) && configured == currentlyDown;
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
    bool moveSizeStarted, bool logicalButtonDown,
    bool startGraceExpired) noexcept {
    if (moveSizeStarted || !logicalButtonDown) {
        return NativeMoveCompletionAction::Complete;
    }
    return startGraceExpired
               ? NativeMoveCompletionAction::UseManualFallback
               : NativeMoveCompletionAction::WaitForStartEvent;
}

}  // namespace superdrag
