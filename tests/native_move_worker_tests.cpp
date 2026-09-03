#include "native_move_worker.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace native_move_worker_tests {
namespace {

struct TestContext {
    int* failures = nullptr;
};

enum class NativeTargetEvent {
    CancelMode,
    Restore,
    NonClientMove,
};

struct NativeTargetContext {
    std::mutex mutex;
    std::condition_variable changed;
    HWND window = nullptr;
    bool ready = false;
    std::vector<NativeTargetEvent> events;
};

LRESULT CALLBACK NativeTargetWindowProc(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<NativeTargetContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<NativeTargetContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(context));
    }
    if (context != nullptr) {
        if (message == WM_CANCELMODE) {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->events.push_back(NativeTargetEvent::CancelMode);
        } else if (message == WM_SYSCOMMAND &&
                   (wParam & 0xFFF0U) == SC_RESTORE) {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->events.push_back(NativeTargetEvent::Restore);
        } else if (message == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->events.push_back(NativeTargetEvent::NonClientMove);
            return 0;
        }
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void RunNativeTargetWindow(NativeTargetContext* context) {
    constexpr wchar_t kClassName[] =
        L"NekoDrag.NativeMoveWorker.RestoreTest";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = NativeTargetWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    if (RegisterClassW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->ready = true;
        context->changed.notify_all();
        return;
    }
    const HWND window = CreateWindowExW(
        0, kClassName, L"NekoDrag restore test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, instance,
        context);
    if (window != nullptr) {
        ShowWindow(window, SW_MAXIMIZE);
        UpdateWindow(window);
    }
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->window = window;
        context->ready = true;
    }
    context->changed.notify_all();
    if (window == nullptr) {
        return;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

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

void TestMaximizedRestoreMetadataIsPreserved(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;
    using nekodrag::Point;
    using nekodrag::Rect;

    std::atomic<bool> sawRestoreRequest{false};
    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request& request) {
            sawRestoreRequest.store(
                request.restoreMaximized &&
                    request.strategy ==
                        NativeMoveStrategy::NonClientCaption &&
                    request.attempt == 1 &&
                    request.maximizedRect.left == -1920 &&
                    request.maximizedRect.right == 0,
                std::memory_order_release);
            NativeMoveWorker::Result result;
            result.maximizedRestoreAttempted = true;
            result.maximizedRestoreSucceeded = true;
            result.restoredOrigin = {-1500, 200};
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    const HWND target = GetDesktopWindow();
    const Rect maximizedRect{-1920, 0, 0, 1080};
    Expect(context, worker.Submit(
                        {64, target, Point{-900, 500}, nullptr,
                         NativeMoveStrategy::NonClientCaption, 1,
                         target, target, true, true, maximizedRect}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, sawRestoreRequest.load(std::memory_order_acquire));
    Expect(context, result.maximizedRestoreAttempted);
    Expect(context, result.maximizedRestoreSucceeded);
    Expect(context, result.restoredOrigin.x == -1500 &&
                        result.restoredOrigin.y == 200);
    Expect(context, result.dispatched);
}

void TestSecondAttemptDoesNotRequestMaximizedRestore(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;

    std::atomic<bool> sawExpectedRequest{false};
    NativeMoveWorker worker(
        nullptr, 0, [&](const NativeMoveWorker::Request& request) {
            sawExpectedRequest.store(
                request.strategy == NativeMoveStrategy::SystemCommand &&
                    request.attempt == 2 &&
                    !request.cancelInitialInteraction &&
                    !request.restoreMaximized,
                std::memory_order_release);
            NativeMoveWorker::Result result;
            result.dispatched = true;
            return result;
        });
    Expect(context, worker.Start());
    Expect(context, worker.Submit(
                        {65, GetDesktopWindow(), {}, nullptr,
                         NativeMoveStrategy::SystemCommand, 2}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 2000));
    worker.Stop();
    Expect(context, sawExpectedRequest.load(std::memory_order_acquire));
    Expect(context, !result.maximizedRestoreAttempted);
}

void TestProductionRestorePrecedesNativeDispatch(TestContext context) {
    using nekodrag::NativeMoveStrategy;
    using nekodrag::NativeMoveWorker;
    using nekodrag::Point;
    using nekodrag::Rect;

    NativeTargetContext targetContext;
    std::thread targetThread(RunNativeTargetWindow, &targetContext);
    HWND target = nullptr;
    {
        std::unique_lock<std::mutex> lock(targetContext.mutex);
        Expect(context,
               targetContext.changed.wait_for(
                   lock, std::chrono::seconds(2),
                   [&targetContext]() { return targetContext.ready; }));
        target = targetContext.window;
    }
    Expect(context, target != nullptr);
    if (target == nullptr) {
        targetThread.join();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(targetContext.mutex);
        targetContext.events.clear();
    }
    Expect(context, IsZoomed(target) != FALSE);

    RECT maximized{};
    Expect(context, GetWindowRect(target, &maximized) != FALSE);
    NativeMoveWorker worker(nullptr, 0);
    Expect(context, worker.Start());
    const Rect maximizedRect{
        static_cast<std::int32_t>(maximized.left),
        static_cast<std::int32_t>(maximized.top),
        static_cast<std::int32_t>(maximized.right),
        static_cast<std::int32_t>(maximized.bottom)};
    const Point cursor{
        static_cast<std::int32_t>(
            maximized.left + (maximized.right - maximized.left) / 2),
        static_cast<std::int32_t>(
            maximized.top + (maximized.bottom - maximized.top) / 2)};
    Expect(context, worker.Submit(
                        {66, target, cursor, nullptr,
                         NativeMoveStrategy::NonClientCaption, 1,
                         target, target, true, true, maximizedRect}));

    NativeMoveWorker::Result result;
    Expect(context, worker.WaitForResult(&result, 3000));
    worker.Stop();
    Expect(context, result.interactionCancelSucceeded);
    Expect(context, result.maximizedRestoreAttempted);
    Expect(context, result.maximizedRestoreSucceeded);
    Expect(context, result.maximizedRestoreError == ERROR_SUCCESS);
    Expect(context, result.dispatched);

    std::vector<NativeTargetEvent> events;
    {
        std::lock_guard<std::mutex> lock(targetContext.mutex);
        events = targetContext.events;
    }
    const auto cancel = std::find(events.begin(), events.end(),
                                  NativeTargetEvent::CancelMode);
    const auto restore = std::find(events.begin(), events.end(),
                                   NativeTargetEvent::Restore);
    const auto dispatch = std::find(events.begin(), events.end(),
                                    NativeTargetEvent::NonClientMove);
    Expect(context, cancel != events.end());
    Expect(context, restore != events.end());
    Expect(context, dispatch != events.end());
    if (cancel != events.end() && restore != events.end() &&
        dispatch != events.end()) {
        Expect(context, cancel < restore && restore < dispatch);
    }

    PostMessageW(target, WM_CLOSE, 0, 0);
    targetThread.join();
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
    TestMaximizedRestoreMetadataIsPreserved(context);
    TestSecondAttemptDoesNotRequestMaximizedRestore(context);
    TestProductionRestorePrecedesNativeDispatch(context);
    TestStopAcceptingRejectsNewRequests(context);
    TestNewGenerationReplacesStaleResult(context);
    TestBlockedRequestHasBoundedStop(context);
}

}  // namespace native_move_worker_tests
