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
    Expect(defaults.dragEngineMode == DragEngineMode::CompatibilityOnly,
           "compatibility drag routing is the safe default");
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
    Expect(NormalizeDragEngineMode(99) ==
               DragEngineMode::CompatibilityOnly,
           "invalid registry values fall back to compatibility mode");

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
    Expect(ShouldForwardInitialPress(DragStartAction::Native),
           "native routing forwards the real primary-button press");
    Expect(!ShouldForwardInitialPress(DragStartAction::Compatibility),
           "compatibility routing suppresses the primary-button press");
    Expect(!ShouldForwardInitialPress(DragStartAction::Reject),
           "rejected native routing suppresses the shortcut press");
}

void TestShouldSkipNativeMoveForTarget() {
    using namespace nekodrag;

    Expect(ShouldSkipNativeMoveForTarget(DragEngineMode::Automatic, true),
           "automatic mode skips native for a Chromium target");
    Expect(!ShouldSkipNativeMoveForTarget(DragEngineMode::Automatic, false),
           "automatic mode still uses native for ordinary windows");
    Expect(!ShouldSkipNativeMoveForTarget(DragEngineMode::NativeOnly, true),
           "native-only mode keeps attempting native for diagnosis");
    Expect(!ShouldSkipNativeMoveForTarget(DragEngineMode::CompatibilityOnly,
                                          true),
           "compatibility mode never starts a native attempt anyway");
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

void TestLegacyStartupCommandOwnership() {
    using nekodrag::ShouldMigrateLegacyStartup;

    Expect(ShouldMigrateLegacyStartup(
               true, L"\"C:\\Tools\\SuperDrag.exe\""),
           "quoted legacy executable is recognized");
    Expect(ShouldMigrateLegacyStartup(
               true, L"\"\\\\server\\share\\SUPERDRAG.EXE\""),
           "legacy executable comparison is case-insensitive");
    Expect(!ShouldMigrateLegacyStartup(
               false, L"\"C:\\Tools\\SuperDrag.exe\""),
           "legacy command is ignored unless settings were imported");
    Expect(!ShouldMigrateLegacyStartup(true, L"C:\\Tools\\SuperDrag.exe"),
           "unquoted legacy command is rejected");
    Expect(!ShouldMigrateLegacyStartup(
               true, L"\"C:\\Tools\\SuperDrag.exe\" --background"),
           "legacy command with arguments is rejected");
    Expect(!ShouldMigrateLegacyStartup(
               true, L"\"C:\\Tools\\NekoDrag.exe\""),
           "different executable is rejected");
    Expect(!ShouldMigrateLegacyStartup(true, L"\"SuperDrag.exe\""),
           "relative legacy executable is rejected");
    Expect(!ShouldMigrateLegacyStartup(
               true, L"\"Tools\\SuperDrag.exe\""),
           "relative legacy path with a directory is rejected");
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
               NativeMoveCompletionAction::WaitForStartEvent,
           "a dispatched pre-start release waits for a delayed start event");
    Expect(DecideNativeMoveCompletion(false, true, true) ==
               NativeMoveCompletionAction::UseManualFallback,
           "a pre-start release still finalizes after the grace period");
    Expect(DecideNativeMoveCompletion(false, false, false) ==
               NativeMoveCompletionAction::WaitForStartEvent,
           "an early return waits for a delayed move-start event");
    Expect(DecideNativeMoveCompletion(false, false, true) ==
               NativeMoveCompletionAction::UseManualFallback,
           "an ignored native request falls back after the grace period");
}

void TestNativeMoveStartAndRetryDecisions() {
    using namespace nekodrag;

    Expect(ShouldRequestNativeMoveOnMovement(true, false, false, true),
           "hook-observed down starts native dispatch without depending on "
           "the suppressed Windows async key state");
    Expect(!ShouldRequestNativeMoveOnMovement(false, false, false, true),
           "compatibility and inactive states do not start native dispatch");
    Expect(!ShouldRequestNativeMoveOnMovement(true, true, false, true),
           "repeated movement cannot queue the same native attempt twice");
    Expect(!ShouldRequestNativeMoveOnMovement(true, false, true, true),
           "a released gesture cannot start native dispatch");
    Expect(!ShouldRequestNativeMoveOnMovement(true, false, false, false),
           "native dispatch requires the hook-observed primary-button down");
    Expect(SelectPhysicalPrimaryButton(false) ==
               PhysicalMouseButton::Left,
           "normal button mapping checks the physical left button");
    Expect(SelectPhysicalPrimaryButton(true) ==
               PhysicalMouseButton::Right,
           "swapped button mapping checks the physical right button");

    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::NonClientCaption,
               DragEngineMode::Automatic, false) ==
               NativeMoveNoStartAction::TrySystemCommand,
           "the caption strategy is followed once by the system command");
    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::NonClientCaption,
               DragEngineMode::NativeOnly, false) ==
               NativeMoveNoStartAction::TrySystemCommand,
           "native-only mode also performs the ordered second strategy");
    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::SystemCommand,
               DragEngineMode::Automatic, false) ==
               NativeMoveNoStartAction::UseManualFallback,
           "automatic mode falls back after both native strategies");
    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::SystemCommand,
               DragEngineMode::NativeOnly, false) ==
               NativeMoveNoStartAction::FailNativeOnly,
           "native-only mode reports failure after both strategies");
    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::NonClientCaption,
               DragEngineMode::Automatic, true) ==
               NativeMoveNoStartAction::UseManualFallback,
           "release between native attempts cancels automatic dispatch");
    Expect(DecideNativeMoveNoStartAction(
               NativeMoveStrategy::NonClientCaption,
               DragEngineMode::NativeOnly, true) ==
               NativeMoveNoStartAction::Complete,
           "release between attempts ends native-only mode");
}

void TestDragReleaseDecisions() {
    using namespace nekodrag;

    Expect(DecideDragReleaseAction(DragReleasePhase::Inactive) ==
               DragReleaseAction::Ignore,
           "inactive gestures ignore button release");
    Expect(DecideDragReleaseAction(DragReleasePhase::BeginPending) ==
               DragReleaseAction::SuppressAndFinalize,
           "release before begin is suppressed and finalized");
    Expect(DecideDragReleaseAction(DragReleasePhase::Manual) ==
               DragReleaseAction::SuppressAndFinalize,
           "manual release is suppressed and finalized");
    Expect(DecideDragReleaseAction(
               DragReleasePhase::NativeAwaitingMovement) ==
               DragReleaseAction::SuppressAndFinalize,
           "release before first movement cannot start a native loop");
    Expect(DecideDragReleaseAction(DragReleasePhase::NativeStarting) ==
               DragReleaseAction::SuppressAndReplay,
           "pre-start native release is deferred for replay");
    Expect(DecideDragReleaseAction(DragReleasePhase::NativeActive) ==
               DragReleaseAction::ForwardToNative,
           "active native release is forwarded to the system loop");

    Expect(DecideForwardedPressReleaseAction(
               DragReleasePhase::BeginPending) ==
               ForwardedPressReleaseAction::ForwardAndEnd,
           "a forwarded press released before setup remains a click");
    Expect(DecideForwardedPressReleaseAction(
               DragReleasePhase::NativeAwaitingMovement) ==
               ForwardedPressReleaseAction::ForwardAndEnd,
           "a forwarded press released before attempt one remains a click");
    Expect(DecideForwardedPressReleaseAction(
               DragReleasePhase::NativeStarting) ==
               ForwardedPressReleaseAction::SuppressAndReplay,
           "release racing native dispatch is suppressed for one replay");
    Expect(DecideForwardedPressReleaseAction(DragReleasePhase::Manual) ==
               ForwardedPressReleaseAction::ForwardAndFinalize,
           "manual fallback forwards release while finalizing position");
    Expect(DecideForwardedPressReleaseAction(
               DragReleasePhase::NativeActive) ==
               ForwardedPressReleaseAction::ForwardToNative,
           "an active native loop receives the real forwarded release");
}

void TestNativeReleaseReplayDecision() {
    using namespace nekodrag;

    Expect(ShouldReplayNativeButtonRelease(false, true, true, true, false,
                                           true, true),
           "a matching native-start event replays a suppressed release");
    Expect(!ShouldReplayNativeButtonRelease(false, true, true, true, true,
                                            true, true),
           "a suppressed release is replayed at most once");
    Expect(!ShouldReplayNativeButtonRelease(false, true, false, true, false,
                                            true, true),
           "replay requires a real observed release");
    Expect(!ShouldReplayNativeButtonRelease(false, true, true, true, false,
                                            false, true),
           "a stale generation cannot replay into a new gesture");
    Expect(!ShouldReplayNativeButtonRelease(false, true, true, true, false,
                                            true, false),
           "a different target cannot receive a replayed release");
    Expect(!ShouldReplayNativeButtonRelease(true, true, true, true, false,
                                            true, true),
           "a completed native request does not receive a late replay");
}

void TestMaximizedNativeRollbackDecision() {
    using namespace nekodrag;

    Expect(ShouldRollbackMaximizedNativeRestore(true, false, false, false),
           "native-only startup failure rolls a restored window back");
    Expect(ShouldRollbackMaximizedNativeRestore(true, true, true, false),
           "Escape rolls an active restored native window back");
    Expect(!ShouldRollbackMaximizedNativeRestore(true, true, false, false),
           "normal native completion keeps the restored window");
    Expect(!ShouldRollbackMaximizedNativeRestore(true, false, false, true),
           "compatibility fallback continues from the restored window");
    Expect(!ShouldRollbackMaximizedNativeRestore(false, false, true, false),
           "an unsuccessful restore cannot be rolled back");
}

void TestShouldCancelTargetNativeMove() {
    using namespace nekodrag;

    Expect(ShouldCancelTargetNativeMove(true, 0, false, false),
           "a forwarded press alone can start the target loop");
    Expect(ShouldCancelTargetNativeMove(false, 1, true, false),
           "a submitted native attempt may have driven the target loop");
    Expect(!ShouldCancelTargetNativeMove(true, 1, true, true),
           "an observed move end already exited the loop");
    Expect(!ShouldCancelTargetNativeMove(false, 0, false, false),
           "no press and no attempt cannot have started a loop");
    Expect(ShouldCancelTargetNativeMove(true, 1, false, false),
           "a started-but-not-ended loop still needs its capture released");
}

void TestNativeMoveEventTimeMatching() {
    using namespace nekodrag;

    Expect(IsNativeMoveEventTimeCurrent(1000, 1000),
           "an event at generation start is current");
    Expect(IsNativeMoveEventTimeCurrent(1100, 1000),
           "an event after generation start is current");
    Expect(!IsNativeMoveEventTimeCurrent(999, 1000),
           "an event before generation start is stale");
    Expect(IsNativeMoveEventTimeCurrent(0x00000010U, 0xFFFFFFF0U),
           "event time matching survives the system tick wrap");
    Expect(!IsNativeMoveEventTimeCurrent(0xFFFFFFF0U, 0x00000010U),
           "a pre-wrap stale event cannot enter a post-wrap generation");

    Expect(IsNativeMoveEventMatch(12, 12, true),
           "a matching attempt token and target accepts a move event");
    Expect(!IsNativeMoveEventMatch(12, 11, true),
           "a late event from attempt one cannot enter attempt two");
    Expect(!IsNativeMoveEventMatch(12, 12, false),
           "an event for a different target is rejected");
    Expect(!IsNativeMoveEventMatch(0, 0, true),
           "an inactive event token rejects queued events");
}

}  // namespace

int main() {
    TestModifierValidation();
    TestDragEngineRouting();
    TestShouldSkipNativeMoveForTarget();
    TestPositionCalculations();
    TestLegacyStartupCommandOwnership();
    TestWindowFiltering();
    TestNativeMoveCompletionDecision();
    TestNativeMoveStartAndRetryDecisions();
    TestDragReleaseDecisions();
    TestNativeReleaseReplayDecision();
    TestMaximizedNativeRollbackDecision();
    TestShouldCancelTargetNativeMove();
    TestNativeMoveEventTimeMatching();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All NekoDrag core tests passed\n";
    return EXIT_SUCCESS;
}
