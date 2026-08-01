#include "native_move_worker.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace native_move_worker_tests {
namespace {

struct TestContext {
    int* failures = nullptr;
};

void Expect(TestContext context, bool condition) {
    if (!condition && context.failures != nullptr) {
        ++*context.failures;
    }
}

void TestSingleRequestAndBusyRejection(TestContext context) {
    using nekodrag::NativeMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    std::atomic<int> calls{0};

    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request&) {
            ++calls;
            {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&release]() { return release; });
            }
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });

    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit({1, target, {10, 20}}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(context,
               changed.wait_for(lock, std::chrono::seconds(2),
                                [&entered]() { return entered; }));
    }
    Expect(context,
           !worker.Submit({2, target, {30, 40}}));
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, calls.load() == 1);
    Expect(context, result.generation == 1);
    Expect(context, result.target == target);
    Expect(context, result.dispatched);
}

void TestFailureAndGenerationArePreserved(TestContext context) {
    using nekodrag::NativeMoveWorker;

    NativeMoveWorker worker(
        nullptr, 0, [](const NativeMoveWorker::Request&) {
            NativeMoveWorker::Result result;
            result.error = ERROR_ACCESS_DENIED;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context,
           worker.Submit({17, target, {-100, 200}}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, result.generation == 17);
    Expect(context, result.target == target);
    Expect(context, !result.dispatched);
    Expect(context, result.error == ERROR_ACCESS_DENIED);
}

void TestStrategyAndAttemptArePreserved(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;

    std::atomic<bool> sawSystemCommand{false};
    NativeMoveWorker worker(
        nullptr, 0, [&sawSystemCommand](
                        const NativeMoveWorker::Request& request) {
            sawSystemCommand.store(
                request.strategy == NativeMoveStrategy::SystemCommand &&
                    request.attempt == 2,
                std::memory_order_release);
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit(
                        {52, target, {100, 200}, nullptr,
                         NativeMoveStrategy::SystemCommand, 2}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, sawSystemCommand.load(std::memory_order_acquire));
    Expect(context, result.generation == 52);
    Expect(context, result.target == target);
    Expect(context,
           result.strategy == NativeMoveStrategy::SystemCommand);
    Expect(context, result.attempt == 2);
}

void TestCancelledRequestSkipsDispatch(TestContext context) {
    using nekodrag::NativeMoveWorker;

    std::atomic<int> calls{0};
    NativeMoveWorker worker(
        nullptr, 0, [&calls](const NativeMoveWorker::Request&) {
            ++calls;
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    const auto cancelRequested = std::make_shared<std::atomic_bool>(true);
    Expect(context, worker.Start());
    Expect(context, worker.Submit(
                        {18, GetDesktopWindow(), {}, cancelRequested}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, calls.load() == 0);
    Expect(context, !result.dispatched);
    Expect(context, result.error == ERROR_CANCELLED);
    Expect(context, result.generation == 18);
}

void TestCancellationAfterDispatchPreservesResult(TestContext context) {
    using nekodrag::NativeMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool finish = false;
    const auto cancelRequested = std::make_shared<std::atomic_bool>(false);
    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request&) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&finish]() { return finish; });
            }
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    Expect(context, worker.Submit(
                        {19, GetDesktopWindow(), {}, cancelRequested}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(context,
               changed.wait_for(lock, std::chrono::seconds(2),
                                [&entered]() { return entered; }));
        cancelRequested->store(true, std::memory_order_release);
        finish = true;
    }
    changed.notify_all();

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, result.dispatched);
    Expect(context, result.error == ERROR_SUCCESS);
    Expect(context, result.generation == 19);
}

void TestSharedCancellationPreventsSecondAttempt(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;

    std::atomic<int> calls{0};
    const auto cancelRequested = std::make_shared<std::atomic_bool>(false);
    NativeMoveWorker worker(
        nullptr, 0, [&calls](const NativeMoveWorker::Request&) {
            ++calls;
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit(
                        {61, target, {}, cancelRequested,
                         NativeMoveStrategy::NonClientCaption, 1}));

    NativeMoveWorker::Result first;
    Expect(context, worker.WaitForResult(&first, 2000));
    cancelRequested->store(true, std::memory_order_release);
    Expect(context, worker.Submit(
                        {61, target, {}, cancelRequested,
                         NativeMoveStrategy::SystemCommand, 2}));

    NativeMoveWorker::Result second;
    Expect(context, worker.WaitForResult(&second, 2000));
    worker.Stop();
    Expect(context, calls.load() == 1);
    Expect(context, first.dispatched);
    Expect(context, second.attempt == 2);
    Expect(context,
           second.strategy == NativeMoveStrategy::SystemCommand);
    Expect(context, !second.dispatched);
    Expect(context, second.error == ERROR_CANCELLED);
}

void TestCancelledCleanupRequestStillRunsCleanupStage(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;

    std::atomic<int> calls{0};
    std::atomic<bool> sawCleanupMetadata{false};
    const auto cancelRequested = std::make_shared<std::atomic_bool>(true);
    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request& request) {
            ++calls;
            sawCleanupMetadata.store(
                request.cancelInitialInteraction &&
                    request.captureWindow == GetDesktopWindow() &&
                    request.initialPressWindow == GetDesktopWindow(),
                std::memory_order_release);
            NativeMoveWorker::Result result;
            result.interactionCancelAttempted = true;
            result.interactionCancelSucceeded = true;
            result.error = ERROR_CANCELLED;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit(
                        {62, target, {}, cancelRequested,
                         NativeMoveStrategy::NonClientCaption, 1,
                         target, target, true}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, calls.load() == 1);
    Expect(context, sawCleanupMetadata.load(std::memory_order_acquire));
    Expect(context, result.interactionCancelAttempted);
    Expect(context, result.interactionCancelSucceeded);
    Expect(context, !result.dispatched);
    Expect(context, result.error == ERROR_CANCELLED);
}

void TestCleanupFailureMetadataIsPreserved(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;

    NativeMoveWorker worker(
        nullptr, 0, [](const NativeMoveWorker::Request&) {
            NativeMoveWorker::Result result;
            result.interactionCancelAttempted = true;
            result.interactionCancelError = ERROR_TIMEOUT;
            result.error = ERROR_TIMEOUT;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit(
                        {63, target, {}, nullptr,
                         NativeMoveStrategy::NonClientCaption, 1,
                         target, nullptr, true}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, result.interactionCancelAttempted);
    Expect(context, !result.interactionCancelSucceeded);
    Expect(context, result.interactionCancelError == ERROR_TIMEOUT);
    Expect(context, !result.dispatched);
    Expect(context, result.error == ERROR_TIMEOUT);
}

void TestStopAcceptingRejectsNewRequests(TestContext context) {
    using nekodrag::NativeMoveWorker;

    NativeMoveWorker worker(
        nullptr, 0, [](const NativeMoveWorker::Request&) {
            return NativeMoveWorker::Result{};
        });
    Expect(context, worker.Start());
    worker.StopAccepting();
    Expect(context,
           !worker.Submit({23, GetDesktopWindow(), {}}));
    worker.Stop();
}

void TestNewGenerationReplacesStaleResult(TestContext context) {
    using nekodrag::NativeMoveWorker;

    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request&) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++calls;
            }
            changed.notify_all();
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    Expect(context, worker.Submit({40, target, {}}));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(context,
               changed.wait_for(lock, std::chrono::seconds(2),
                                [&calls]() { return calls >= 1; }));
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool submitted = false;
    while (!submitted && std::chrono::steady_clock::now() < deadline) {
        submitted = worker.Submit({41, target, {}});
        if (!submitted) {
            Sleep(1);
        }
    }
    Expect(context, submitted);
    {
        std::unique_lock<std::mutex> lock(mutex);
        Expect(context,
               changed.wait_for(lock, std::chrono::seconds(2),
                                [&calls]() { return calls >= 2; }));
    }

    NativeMoveWorker::Result result;
    bool receivedNewest = false;
    const auto resultDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!receivedNewest &&
           std::chrono::steady_clock::now() < resultDeadline) {
        if (worker.WaitForResult(&result, 100) &&
            result.generation == 41) {
            receivedNewest = true;
        }
    }
    worker.Stop();
    Expect(context, receivedNewest);
}

void TestBlockedRequestHasBoundedStop(TestContext context) {
    using nekodrag::NativeMoveWorker;

    struct SharedBlock {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool release = false;
        bool completed = false;
    };
    const auto block = std::make_shared<SharedBlock>();

    NativeMoveWorker worker(
        nullptr, 0, [block](const NativeMoveWorker::Request&) {
            {
                std::unique_lock<std::mutex> lock(block->mutex);
                block->entered = true;
                block->changed.notify_all();
                block->changed.wait(
                    lock, [block]() { return block->release; });
                block->completed = true;
            }
            block->changed.notify_all();
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    Expect(context,
           worker.Submit({29, GetDesktopWindow(), {}}));
    {
        std::unique_lock<std::mutex> lock(block->mutex);
        Expect(context,
               block->changed.wait_for(
                   lock, std::chrono::seconds(2),
                   [block]() { return block->entered; }));
    }

    const auto started = std::chrono::steady_clock::now();
    worker.Stop(10);
    const auto stopElapsed = std::chrono::steady_clock::now() - started;
    Expect(context, stopElapsed < std::chrono::milliseconds(500));

    {
        std::lock_guard<std::mutex> lock(block->mutex);
        block->release = true;
    }
    block->changed.notify_all();
    {
        std::unique_lock<std::mutex> lock(block->mutex);
        Expect(context,
               block->changed.wait_for(
                   lock, std::chrono::seconds(2),
                   [block]() { return block->completed; }));
    }
}

}  // namespace

void Run(int* failures) {
    const TestContext context{failures};
    TestSingleRequestAndBusyRejection(context);
    TestFailureAndGenerationArePreserved(context);
    TestStrategyAndAttemptArePreserved(context);
    TestCancelledRequestSkipsDispatch(context);
    TestCancellationAfterDispatchPreservesResult(context);
    TestSharedCancellationPreventsSecondAttempt(context);
    TestCancelledCleanupRequestStillRunsCleanupStage(context);
    TestCleanupFailureMetadataIsPreserved(context);
    TestStopAcceptingRejectsNewRequests(context);
    TestNewGenerationReplacesStaleResult(context);
    TestBlockedRequestHasBoundedStop(context);
}

}  // namespace native_move_worker_tests
