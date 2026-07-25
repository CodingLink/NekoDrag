#include "core.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void TestModifierValidation() {
    using namespace nekodrag;
    const UserSettings defaults;
    Expect(defaults.enabled, "dragging is enabled by default");
    Expect(defaults.modifierMask == (kModifierWin | kModifierAlt),
           "default shortcut is Win+Alt");
    Expect(defaults.dragEngineMode == DragEngineMode::Automatic,
           "automatic drag routing is the default");
    Expect(!defaults.firstRunCompleted,
           "a new profile has not completed first-run setup");
    Expect(IsValidModifierMask(kModifierWin), "one modifier is valid");
    Expect(IsValidModifierMask(kModifierWin | kModifierAlt),
           "default modifier pair is valid");
    Expect(IsValidModifierMask(kModifierControl | kModifierAlt |
                               kModifierShift),
           "three modifiers are valid");
    Expect(!IsValidModifierMask(0), "empty modifier mask is invalid");
    Expect(!IsValidModifierMask(kAllModifiers),
           "four modifiers are outside the supported range");
    Expect(!IsValidModifierMask(kModifierWin | 0x1000),
           "unknown modifier bits are invalid");
    Expect(IsExactModifierMatch(kDefaultModifiers, kDefaultModifiers),
           "configured modifiers match exactly");
    Expect(!IsExactModifierMatch(kDefaultModifiers,
                                 kDefaultModifiers | kModifierShift),
           "additional modifiers prevent activation");
}

void TestDragEngineRouting() {
    using namespace nekodrag;

    Expect(IsValidDragEngineMode(0), "automatic drag mode is valid");
    Expect(IsValidDragEngineMode(1), "native-only drag mode is valid");
    Expect(IsValidDragEngineMode(2),
           "compatibility-only drag mode is valid");
    Expect(!IsValidDragEngineMode(3), "unknown drag mode is invalid");
    Expect(NormalizeDragEngineMode(0) == DragEngineMode::Automatic,
           "automatic registry value is preserved");
    Expect(NormalizeDragEngineMode(1) == DragEngineMode::NativeOnly,
           "native-only registry value is preserved");
    Expect(NormalizeDragEngineMode(2) ==
               DragEngineMode::CompatibilityOnly,
           "compatibility registry value is preserved");
    Expect(NormalizeDragEngineMode(99) == DragEngineMode::Automatic,
           "invalid registry values fall back to automatic mode");

    Expect(SelectDragStartAction(DragEngineMode::Automatic, true) ==
               DragStartAction::Native,
           "automatic mode prefers the native engine");
    Expect(SelectDragStartAction(DragEngineMode::Automatic, false) ==
               DragStartAction::Compatibility,
           "automatic mode uses compatibility when native is unavailable");
    Expect(SelectDragStartAction(DragEngineMode::NativeOnly, true) ==
               DragStartAction::Native,
           "native-only mode starts native when available");
    Expect(SelectDragStartAction(DragEngineMode::NativeOnly, false) ==
               DragStartAction::Reject,
           "native-only mode rejects unavailable native infrastructure");
    Expect(SelectDragStartAction(DragEngineMode::CompatibilityOnly, true) ==
               DragStartAction::Compatibility,
           "compatibility-only mode bypasses available native support");
    Expect(SelectDragStartAction(DragEngineMode::CompatibilityOnly, false) ==
               DragStartAction::Compatibility,
           "compatibility-only mode does not depend on native support");
    Expect(SelectDragStartAction(static_cast<DragEngineMode>(99), true) ==
               DragStartAction::Reject,
           "invalid in-memory modes fail closed");
    Expect(AllowsCompatibilityFallback(DragEngineMode::Automatic),
           "automatic mode permits compatibility fallback");
    Expect(!AllowsCompatibilityFallback(DragEngineMode::NativeOnly),
           "native-only mode forbids compatibility fallback");
    Expect(!AllowsCompatibilityFallback(DragEngineMode::CompatibilityOnly),
           "compatibility-only mode never starts a native attempt");
}

void TestPositionCalculations() {
    using namespace nekodrag;
    const Point dragged = ComputeDraggedOrigin({250, 180}, {50, 30});
    Expect(dragged.x == 200 && dragged.y == 150,
           "normal drag preserves the grab offset");

    const Point restored =
        ComputeRestoredOrigin({960, 540}, {0, 0, 1920, 1080}, {1000, 600});
    Expect(restored.x == 460 && restored.y == 240,
           "maximized restore preserves the relative center point");

    const Point negative = ComputeRestoredOrigin(
        {-960, 540}, {-1920, 0, 0, 1080}, {800, 600});
    Expect(negative.x == -1360 && negative.y == 240,
           "restore math supports monitors with negative coordinates");

    const Point clamped =
        ComputeRestoredOrigin({2500, -100}, {0, 0, 1920, 1080}, {800, 600});
    Expect(clamped.x == 1700 && clamped.y == -100,
           "relative positions are clamped to the original window");
}

void TestWindowFiltering() {
    using namespace nekodrag;
    WindowTraits valid;
    valid.exists = true;
    valid.visible = true;
    valid.enabled = true;
    valid.topLevel = true;
    Expect(IsMovableWindowCandidate(valid), "ordinary top-level window is valid");

    WindowTraits own = valid;
    own.ownProcess = true;
    Expect(!IsMovableWindowCandidate(own), "own windows are rejected");

    WindowTraits shell = valid;
    shell.shellSurface = true;
    Expect(!IsMovableWindowCandidate(shell), "shell surfaces are rejected");

    WindowTraits cloaked = valid;
    cloaked.cloaked = true;
    Expect(!IsMovableWindowCandidate(cloaked), "cloaked windows are rejected");

    WindowTraits minimized = valid;
    minimized.minimized = true;
    Expect(!IsMovableWindowCandidate(minimized),
           "minimized windows are rejected");
}

void TestNativeMoveCompletionDecision() {
    using nekodrag::DecideNativeMoveCompletion;
    using nekodrag::NativeMoveCompletionAction;

    Expect(DecideNativeMoveCompletion(true, false, false) ==
               NativeMoveCompletionAction::Complete,
           "an entered native loop completes even when Escape leaves the "
           "button down");
    Expect(DecideNativeMoveCompletion(false, true, false) ==
               NativeMoveCompletionAction::Complete,
           "an observed release completes a quick native request");
    Expect(DecideNativeMoveCompletion(false, true, true) ==
               NativeMoveCompletionAction::Complete,
           "an observed release never falls back after the grace period");
    Expect(DecideNativeMoveCompletion(false, false, false) ==
               NativeMoveCompletionAction::WaitForStartEvent,
           "an early return waits for a delayed move-start event");
    Expect(DecideNativeMoveCompletion(false, false, true) ==
               NativeMoveCompletionAction::UseManualFallback,
           "an ignored native request falls back after the grace period");
}

}  // namespace

int main() {
    TestModifierValidation();
    TestDragEngineRouting();
    TestPositionCalculations();
    TestWindowFiltering();
    TestNativeMoveCompletionDecision();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All NekoDrag core tests passed\n";
    return EXIT_SUCCESS;
}
