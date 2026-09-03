#include "native_move_worker.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace nekodrag {
namespace {

constexpr UINT kNativeMoveHangTimeoutMs = 1000;
constexpr UINT kInteractionCancelTimeoutMs = 250;
constexpr UINT kMaximizedRestoreTimeoutMs = 500;

bool IsCancellationRequested(
    const NativeMoveWorker::Request& request) noexcept {
    return request.cancelRequested != nullptr &&
           request.cancelRequested->load(std::memory_order_acquire);
}

bool CancelInitialInteraction(
    const NativeMoveWorker::Request& request, DWORD* error) noexcept {
    const std::array<HWND, 3> candidates{
        request.captureWindow, request.initialPressWindow, request.target};
    std::array<HWND, 3> visited{};
    std::size_t visitedCount = 0;

    for (const HWND candidate : candidates) {
        if (candidate == nullptr || !IsWindow(candidate)) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t index = 0; index < visitedCount; ++index) {
            if (visited[index] == candidate) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        visited[visitedCount++] = candidate;

        if (candidate != request.target &&
            GetAncestor(candidate, GA_ROOT) != request.target) {
            continue;
        }

        DWORD_PTR ignored = 0;
        SetLastError(ERROR_SUCCESS);
        const LRESULT sent = SendMessageTimeoutW(
            candidate, WM_CANCELMODE, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            kInteractionCancelTimeoutMs, &ignored);
        if (sent == 0) {
            const DWORD sendError = GetLastError();
            *error = sendError == ERROR_SUCCESS ? ERROR_TIMEOUT : sendError;
            return false;
        }
    }
    *error = ERROR_SUCCESS;
    return true;
}

bool RestoreMaximizedWindow(const NativeMoveWorker::Request& request,
                            NativeMoveWorker::Result* result) noexcept {
    result->maximizedRestoreAttempted = true;
    DWORD_PTR ignored = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT restored = SendMessageTimeoutW(
        request.target, WM_SYSCOMMAND, SC_RESTORE, 0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        kNativeMoveHangTimeoutMs, &ignored);
    if (restored == 0) {
        const DWORD restoreError = GetLastError();
        result->maximizedRestoreError =
            restoreError == ERROR_SUCCESS ? ERROR_TIMEOUT : restoreError;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              kMaximizedRestoreTimeoutMs);
    while (IsWindow(request.target) && IsZoomed(request.target) &&
           std::chrono::steady_clock::now() < deadline) {
        if (IsCancellationRequested(request)) {
            result->maximizedRestoreError = ERROR_CANCELLED;
            return false;
        }
        Sleep(10);
    }
    if (!IsWindow(request.target)) {
        result->maximizedRestoreError = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    if (IsZoomed(request.target)) {
        result->maximizedRestoreError = ERROR_TIMEOUT;
        return false;
    }

    result->maximizedRestoreSucceeded = true;
    RECT restoredRect{};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowRect(request.target, &restoredRect)) {
        result->maximizedRestoreError = GetLastError();
        if (result->maximizedRestoreError == ERROR_SUCCESS) {
            result->maximizedRestoreError = ERROR_GEN_FAILURE;
        }
        return false;
    }
    const Size restoredSize{
        static_cast<std::int32_t>(restoredRect.right - restoredRect.left),
        static_cast<std::int32_t>(restoredRect.bottom - restoredRect.top)};
    const Point desiredOrigin = ComputeRestoredOrigin(
        request.startCursor, request.maximizedRect, restoredSize);
    if (IsHungAppWindow(request.target)) {
        result->maximizedRestoreError = ERROR_TIMEOUT;
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowPos(request.target, nullptr, desiredOrigin.x,
                      desiredOrigin.y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_NOOWNERZORDER | SWP_DEFERERASE)) {
        result->maximizedRestoreError = GetLastError();
        if (result->maximizedRestoreError == ERROR_SUCCESS) {
            result->maximizedRestoreError = ERROR_GEN_FAILURE;
        }
        return false;
    }
    RECT actualRect{};
    result->restoredOrigin = desiredOrigin;
    if (GetWindowRect(request.target, &actualRect)) {
        result->restoredOrigin = {
            static_cast<std::int32_t>(actualRect.left),
            static_cast<std::int32_t>(actualRect.top)};
    }
    return true;
}

}  // namespace

struct NativeMoveWorker::State {
    std::mutex mutex;
    std::condition_variable workReady;
    std::condition_variable resultReady;
    std::optional<Request> pendingRequest;
    std::optional<Result> latestResult;
    MoveFunction moveFunction;
    HWND completionWindow = nullptr;
    UINT completionMessage = 0;
    bool completionMessagePending = false;
    bool accepting = false;
    bool busy = false;
    bool running = false;
    bool stopping = false;
};

NativeMoveWorker::NativeMoveWorker(HWND completionWindow,
                                   UINT completionMessage,
                                   MoveFunction moveFunction)
    : state_(std::make_shared<State>()) {
    state_->completionWindow = completionWindow;
    state_->completionMessage = completionMessage;
    if (moveFunction) {
        state_->moveFunction = std::move(moveFunction);
    } else {
        state_->moveFunction = RunNativeMove;
    }
}

NativeMoveWorker::~NativeMoveWorker() {
    Stop();
}

bool NativeMoveWorker::Start() {
    if (thread_.joinable()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) {
            return false;
        }
        state_->accepting = true;
        state_->running = true;
    }
    try {
        const std::shared_ptr<State> state = state_;
        thread_ = std::thread([state]() { Run(state); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        state_->running = false;
        return false;
    }
    return true;
}

bool NativeMoveWorker::Submit(const Request& request) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting || !state_->running || state_->stopping ||
            state_->busy) {
            return false;
        }
        state_->busy = true;
        state_->pendingRequest = request;
    }
    state_->workReady.notify_one();
    return true;
}

void NativeMoveWorker::StopAccepting() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    if (state_->pendingRequest.has_value()) {
        state_->pendingRequest.reset();
        state_->busy = false;
    }
    state_->completionWindow = nullptr;
}

void NativeMoveWorker::CancelGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pendingRequest.has_value() &&
        state_->pendingRequest->generation == generation) {
        state_->pendingRequest.reset();
        state_->busy = false;
    }
}

bool NativeMoveWorker::TakeLatestResult(Result* result) {
    if (result == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->latestResult.has_value()) {
        state_->completionMessagePending = false;
        return false;
    }
    *result = *state_->latestResult;
    state_->latestResult.reset();
    state_->completionMessagePending = false;
    return true;
}

bool NativeMoveWorker::WaitForResult(Result* result, DWORD timeoutMs) {
    if (result == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    const std::shared_ptr<State> state = state_;
    const bool ready = state_->resultReady.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [state]() {
            return state->latestResult.has_value() || !state->running;
        });
    if (!ready || !state_->latestResult.has_value()) {
        return false;
    }
    *result = *state_->latestResult;
    state_->latestResult.reset();
    state_->completionMessagePending = false;
    return true;
}

void NativeMoveWorker::Stop(DWORD timeoutMs) {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        state_->stopping = true;
        if (state_->pendingRequest.has_value()) {
            state_->pendingRequest.reset();
            state_->busy = false;
        }
        state_->completionWindow = nullptr;
    }
    state_->workReady.notify_all();

    const DWORD waitResult =
        WaitForSingleObject(thread_.native_handle(), timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        thread_.join();
    } else {
        // The shared state owns everything used by the blocked sender. This
        // keeps shutdown bounded even if a target stops pumping messages.
        thread_.detach();
    }
}

NativeMoveWorker::Result NativeMoveWorker::RunNativeMove(
    const Request& request) {
    Result result;
    if (request.target == nullptr || !IsWindow(request.target)) {
        result.error = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }
    if (request.cancelInitialInteraction) {
        result.interactionCancelAttempted = true;
        result.interactionCancelSucceeded = CancelInitialInteraction(
            request, &result.interactionCancelError);
        if (!result.interactionCancelSucceeded) {
            result.error = result.interactionCancelError;
            return result;
        }
    }
    if (IsCancellationRequested(request)) {
        result.error = ERROR_CANCELLED;
        return result;
    }
    if (request.restoreMaximized &&
        !RestoreMaximizedWindow(request, &result)) {
        result.error = result.maximizedRestoreError;
        return result;
    }
    if (IsCancellationRequested(request)) {
        result.error = ERROR_CANCELLED;
        return result;
    }
    const LPARAM cursor = MAKELPARAM(
        static_cast<WORD>(static_cast<SHORT>(request.startCursor.x)),
        static_cast<WORD>(static_cast<SHORT>(request.startCursor.y)));
    DWORD_PTR messageResult = 0;
    if (IsCancellationRequested(request)) {
        result.error = ERROR_CANCELLED;
        return result;
    }
    SetLastError(ERROR_SUCCESS);
    UINT message = 0;
    WPARAM wParam = 0;
    switch (request.strategy) {
        case NativeMoveStrategy::NonClientCaption:
            message = WM_NCLBUTTONDOWN;
            wParam = HTCAPTION;
            break;
        case NativeMoveStrategy::SystemCommand:
            message = WM_SYSCOMMAND;
            wParam = SC_MOVE | HTCAPTION;
            break;
        default:
            result.error = ERROR_INVALID_PARAMETER;
            return result;
    }
    const LRESULT sent = SendMessageTimeoutW(
        request.target, message, wParam, cursor,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_NOTIMEOUTIFNOTHUNG |
            SMTO_ERRORONEXIT,
        kNativeMoveHangTimeoutMs, &messageResult);
    if (sent == 0) {
        const DWORD sendError = GetLastError();
        result.error = sendError == ERROR_SUCCESS ? ERROR_TIMEOUT : sendError;
        return result;
    }

    result.dispatched = true;
    return result;
}

void NativeMoveWorker::Run(const std::shared_ptr<State>& state) {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->workReady.wait(lock, [&state]() {
                return state->stopping || state->pendingRequest.has_value();
            });
            if (state->stopping) {
                break;
            }
            request = *state->pendingRequest;
            state->pendingRequest.reset();
        }

        const auto startedAt = std::chrono::steady_clock::now();
        Result result;
        if (IsCancellationRequested(request) &&
            !request.cancelInitialInteraction) {
            result.error = ERROR_CANCELLED;
        } else {
            result = state->moveFunction(request);
        }
        result.generation = request.generation;
        result.target = request.target;
        result.strategy = request.strategy;
        result.attempt = request.attempt;
        result.elapsedUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startedAt)
                .count());

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->busy = false;
            const bool replaceResult =
                !state->latestResult.has_value() ||
                result.generation >= state->latestResult->generation;
            if (replaceResult) {
                state->latestResult = result;
            }
            if (state->completionWindow != nullptr &&
                state->completionMessage != 0 &&
                !state->completionMessagePending) {
                state->completionMessagePending = true;
                if (!PostMessageW(state->completionWindow,
                                  state->completionMessage, 0, 0)) {
                    state->completionMessagePending = false;
                }
            }
        }
        state->resultReady.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->busy = false;
        state->running = false;
    }
    state->resultReady.notify_all();
}

}  // namespace nekodrag
