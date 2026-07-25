#include "window_move_worker.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace nekodrag {

struct WindowMoveWorker::State {
    std::mutex mutex;
    std::condition_variable workReady;
    std::condition_variable resultReady;
    std::optional<Request> pendingRequest;
    std::optional<Result> latestResult;
    MoveFunction moveFunction;
    HWND completionWindow = nullptr;
    UINT completionMessage = 0;
    std::uint64_t coalescedRequests = 0;
    bool completionMessagePending = false;
    bool accepting = false;
    bool running = false;
    bool stopping = false;
};

WindowMoveWorker::WindowMoveWorker(HWND completionWindow,
                                   UINT completionMessage,
                                   MoveFunction moveFunction)
    : state_(std::make_shared<State>()) {
    state_->completionWindow = completionWindow;
    state_->completionMessage = completionMessage;
    if (moveFunction) {
        state_->moveFunction = std::move(moveFunction);
    } else {
        state_->moveFunction = MoveWindowSynchronously;
    }
}

WindowMoveWorker::~WindowMoveWorker() {
    Stop();
}

bool WindowMoveWorker::Start() {
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
        state_->running = false;
        return false;
    }
    return true;
}

bool WindowMoveWorker::Submit(const Request& request) {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting || !state_->running || state_->stopping) {
            return false;
        }
        if (state_->pendingRequest.has_value()) {
            ++state_->coalescedRequests;
        }
        state_->pendingRequest = request;
    }
    state_->workReady.notify_one();
    return true;
}

void WindowMoveWorker::StopAccepting() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    state_->pendingRequest.reset();
    state_->coalescedRequests = 0;
    state_->completionWindow = nullptr;
}

void WindowMoveWorker::CancelGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->pendingRequest.has_value() &&
        state_->pendingRequest->generation == generation) {
        state_->pendingRequest.reset();
        state_->coalescedRequests = 0;
    }
}

bool WindowMoveWorker::TakeLatestResult(Result* result) {
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

bool WindowMoveWorker::WaitForResult(Result* result, DWORD timeoutMs) {
    if (result == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    const bool ready = state_->resultReady.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this]() {
            return state_->latestResult.has_value() || !state_->running;
        });
    if (!ready || !state_->latestResult.has_value()) {
        return false;
    }
    *result = *state_->latestResult;
    state_->latestResult.reset();
    state_->completionMessagePending = false;
    return true;
}

void WindowMoveWorker::Stop(DWORD timeoutMs) {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        state_->stopping = true;
        state_->pendingRequest.reset();
        state_->completionWindow = nullptr;
    }
    state_->workReady.notify_all();

    const DWORD waitResult =
        WaitForSingleObject(thread_.native_handle(), timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        thread_.join();
    } else {
        // The worker owns the shared state. If a target blocks SetWindowPos,
        // detaching keeps shutdown bounded without leaving a dangling app
        // pointer; process teardown will release the blocked OS thread.
        thread_.detach();
    }
}

WindowMoveWorker::Result WindowMoveWorker::MoveWindowSynchronously(
    const Request& request) {
    Result result;
    if (request.target == nullptr || !IsWindow(request.target)) {
        result.error = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }
    if (IsHungAppWindow(request.target)) {
        result.error = ERROR_TIMEOUT;
        return result;
    }

    SetLastError(ERROR_SUCCESS);
    if (!SetWindowPos(request.target, nullptr, request.origin.x,
                      request.origin.y, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_NOOWNERZORDER | SWP_DEFERERASE)) {
        result.error = GetLastError();
        if (result.error == ERROR_SUCCESS) {
            result.error = ERROR_GEN_FAILURE;
        }
        return result;
    }

    result.success = true;
    RECT actual{};
    if (GetWindowRect(request.target, &actual)) {
        result.actualOrigin = {
            static_cast<std::int32_t>(actual.left),
            static_cast<std::int32_t>(actual.top),
        };
        result.actualPositionKnown = true;
    }
    return result;
}

void WindowMoveWorker::Run(const std::shared_ptr<State>& state) {
    for (;;) {
        Request request;
        std::uint64_t coalescedRequests = 0;
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
            coalescedRequests = state->coalescedRequests;
            state->coalescedRequests = 0;
        }

        const auto startedAt = std::chrono::steady_clock::now();
        Result result = state->moveFunction(request);
        result.generation = request.generation;
        result.target = request.target;
        result.requestedOrigin = request.origin;
        result.elapsedUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - startedAt)
                .count());
        result.coalescedRequests = coalescedRequests;

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const bool replaceResult =
                !state->latestResult.has_value() ||
                result.generation > state->latestResult->generation ||
                (result.generation == state->latestResult->generation &&
                 (!result.success || state->latestResult->success));
            if (replaceResult) {
                state->latestResult = result;
            }
            if (state->completionWindow != nullptr &&
                state->completionMessage != 0 &&
                !state->completionMessagePending) {
                state->completionMessagePending = true;
                // Post while holding the state lock so StopAccepting cannot
                // invalidate the completion window between the check and the
                // non-blocking queue operation.
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
        state->running = false;
    }
    state->resultReady.notify_all();
}

}  // namespace nekodrag
