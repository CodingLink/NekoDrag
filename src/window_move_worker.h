#pragma once

#include "core.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace nekodrag {

// Serializes cross-process window moves away from the low-level hook thread.
// At most one request waits behind the in-flight call; newer coordinates
// replace that pending request.
class WindowMoveWorker {
  public:
    struct Request {
        std::uint64_t generation = 0;
        HWND target = nullptr;
        Point origin{};
    };

    struct Result {
        std::uint64_t generation = 0;
        HWND target = nullptr;
        Point requestedOrigin{};
        Point actualOrigin{};
        bool actualPositionKnown = false;
        bool success = false;
        DWORD error = ERROR_SUCCESS;
        std::uint64_t elapsedUs = 0;
        std::uint64_t coalescedRequests = 0;
    };

    using MoveFunction = std::function<Result(const Request&)>;

    WindowMoveWorker(HWND completionWindow, UINT completionMessage,
                     MoveFunction moveFunction = {});
    ~WindowMoveWorker();

    WindowMoveWorker(const WindowMoveWorker&) = delete;
    WindowMoveWorker& operator=(const WindowMoveWorker&) = delete;

    bool Start();
    bool Submit(const Request& request);
    void StopAccepting();
    void CancelGeneration(std::uint64_t generation);
    bool TakeLatestResult(Result* result);
    bool WaitForResult(Result* result, DWORD timeoutMs);
    void Stop(DWORD timeoutMs = 250);

  private:
    struct State;

    static Result MoveWindowSynchronously(const Request& request);
    static void Run(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace nekodrag
