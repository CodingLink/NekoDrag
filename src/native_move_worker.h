#pragma once

#include "core.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace nekodrag {

// Attempts the target window's native caption move loop away from the
// low-level mouse hook thread. A successful call remains in flight until the
// target's DefWindowProc leaves its move/size modal loop.
class NativeMoveWorker {
  public:
    struct Request {
        std::uint64_t generation = 0;
        HWND target = nullptr;
        Point startCursor{};
        std::shared_ptr<std::atomic_bool> cancelRequested;
        NativeMoveStrategy strategy =
            NativeMoveStrategy::NonClientCaption;
        std::uint32_t attempt = 0;
        HWND initialPressWindow = nullptr;
        HWND captureWindow = nullptr;
        bool cancelInitialInteraction = false;
        bool restoreMaximized = false;
        Rect maximizedRect{};
    };

    struct Result {
        std::uint64_t generation = 0;
        HWND target = nullptr;
        bool dispatched = false;
        DWORD error = ERROR_SUCCESS;
        std::uint64_t elapsedUs = 0;
        NativeMoveStrategy strategy =
            NativeMoveStrategy::NonClientCaption;
        std::uint32_t attempt = 0;
        bool interactionCancelAttempted = false;
        bool interactionCancelSucceeded = false;
        DWORD interactionCancelError = ERROR_SUCCESS;
        bool maximizedRestoreAttempted = false;
        bool maximizedRestoreSucceeded = false;
        DWORD maximizedRestoreError = ERROR_SUCCESS;
        Point restoredOrigin{};
    };

    using MoveFunction = std::function<Result(const Request&)>;

    NativeMoveWorker(HWND completionWindow, UINT completionMessage,
                     MoveFunction moveFunction = {});
    ~NativeMoveWorker();

    NativeMoveWorker(const NativeMoveWorker&) = delete;
    NativeMoveWorker& operator=(const NativeMoveWorker&) = delete;

    bool Start();
    bool Submit(const Request& request);
    void StopAccepting();
    void CancelGeneration(std::uint64_t generation);
    bool TakeLatestResult(Result* result);
    bool WaitForResult(Result* result, DWORD timeoutMs);
    void Stop(DWORD timeoutMs = 250);

  private:
    struct State;

    static Result RunNativeMove(const Request& request);
    static void Run(const std::shared_ptr<State>& state);

    std::shared_ptr<State> state_;
    std::thread thread_;
};

}  // namespace nekodrag
