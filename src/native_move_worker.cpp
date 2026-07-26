#include "native_move_worker.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace nekodrag {
namespace {

constexpr UINT kNativeMoveHangTimeoutMs = 1000;

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
    const LPARAM cursor = MAKELPARAM(
        static_cast<WORD>(static_cast<SHORT>(request.startCursor.x)),
        static_cast<WORD>(static_cast<SHORT>(request.startCursor.y)));
    DWORD_PTR messageResult = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(
        request.target, WM_NCLBUTTONDOWN, HTCAPTION, cursor,
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
        Result result = state->moveFunction(request);
        result.generation = request.generation;
        result.target = request.target;
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
