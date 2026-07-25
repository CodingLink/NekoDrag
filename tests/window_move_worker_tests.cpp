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

namespace {

int failures = 0;

void Expect(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

bool SamePoint(superdrag::Point left, superdrag::Point right) {
    return left.x == right.x && left.y == right.y;
}

bool WaitForRequestedOrigin(superdrag::WindowMoveWorker* worker,
                            superdrag::Point expected,
                            superdrag::WindowMoveWorker::Result* result) {
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
    using superdrag::Point;
    using superdrag::WindowMoveWorker;

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
    using superdrag::WindowMoveWorker;

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
    using superdrag::WindowMoveWorker;

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
    using superdrag::WindowMoveWorker;

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
    using superdrag::Point;
    using superdrag::WindowMoveWorker;

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
    using superdrag::WindowMoveWorker;

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
    TestLatestRequestWinsAndMovesAreSerialized();
    TestNewGenerationSupersedesPendingOldDrag();
    TestFailureResultIsPreserved();
    TestNewGenerationReplacesStaleCompletion();
    TestFinalReleaseCoordinateWins();
    TestStopAcceptingRejectsNewMoves();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All SuperDrag window move worker tests passed\n";
    return EXIT_SUCCESS;
}
