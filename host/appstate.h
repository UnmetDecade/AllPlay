/**
 * @file   appstate.h
 * @brief  Display state shared across threads
 * @author blt
 */

// ============================================================================
// appstate.h -- Display state shared across threads
// ============================================================================
//
// Threading contract (same as the Plan host design):
//   · Core / Bluetooth / decode threads: write (all via atomic; errorMsg only written when switching to Error)
//   · Main thread (SDL rendering): read
// The main thread never blocks on a lock -- except for the very short errorMsg mutex.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace allplay {

/// Link state (shown in the status bar + drives later window-ownership switches)
enum class LinkState : int {
    Idle = 0,        ///< Just started
    WaitingPair,     ///< Waiting for Bluetooth pairing/trigger (driven by the bt layer since M2)
    Connecting,      ///< Triggered, waiting for the AirPlay session to come up
    Streaming,       ///< Session active, video is streaming
    Error,           ///< Error, see errorMsg
};

inline const char* toString(LinkState s)
{
    switch (s) {
        case LinkState::Idle:        return "IDLE";
        case LinkState::WaitingPair: return "WAIT-BT";
        case LinkState::Connecting:  return "CONNECTING";
        case LinkState::Streaming:   return "STREAMING";
        case LinkState::Error:       return "ERROR";
    }
    return "?";
}

struct SharedStatus {
    std::atomic<int>       state{static_cast<int>(LinkState::Idle)};
    std::atomic<int>       videoW{0};        ///< Current video width (0 = unknown)
    std::atomic<int>       videoH{0};        ///< Current video height
    std::atomic<int>       fps{0};           ///< Decoded display frame rate (1-second window)
    std::atomic<uint64_t>  frames{0};        ///< Total displayed frames
    std::atomic<uint64_t>  lastFrameMs{0};   ///< Wall-clock ms of the latest frame (0 = none yet)
    std::atomic<uint64_t>  audioFrames{0};   ///< Audio frames decoded/received (0 = no audio stream)
    std::atomic<uint64_t>  audioUnderruns{0};///< Audio buffer underruns (>0 means it has stuttered)

    // Updated by the writer only when state switches to Error
    void setError(const std::string& msg)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            errorMsg_ = msg;
        }
        state.store(static_cast<int>(LinkState::Error), std::memory_order_release);
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return errorMsg_;
    }

private:
    mutable std::mutex mu_;
    std::string        errorMsg_;
};

}  // namespace allplay
