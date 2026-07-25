#include "layout.h"
#include "window_move_worker.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

namespace native_move_worker_tests {
void Run(int* failures);
}

namespace {

int failures = 0;

void Expect(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

bool SamePoint(nekodrag::Point left, nekodrag::Point right) {
    return left.x == right.x && left.y == right.y;
}

void TestSettingsLayoutIncludesDragModeGroup() {
    using nekodrag::ui::SettingsLayout;

    constexpr UINT testDpis[] = {96U, 144U, 192U};
    for (const UINT dpi : testDpis) {
        const int clientWidth =
            SettingsLayout::Scale(SettingsLayout::kMinClientWidth, dpi);
        const int clientHeight =
            SettingsLayout::Scale(SettingsLayout::kMinClientHeight, dpi);
        const RECT modifier =
            SettingsLayout::ModifierGroup(dpi, clientWidth);
        const RECT dragMode =
            SettingsLayout::DragModeGroup(dpi, clientWidth, modifier.bottom);
        const RECT startup =
            SettingsLayout::StartupCheckbox(dpi, dragMode.bottom);
        const RECT help =
            SettingsLayout::HelpLabel(dpi, clientWidth, startup.bottom);
        const RECT status =
            SettingsLayout::StatusLabel(dpi, clientWidth, help.bottom);
        const RECT save =
            SettingsLayout::SaveButton(dpi, clientWidth, clientHeight);

        Expect(modifier.bottom < dragMode.top,
               "drag mode group follows modifier group");
        Expect(dragMode.bottom < startup.top,
               "startup checkbox follows drag mode group");
        Expect(status.bottom <= save.top,
               "status label does not overlap action buttons");
        for (int index = 0; index < 3; ++index) {
            const RECT option = SettingsLayout::DragModeOption(
                dpi, dragMode.left, dragMode.top, index);
            Expect(option.left >= dragMode.left &&
                       option.right <= dragMode.right &&
                       option.top >= dragMode.top &&
                       option.bottom <= dragMode.bottom,
                   "drag mode option remains inside its group");
        }
    }
}

bool WaitForRequestedOrigin(nekodrag::WindowMoveWorker* worker,
                            nekodrag::Point expected,
                            nekodrag::WindowMoveWorker::Result* result) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker->WaitForResult(result, 100) &&
            SamePoint(result->requestedOrigin, expected)) {
            return true;
        }
    }
    return false;
}

void UpdateMaximum(std::atomic<int>* maximum, int value) {
    int observed = maximum->load();
    while (value > observed &&
           !maximum->compare_exchange_weak(observed, value)) {
    }
}

void TestLatestRequestWinsAndMovesAreSerialized() {
    using nekodrag::Point;
    using nekodrag::WindowMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<WindowMoveWorker::Request> calls;
    bool releaseFirst = false;
    std::atomic<int> activeCalls{0};
    std::atomic<int> maximumActiveCalls{0};

    WindowMoveWorker worker(
        nullptr, 0, [&](const WindowMoveWorker::Request& request) {
            const int active = activeCalls.fetch_add(1) + 1;
            UpdateMaximum(&maximumActiveCalls, active);
            {
                std::unique_lock<std::mutex> lock(mutex);
                calls.push_back(request);
                changed.notify_all();
                if (calls.size() == 1) {
                    changed.wait(lock, [&releaseFirst]() {
                        return releaseFirst;
                    });
                }
            }
            activeCalls.fetch_sub(1);
            changed.notify_all();

            WindowMoveWorker::Result result;
            result.success = true;
            result.actualOrigin = request.origin;
            result.actualPositionKnown = true;
            return result;
        });

    Expect(worker.Start(), "move worker starts");
    const HWND target = GetDesktopWindow();
    Expect(worker.Submit({1, target, {10, 20}}),
           "first request is accepted");
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&calls]() {
                   return !calls.empty();
               }),
               "first move starts");
    }

    Expect(worker.Submit({1, target, {30, 40}}),
           "intermediate request is accepted");
    Expect(worker.Submit({1, target, {50, 60}}),
           "latest request is accepted");
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirst = true;
    }
    changed.notify_all();

    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&calls]() {
                   return calls.size() >= 2;
               }),
               "latest pending move runs");
    }

    WindowMoveWorker::Result result;
    Expect(WaitForRequestedOrigin(&worker, {50, 60}, &result),
           "latest move result is delivered");
    worker.Stop();

    Expect(calls.size() == 2, "intermediate pending request is coalesced");
    if (calls.size() >= 2) {
        Expect(SamePoint(calls[1].origin, {50, 60}),
               "worker executes the newest coordinates");
    }
    Expect(maximumActiveCalls.load() == 1,
           "only one move function runs at a time");
    Expect(result.coalescedRequests == 1,
           "result reports one overwritten request");
}

void TestNewGenerationSupersedesPendingOldDrag() {
    using nekodrag::WindowMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<WindowMoveWorker::Request> calls;
    bool releaseFirst = false;

    WindowMoveWorker worker(
        nullptr, 0, [&](const WindowMoveWorker::Request& request) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                calls.push_back(request);
                changed.notify_all();
                if (calls.size() == 1) {
                    changed.wait(lock, [&releaseFirst]() {
                        return releaseFirst;
                    });
                }
            }
            WindowMoveWorker::Result result;
            result.success = true;
            result.actualOrigin = request.origin;
            result.actualPositionKnown = true;
            return result;
        });

    Expect(worker.Start(), "generation test worker starts");
    const HWND target = GetDesktopWindow();
    Expect(worker.Submit({7, target, {1, 2}}),
           "old drag in-flight request is accepted");
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&calls]() {
                   return !calls.empty();
               }),
               "old drag request starts");
    }

    Expect(worker.Submit({7, target, {3, 4}}),
           "old pending drag request is accepted");
    Expect(worker.Submit({8, target, {90, 100}}),
           "new drag final request is accepted");
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirst = true;
    }
    changed.notify_all();

    WindowMoveWorker::Result result;
    Expect(WaitForRequestedOrigin(&worker, {90, 100}, &result),
           "new generation final result is delivered");
    worker.Stop();

    Expect(result.generation == 8,
           "completion preserves the new drag generation");
    Expect(calls.size() == 2,
           "pending request from the stale generation is overwritten");
}

void TestFailureResultIsPreserved() {
    using nekodrag::WindowMoveWorker;

    WindowMoveWorker worker(
        nullptr, 0, [](const WindowMoveWorker::Request&) {
            WindowMoveWorker::Result result;
            result.success = false;
            result.error = ERROR_ACCESS_DENIED;
            return result;
        });

    Expect(worker.Start(), "failure test worker starts");
    const HWND target = GetDesktopWindow();
    Expect(worker.Submit({12, target, {200, 300}}),
           "failing request is accepted");

    WindowMoveWorker::Result result;
    Expect(worker.WaitForResult(&result, 1000),
           "failure result is delivered");
    worker.Stop();

    Expect(!result.success, "failure status is preserved");
    Expect(result.error == ERROR_ACCESS_DENIED,
           "failure error code is preserved");
    Expect(result.generation == 12,
           "failure result preserves its generation");
    Expect(SamePoint(result.requestedOrigin, {200, 300}),
           "failure result preserves requested coordinates");
}

void TestNewGenerationReplacesStaleCompletion() {
    using nekodrag::WindowMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    int completed = 0;
    WindowMoveWorker worker(
        nullptr, 0, [&](const WindowMoveWorker::Request& request) {
            WindowMoveWorker::Result result;
            result.success = true;
            result.actualOrigin = request.origin;
            result.actualPositionKnown = true;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++completed;
            }
            changed.notify_all();
            return result;
        });

    Expect(worker.Start(), "stale completion worker starts");
    const HWND target = GetDesktopWindow();
    Expect(worker.Submit({15, target, {10, 15}}),
           "stale generation request is accepted");
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&completed]() {
                   return completed >= 1;
               }),
               "stale generation completes");
    }

    Expect(worker.Submit({16, target, {20, 25}}),
           "current generation request is accepted");
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&completed]() {
                   return completed >= 2;
               }),
               "current generation completes");
    }

    WindowMoveWorker::Result result;
    Expect(worker.WaitForResult(&result, 1000),
           "latest generation completion is delivered");
    worker.Stop();
    Expect(result.generation == 16,
           "unconsumed stale completion is discarded");
    Expect(SamePoint(result.requestedOrigin, {20, 25}),
           "latest generation coordinates are preserved");
}

void TestFinalReleaseCoordinateWins() {
    using nekodrag::Point;
    using nekodrag::WindowMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<WindowMoveWorker::Request> calls;
    bool releaseFirst = false;

    WindowMoveWorker worker(
        nullptr, 0, [&](const WindowMoveWorker::Request& request) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                calls.push_back(request);
                changed.notify_all();
                if (calls.size() == 1) {
                    changed.wait(lock, [&releaseFirst]() {
                        return releaseFirst;
                    });
                }
            }
            WindowMoveWorker::Result result;
            result.success = true;
            result.actualOrigin = request.origin;
            result.actualPositionKnown = true;
            return result;
        });

    Expect(worker.Start(), "final coordinate worker starts");
    const HWND target = GetDesktopWindow();
    Expect(worker.Submit({21, target, {5, 10}}),
           "in-flight move is accepted");
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(2), [&calls]() {
                   return !calls.empty();
               }),
               "in-flight move starts");
    }

    Expect(worker.Submit({21, target, {25, 30}}),
           "intermediate release move is accepted");
    const Point finalOrigin{45, 50};
    Expect(worker.Submit({21, target, finalOrigin}),
           "final release coordinate is accepted");
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirst = true;
    }
    changed.notify_all();

    WindowMoveWorker::Result result;
    Expect(WaitForRequestedOrigin(&worker, finalOrigin, &result),
           "final release coordinate completes");
    worker.Stop();

    Expect(calls.size() == 2,
           "only the in-flight and final release coordinates execute");
    if (calls.size() >= 2) {
        Expect(SamePoint(calls.back().origin, finalOrigin),
               "final release coordinate is the last executed move");
    }
    Expect(SamePoint(result.actualOrigin, finalOrigin),
           "final release result reports the final actual coordinate");
}

void TestStopAcceptingRejectsNewMoves() {
    using nekodrag::WindowMoveWorker;

    WindowMoveWorker worker(
        nullptr, 0, [](const WindowMoveWorker::Request& request) {
            WindowMoveWorker::Result result;
            result.success = true;
            result.actualOrigin = request.origin;
            result.actualPositionKnown = true;
            return result;
        });

    Expect(worker.Start(), "shutdown test worker starts");
    worker.StopAccepting();
    Expect(!worker.Submit({31, GetDesktopWindow(), {1, 1}}),
           "shutdown rejects new move requests");
    worker.Stop();
}

}  // namespace

int main() {
    TestSettingsLayoutIncludesDragModeGroup();
    TestLatestRequestWinsAndMovesAreSerialized();
    TestNewGenerationSupersedesPendingOldDrag();
    TestFailureResultIsPreserved();
    TestNewGenerationReplacesStaleCompletion();
    TestFinalReleaseCoordinateWins();
    TestStopAcceptingRejectsNewMoves();
    native_move_worker_tests::Run(&failures);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All NekoDrag window move worker tests passed\n";
    return EXIT_SUCCESS;
}
