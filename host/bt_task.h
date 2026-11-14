/**
 * @file   bt_task.h
 * @brief  Bluetooth trigger task for the desktop host (iAP2 -> 0x4301)
 * @author blt
 */

// ============================================================================
// host/bt_task.h -- Bluetooth trigger task (host layer)
// ============================================================================
// Why a separate task: `core::bt::runBtTrigger()` is **fully blocking** and has **no cancel interface**
// (core/iap2/wireless_client.cpp polls every 100 ms until cfg.timeoutMs; after sending 0x4301
//  it still does not return early, it only makes the later timeout report ok=true). Running it directly
// on the SDL main thread would freeze the picture -- and "showing the picture" is exactly why this program exists.
//
// The thread owns the transport and all trigger parameters (moved into the closure in start()),
// so stop() can safely detach. See bt_task.cpp for details.
#pragma once

#include "airplay/server.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace allplay {

struct BtTaskOptions {
    std::string name          = "AllPlay";  ///< Accessory name in the iAP2 identification info (matches the Bonjour name)
    std::string deviceName    = "iPhone";   ///< Substring match on the name
    std::string deviceAddress;              ///< If non-empty, connect directly by address (skips name matching)
    bool        serverMode    = false;      ///< On real Linux/macOS only the client works (we connect to the phone)
    std::string ssid;                       ///< If empty, uses allplay::posix::currentSSID()
    std::string passphrase;                 ///< Wi-Fi password sent to the phone in 0x5703
    std::string identityDir   = "identity"; ///< MFi identity directory
    int         retry         = 10;         ///< Whole-chain retry count (matches Android jni_bridge.cpp:445)
    int         retryDelayMs  = 3000;
};

class BtTask {
public:
    BtTask() = default;
    ~BtTask() { stop(); }
    BtTask(const BtTask&) = delete;
    BtTask& operator=(const BtTask&) = delete;

    /// Start one trigger. **Must be called after rx.start()** -- the parameters use rx.ip()/port()/info().
    /// Returns false if the previous run has not finished (no queueing, no interruption).
    bool start(const core::airplay::Receiver& rx, const BtTaskOptions& o,
               std::function<void(const std::string&)> log);

    /// Whether the previous trigger has finished. false = still running.
    bool done() const { return done_->load(); }

    /// Shut down: set the cancel flag -> the thread exits on its own within 100 ms (sets done to true), then detach.
    /// Call it after the phone leaves so the next start() can trigger CarPlay again -- otherwise, once 0x4301 is sent,
    /// the thread stays on the stale link and never triggers again.
    void stop();

private:
    std::thread                        thread_;
    /// shared_ptr instead of a member bool: the thread must be able to write it without referencing this (see stop()).
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(true);
    /// Cancel flag for this trigger (a fresh one per start(); the old thread keeps the old one, independent).
    std::shared_ptr<std::atomic<bool>> cancel_;
};

}  // namespace allplay
