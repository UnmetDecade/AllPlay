/**
 * @file   wired_task.h
 * @brief  Wired (USB) CarPlay attach task for the desktop host
 * @author blt
 */

// ============================================================================
// host/wired_task.h -- Wired attach task (host layer)
// ============================================================================
// The same job as BtTask with the other transport, but the **phases are completely different -- do not copy it**:
//
//   Bluetooth: the Receiver comes up first (wildcard bind + mDNS advertising); the trigger just tells the phone "come connect to me".
//   Wired: **the Receiver must wait until the USB side is ready** -- the phone reads the address and port from 0x4301,
//         and that address is the NCM NIC's IPv6 link-local, known only after taking the USB device apart,
//         identifying the NIC by bus/addr and pinning the address onto it. Starting the Receiver first means
//         guessing the address, and a wrong guess broadcasts the wrong address
//         (see the bindAddr comment in core/airplay/server.h).
//
// So it is split into two phases:
//
//   ① beginBringUp() (runs on a background thread): switch to the CarPlay configuration -> usbmux v2 -> lockdown pairing ->
//      in-place TLS -> carkit service -> iAP2 link negotiation (host-initiated) -> identify -> MFi -> power + subscribe
//      -> NCM NIC ready. When it finishes, result() holds ifname / bindAddr -- exactly the
//      two fields ReceiverConfig needs.
//   ② The main thread starts the Receiver with those two fields (**rx is still owned by a single thread**),
//      then startPump() has the background thread wait for 0x4300 -> reply 0x4301.
//
// Why not put both phases on the background thread: the main loop reads rx every frame (running/connecting/port/info...),
// so having another thread call rx.start() is a genuine data race.
//
// Threading contract (matching bt_task.cpp): both threads **touch only what they moved in** (here a
// shared_ptr<Impl> plus by-value snapshot parameters) and never reference this / rx / Options.
// So join in stop() is safe, and WiredTask destruction never leaves a thread on a dangling object.
//
// Platform: the whole chain depends on libusb (platform/posix/usb_libusb.cpp). Without libusb compiled in,
// beginBringUp() sets Failed with a reason, and the host keeps using the wireless path as usual.
#pragma once

#include "airplay/server.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace allplay {

/// Log callback (empty = print to stdout with a [wired] prefix). Same shape as BtTask.
using WiredLogFn = std::function<void(const std::string&)>;

struct WiredTaskOptions {
    std::string name          = "AllPlay";   ///< Head-unit name reported in the iAP2 identification info
    std::string mfiDir        = "identity";  ///< MFi identity directory (identity.pk8 + certificate.p7b)
    /// Where pairing material is stored. Empty = defaultStateDir(). **sudo changes HOME**, so when running as root it must be given explicitly --
    /// otherwise root stores one copy and blt another, i.e. two different "computers", and the trust prompt keeps appearing.
    std::string stateDir;
    /// IPv6 link-local address pinned onto the NCM NIC. Empty = fe80::2 (same as DiPlay's config.linkLocal).
    /// **Do not change casually**: the phone connects using this address from 0x4301; changing it only swaps the value, but both ends must agree.
    std::string ncmLinkLocal;
    int         productId     = -1;          ///< -1 = unrestricted (Apple keyboards/hubs are excluded automatically)
    int         switchWaitMs  = 10000;       ///< Max wait for the device to come back after the configuration switch
};

/// Result of ①: what ReceiverConfig needs + a few log-only fields.
struct WiredBringUp {
    std::string ifname;     ///< NCM NIC name (enx... is derived from the MAC, so it cannot be hard-coded)
    std::string bindAddr;   ///< Link-local address pinned onto it -- **fill it into ReceiverConfig::bindAddr verbatim**
    std::string mac;        ///< NIC MAC (log only)
    std::string serial;     ///< Phone serial number (log only)
    bool unmanaged = false; ///< Whether the NIC was taken away from NetworkManager
    bool pinned    = false; ///< Address was pinned this time (false = the NIC already had it)
};

enum class WiredPhase : int {
    Idle = 0,   ///< Not started
    Working,    ///< ① running (USB / pairing / auth. A locked phone shows a pairing prompt, so wait for the user to tap "Trust")
    Ready,      ///< ① succeeded -- the main thread should now start the Receiver, then startPump()
    Failed,     ///< ① failed, see error()
    Pumping,    ///< ② running (waiting for 0x4300 / 0x4301 already sent)
    Done,       ///< ② finished (link dropped, or stop() called)
};

class WiredTask {
public:
    WiredTask();
    ~WiredTask() { stop(); }
    WiredTask(const WiredTask&) = delete;
    WiredTask& operator=(const WiredTask&) = delete;

    /// ① Start background preparation. Non-blocking, returns immediately. Returns false if the previous run is still in progress (no queueing, no restart-by-interrupt).
    bool beginBringUp(const WiredTaskOptions& o, WiredLogFn log = nullptr);

    WiredPhase phase() const;
    /// Meaningful only after phase() == Ready.
    const WiredBringUp& result() const;
    /// Failure reason / ② shutdown description; empty if none.
    std::string error() const;

    /// ② Wait for 0x4300 -> reply 0x4301. **Must be called after rx.start() succeeds**:
    /// port, deviceid and public key all come from rx (what 0x4301 reports must be what the Receiver itself recognizes).
    /// These values are **snapshotted by value** on the calling thread, so the thread never touches rx again.
    bool startPump(const core::airplay::Receiver& rx, WiredLogFn log = nullptr);

    /// Whether ② has finished (link dropped / shutdown).
    bool pumpDone() const;

    /// Shut down: set the stop flag + join. The pump checks the stop flag every 200 ms and returns quickly;
    /// if ① is still running, this waits for its current blocking call to time out (up to about a minute), in exchange for deterministic destruction order.
    void stop();

private:
    struct Impl;

    /// What ② needs: all snapshotted by value on the **calling thread**, so the thread never touches rx.
    struct PumpParams {
        uint16_t    airPlayPort = 7000;
        /// The **one** address reported to the phone (= the one the Receiver binds). Reporting only one is intentional:
        /// report more and the phone may pick an address we are not listening on, and the connect is refused.
        std::string address;
        std::string deviceId;
        std::string publicKeyHex;
    };

    /// Thread body. A static member so the private nested type Impl can be named and this is never touched.
    static void runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions o, WiredLogFn log);
    static void runPump(std::shared_ptr<Impl> impl, PumpParams pp, WiredLogFn log);

    std::shared_ptr<Impl> impl_;
    std::thread           bringUpThread_;
    std::thread           pumpThread_;
};

}  // namespace allplay
