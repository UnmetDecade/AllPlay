// ============================================================================
// host/bt_task.cpp -- Bluetooth trigger task (host layer)
// ============================================================================
#include "bt_task.h"

#include "bt/bt_trigger.h"
#include "posix/posix.h"

#include <chrono>
#include <cstdio>

namespace allplay {

namespace {

void emit(const std::function<void(const std::string&)>& log, const std::string& s)
{
    if (log) log(s);
    else {
        std::printf("[bt] %s\n", s.c_str());
        std::fflush(stdout);
    }
}

}  // namespace

bool BtTask::start(const core::airplay::Receiver& rx, const BtTaskOptions& o,
                   std::function<void(const std::string&)> log)
{
    if (!done_->load()) return false;   // Previous run still going, do not queue
    // The previous thread has finished but std::thread is still joinable -- assigning a new thread to it is
    // std::terminate (observed symptom: the process aborts on the second/third auto-reconnect trigger,
    // `libc++abi: terminating`). It has already ended, so join is instantaneous.
    if (thread_.joinable()) thread_.join();

    // -- All parameters are snapshotted to values here (on the calling thread) --
    // The thread touches only what it moved in and never references rx / Options / this.
    // This is the precondition for stop() being able to detach safely.
    core::bt::BtTriggerParams bp;
    bp.deviceName    = o.deviceName;
    bp.deviceAddress = o.deviceAddress;
    bp.serverMode    = o.serverMode;
    bp.serverWaitMs  = 300000;

    // Wiring copied from tools/core_selftest.cpp:130-147
    auto& c = bp.iap2;
    c.name             = o.name;
    c.model            = "AllPlay1,1";
    c.manufacturer     = "AllPlay";
    c.serial           = rx.info().deviceId;
    c.firmware         = "1.0";
    c.hardware         = "1.0";
    c.btMac            = allplay::posix::controllerAddress();
    c.ssid             = o.ssid.empty() ? allplay::posix::currentSSID() : o.ssid;
    c.passphrase       = o.passphrase;
    c.ipAddresses      = {rx.ip()};
    c.airPlayPort      = rx.port();
    c.deviceIdentifier = rx.info().deviceId;
    c.publicKeyHex     = rx.info().pkHex;
    c.mfiDir           = o.identityDir;
    c.timeoutMs        = 90000;

    {
        char buf[256];
        std::snprintf(buf, sizeof buf, "Starting trigger: btMac=%s ssid=%s ip=%s port=%u",
                      c.btMac.c_str(), c.ssid.c_str(),
                      c.ipAddresses.empty() ? "?" : c.ipAddresses[0].c_str(),
                      unsigned(rx.port()));
        emit(log, buf);
    }

    auto transport = allplay::posix::makeBluetooth();
    if (!transport) {
        emit(log, "No usable IBluetooth implementation on this platform");
        return false;
    }

    auto done = done_;   // shared_ptr copy: the thread does not need this to write the flag
    done->store(false);
    // Cancel flag: once stop() sets it, runBtTrigger's polling loop exits within 100 ms.
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    bp.iap2.cancel = cancel.get();   // Points into the shared_ptr above; the closure keeps it alive
    thread_ = std::thread(
        [transport = std::move(transport), bp, o, log = std::move(log), done, cancel]() mutable {
            for (int attempt = 1; attempt <= o.retry; ++attempt) {
                if (cancel->load()) {   // No more retries after cancellation
                    done->store(true);
                    return;
                }
                auto r = core::bt::runBtTrigger(transport.get(), bp, log);
                if (r.ok) {
                    done->store(true);
                    return;
                }
                // The iPhone's iAP2 SPP service opens asynchronously after the Bluetooth ACL connects, so a single
                // connectRFCOMM often fails too early -- Android retries the whole chain the same way (jni_bridge.cpp:443-455).
                if (attempt < o.retry) {
                    char buf[256];
                    std::snprintf(buf, sizeof buf, "Attempt %d/%d failed (%s), retrying in %dms",
                                  attempt, o.retry, r.error.c_str(), o.retryDelayMs);
                    emit(log, buf);
                    std::this_thread::sleep_for(std::chrono::milliseconds(o.retryDelayMs));
                } else {
                    emit(log, "Trigger failed (retried " + std::to_string(o.retry) + " times): " + r.error);
                }
            }
            done->store(true);
        });
    return true;
}

void BtTask::stop()
{
    // Set the cancel flag first: runBtTrigger's 100 ms poll sees it and returns, then the thread sets done to true.
    // This lets the main loop's re-scheduling logic trigger again right after the phone leaves; without it, after
    // 0x4301 is sent the thread sits on the stale link forever (observed symptom: CarPlay does not come up even when the phone is brought back).
    if (cancel_) cancel_->store(true);
    // The thread has moved the transport / parameters / log callback into its own closure and does not reference this,
    // so detach is safe -- RFCOMM streams already leak on the "object lifetime = process" model
    // (platform/posix/bt_bluez.cpp:582) and are cleaned up when the process exits.
    //
    // NOTE: std::thread std::terminates if destroyed while still joinable, so detach is not optional.
    if (thread_.joinable()) thread_.detach();
}

}  // namespace allplay
