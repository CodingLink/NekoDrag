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
    using superdrag::NativeMoveWorker;

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
           !worker.Submit({2, target, {30, 40}, VK_LBUTTON}));
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
    using superdrag::NativeMoveWorker;

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

void TestStopAcceptingRejectsNewRequests(TestContext context) {
    using superdrag::NativeMoveWorker;

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
    using superdrag::NativeMoveWorker;

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
    using superdrag::NativeMoveWorker;

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
    TestStopAcceptingRejectsNewRequests(context);
    TestNewGenerationReplacesStaleResult(context);
    TestBlockedRequestHasBoundedStop(context);
}

}  // namespace native_move_worker_tests
