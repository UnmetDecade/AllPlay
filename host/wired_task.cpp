// ============================================================================
// host/wired_task.cpp -- Wired attach task (host layer)
// ============================================================================
// Where the two phases live:
//   ① runBringUp() -- steps ①-⑦ from tools/wired_probe.cpp, carried over without skipping any.
//      The probe lets each step stop midway for inspection; here it runs through to ifname/bindAddr.
//   ② runPump()    -- the probe's ⑧: wait for 0x4300 -> reply 0x4301 -> then keep pumping (keepalive).
//
// The only hand-off point between the two phases is the three unique_ptrs in Impl (pipe / control / link):
// they are the entire evidence that "the phone still thinks we are in the car"; lose them and you start over.
#include "wired_task.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>

// These headers are **not put under conditional compilation**: Impl's type and the class itself must be complete in both builds.
// Only the implementation of openWiredCarPlay() (usb_libusb.cpp) truly depends on libusb,
// and its absence is a link error rather than a compile error -- so conditional compilation is opened only at the function-body level.
#include "iap2/wired_link.h"
#include "mfi/mfi_identity.h"
#include "posix/posix.h"
#include "posix/usb.h"
#include "usb/wired_control.h"

namespace allplay {

namespace {

/// Format one log line. Empty log means print to stdout (same meaning as emit in bt_task.cpp).
void emit(const WiredLogFn& log, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log) log(buf);
    else {
        std::printf("[wired] %s\n", buf);
        std::fflush(stdout);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl: all state. Both threads touch it only through a shared_ptr, so destroying WiredTask does not mean
//       the thread dangles -- this is the basis for daring to join in stop().
// ---------------------------------------------------------------------------
struct WiredTask::Impl {
    std::atomic<int>  phase{int(WiredPhase::Idle)};
    std::atomic<bool> stop{false};

    WiredBringUp bringUp;

    // What ② also uses. **Declaration order is destruction order**, do not reorder:
    // WiredControl stores a reference to pipe, WiredLink stores a reference to the service() stream.
    std::unique_ptr<core::IByteStream>       pipe;
    std::unique_ptr<core::usb::WiredControl> control;
    std::unique_ptr<core::iap2::WiredLink>   link;

    mutable std::mutex mu;
    std::string        err;

    void setError(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(mu);
        err = s;
    }
    std::string getError() const
    {
        std::lock_guard<std::mutex> lk(mu);
        return err;
    }
};

WiredTask::WiredTask() : impl_(std::make_shared<Impl>()) {}

WiredPhase WiredTask::phase() const { return WiredPhase(impl_->phase.load()); }

const WiredBringUp& WiredTask::result() const { return impl_->bringUp; }

std::string WiredTask::error() const { return impl_->getError(); }

#if defined(ALLPLAY_HAVE_LIBUSB) || defined(__APPLE__)

// ---------------------------------------------------------------------------
// ① USB control plane + media plane preparation
// ---------------------------------------------------------------------------
void WiredTask::runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions o, WiredLogFn log)
{
    auto fail = [&](const std::string& why) {
        emit(log, "✗ %s", why.c_str());
        impl->setError(why);
        impl->phase.store(int(WiredPhase::Failed));
    };

    // ---- ① Transport layer ----
    // Linux: switch the USB configuration, claim the USBMUX interface, and run usbmux v2 ourselves.
    // macOS: no USB handling; borrow the system usbmuxd tunnel (AppleUSBNCMData creates the NIC itself).
    allplay::posix::WiredUsbInfo info;
    std::string                  err;
    std::unique_ptr<core::IByteStream> pipe;   // Linux only; left empty on macOS

#ifdef __APPLE__
    (void)o.productId;   // macOS does not enumerate USB, so restricting the product ID is meaningless
    std::unique_ptr<core::usb::IMux> sysMux = allplay::posix::openSystemUsbMuxd(&info, &err);
    if (!sysMux) {
        fail("Failed to attach to system usbmuxd: " + err);
        return;
    }
    auto control = std::make_unique<core::usb::WiredControl>(std::move(sysMux));
    emit(log, "System usbmuxd took over (%s) — not switching configuration, the control plane uses Apple's own tunnel",
         info.serial.c_str());
#else
    allplay::posix::WiredUsbOptions uopt;
    if (o.productId > 0) uopt.productId = o.productId;
    uopt.switchWaitMs = o.switchWaitMs;

    pipe = allplay::posix::openWiredCarPlay(uopt, &info, &err);
    if (!pipe) {
        fail("Failed to open USB: " + err);
        return;
    }
    emit(log, "USB entered CarPlay configuration (%s, bus %u addr %u, config %d)", info.serial.c_str(),
         unsigned(info.bus), unsigned(info.address), info.configValue);

    auto control = std::make_unique<core::usb::WiredControl>(*pipe);
#endif

    // ---- ② Control plane: lockdown pairing -> session -> in-place TLS -> carkit service ----
    // Failures here are often "the phone is locked" (only 1 configuration reported) or "nobody tapped the pairing prompt";
    // neither is a code problem -- so pass err through verbatim instead of inventing a vague message.
    // Pairing material is stored and reused per phone serial -- without it, every run gets a new HostID and certificate,
    // the phone treats us as a stranger every time, and **prompts "Trust This Computer?" every time**. info.serial is the USB
    // descriptor serial number, available before DevicePublicKey is read, which makes it a good key.
    control->setPairStore(info.serial, o.stateDir);
    if (!control->connect(&err)) {
        fail("Control plane failed: " + err);
        return;
    }
    emit(log, "Control plane established (HostID=%s)", control->hostId().c_str());

    // ---- ③ iAP2 link negotiation (**host-initiated**: marker + SYN, cannot wait for the phone to speak first) ----
    auto link = std::make_unique<core::iap2::WiredLink>(*control->service());
    std::string lerr;
    if (!link->negotiate(30000, &lerr)) {
        fail("iAP2 link negotiation failed: " + lerr);
        return;
    }
    emit(log, "Link NORMAL (peer max payload %zu bytes)", link->engine().peerMaxPayload());

    // ---- ④ Identify (0x1D00 -> 0x1D01 -> 0x1D02) ----
    // **Must be done right after** the link is NORMAL: this is what the phone waits for; idle for ~44 seconds and it resets USB.
    core::iap2::WiredConfig wcfg;
    wcfg.name = o.name;
    // The USBHostTransport group needs the phone-side CarPlay interface number, i.e. the USBMUX interface we identified.
    // Hard-coding it to 1 just happens to match this machine.
    wcfg.carPlayUsbInterfaceNumber = int(info.usbMuxInterface);
    if (!link->identify(wcfg, &lerr)) {
        fail("Identify failed: " + lerr);
        return;
    }
    emit(log, "Identify succeeded (USBHostTransport interface number %d)", wcfg.carPlayUsbInterfaceNumber);

    // ---- ⑤ MFi authentication ----
    auto mfi = core::mfi::CertificateMfiIdentity::load(o.mfiDir, &err);
    if (!mfi) {
        fail("Failed to load MFi identity (" + o.mfiDir + "): " + err);
        return;
    }
    if (!link->authenticate(mfi.get(), 30000, &lerr)) {
        fail("MFi authentication failed: " + lerr);
        return;
    }
    emit(log, "MFi authentication succeeded (certificate %zu bytes)", mfi->certificate().size());

    // ---- ⑥ Power + 5 subscriptions ----
    // These two are not formalities: on the wired path the phone really is powered by us, and without subscriptions it pushes no state.
    link->sendPowerSourceUpdate(wcfg.availableCurrentMilliAmps, /*charging=*/true);
    link->sendSubscriptions();
    emit(log, "Reported power %u mA and 5 subscriptions", unsigned(wcfg.availableCurrentMilliAmps));

    // ---- ⑦ Media plane: the NIC exposed by the iPhone-side NCM function ----
    // On Linux it is created by the kernel cdc_ncm and we only pin the address; on macOS it is AppleUSBNCMData
    // that creates it, and the system provides the address too (found in IORegistry by serial). Both paths use only
    // "NIC name + IPv6 link-local"; NTB16/Ethernet/IPv6 themselves are not our concern.
    allplay::posix::WiredNcmLink ncm;
#ifdef __APPLE__
    const bool ncmOk = allplay::posix::prepareWiredNcmDarwin(info.serial, &ncm, &err);
#else
    const bool ncmOk = allplay::posix::prepareWiredNcm(int(info.bus), int(info.address),
                                                       o.ncmLinkLocal, &ncm, &err);
#endif
    if (!ncmOk) {
        fail("Wired media plane failed to start: " + err);
        return;
    }
    if (ncm.ipv6.empty()) {   // both sides guarantee non-empty; this just keeps front() from running off the end
        fail("NCM NIC " + ncm.ifname + " has no IPv6 link-local address");
        return;
    }

    impl->bringUp.ifname    = ncm.ifname;
    impl->bringUp.bindAddr  = ncm.ipv6.front();   // the pinned one is always first
    impl->bringUp.mac       = ncm.mac;
    impl->bringUp.serial    = info.serial;
    impl->bringUp.unmanaged = ncm.unmanaged;
    impl->bringUp.pinned    = ncm.pinned;

    emit(log, "NCM NIC %s: MAC %s, IPv6 %s%s", ncm.ifname.c_str(), ncm.mac.c_str(),
         ncm.ipv6.front().c_str(), ncm.pinned ? " (pinned this run)" : "");
    if (ncm.unmanaged) emit(log, "Detached the NIC from NetworkManager (otherwise it would request DHCP)");

    // ---- Hand over to ② ----
    // **First tear down the previous round's three in destruction order, then install the new ones.** Do not just assign in sequence: then
    // `impl->control = ...` destroys the old control first (the TLS stream lives inside it), while the old link still
    // holds a reference to that stream -- destroying link right after steps on a destructed object and segfaults.
    // On the first run the old three are all empty, so this wrong path never surfaced in the original design; "reconnect from scratch"
    // is the first thing that makes this run a second time. (Order is the reverse of Impl's member declarations: link -> control -> pipe.)
    impl->link.reset();
    impl->control.reset();
    impl->pipe.reset();

    impl->pipe    = std::move(pipe);
    impl->control = std::move(control);
    impl->link    = std::move(link);
    impl->phase.store(int(WiredPhase::Ready));
    emit(log, "USB side ready — time to start the Receiver");
}

// ---------------------------------------------------------------------------
// ② Wait for 0x4300 -> reply 0x4301 -> keepalive
// ---------------------------------------------------------------------------
void WiredTask::runPump(std::shared_ptr<Impl> impl, PumpParams pp, WiredLogFn log)
{
    core::iap2::WiredCarPlayEndpoint ep;
    ep.ipv6Addresses.push_back(pp.address);
    ep.airPlayPort      = pp.airPlayPort;
    ep.deviceIdentifier = pp.deviceId;
    ep.publicKeyHex     = pp.publicKeyHex;
    ep.sourceVersion    = core::airplay::kSourceVersion;   // must not be empty, the phone picks a version

    bool     sent = false;
    uint64_t beat = core::iap2::monoMs();

    while (!impl->stop.load()) {
        std::vector<core::iap2::WiredLink::Message> msgs;
        if (!impl->link->pump(200, &msgs)) {
            emit(log, "iAP2 link disconnected");
            break;
        }
        for (const auto& m : msgs) {
            if (m.id != 0x4300) {
                emit(log, "Control frame 0x%04x, %zu bytes", unsigned(m.id), m.body.size());
                continue;
            }
            core::iap2::CarPlayAvailability av;
            if (core::iap2::parseCarPlayAvailability(m.body, &av))
                emit(log, "Received 0x4300: %s", core::iap2::describeCarPlayAvailability(av).c_str());
            else
                emit(log, "Received 0x4300 (parse failed, %zu bytes)", m.body.size());

            if (sent) continue;   // the phone may ask repeatedly; reply once and stop (DiPlay also replies only once)

            // **Do not filter by available**: DiPlay replies on receipt, leaving availability to the phone.
            std::string serr;
            if (!impl->link->sendCarPlayStartSession(ep, &serr)) {
                emit(log, "✗ Failed to send 0x4301: %s", serr.c_str());
                impl->setError("Failed to send 0x4301: " + serr);
                impl->phase.store(int(WiredPhase::Done));
                return;
            }
            sent = true;
            emit(log, "Replied 0x4301 — waiting for the phone to connect AirPlay at [%s]:%u", pp.address.c_str(),
                 unsigned(pp.airPlayPort));
        }
        // Heartbeat: one line every 10 seconds. Waiting for 0x4300 can take a while (the phone has to start its CarPlay session first),
        // and printing nothing at all looks like a hang.
        if (core::iap2::monoMs() - beat >= 10000) {
            beat = core::iap2::monoMs();
            emit(log, "%s", sent ? "0x4301 sent, keeping link alive" : "waiting for the phone's 0x4300");
        }
    }
    impl->phase.store(int(WiredPhase::Done));
}

#else  // neither libusb nor macOS

// This build has no wired transport layer: the interface is all there, but calling it reports a clear error, so the host needs no conditional compilation for it.
void WiredTask::runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions, WiredLogFn)
{
    impl->setError("This build has no wired transport layer (install libusb-1.0-dev on Linux and re-run cmake)");
    impl->phase.store(int(WiredPhase::Failed));
}

void WiredTask::runPump(std::shared_ptr<Impl>, PumpParams, WiredLogFn) {}

#endif  // ALLPLAY_HAVE_LIBUSB / __APPLE__

// ---------------------------------------------------------------------------
// Public interface (both halves are here; conditional compilation is only above)
// ---------------------------------------------------------------------------
bool WiredTask::beginBringUp(const WiredTaskOptions& o, WiredLogFn log)
{
    int expect = int(WiredPhase::Idle);
    if (!impl_->phase.compare_exchange_strong(expect, int(WiredPhase::Working))) {
        // Failed / Done allow a restart (re-running after tapping "Trust" on the phone is routine),
        // while running (Working / Ready / Pumping) is not interrupted.
        if (expect == int(WiredPhase::Failed) || expect == int(WiredPhase::Done)) {
            if (bringUpThread_.joinable()) bringUpThread_.join();
            // Also retire the previous round's pump thread. A phase of Done means it has exited its loop, but the thread object
            // is still dangling unjoined -- without joining, when the new round installs pipe/control/link, it could in theory
            // still touch those references as they are being swapped out (see the comment at the end of runBringUp).
            if (pumpThread_.joinable()) pumpThread_.join();
            impl_->setError(std::string());
            impl_->phase.store(int(WiredPhase::Working));
        } else {
            return false;
        }
    }
    impl_->stop.store(false);

    auto impl = impl_;
    bringUpThread_ = std::thread(
        [impl, o, log]() mutable { runBringUp(impl, o, std::move(log)); });
    return true;
}

bool WiredTask::startPump(const core::airplay::Receiver& rx, WiredLogFn log)
{
    if (impl_->phase.load() != int(WiredPhase::Ready)) return false;

    // Everything from rx is taken as values here (on the calling thread) -- the thread never touches rx again.
    PumpParams pp;
    pp.airPlayPort  = rx.port();
    pp.deviceId     = rx.info().deviceId;
    pp.publicKeyHex = rx.info().pkHex;
    // The address reported to the phone must be the one the Receiver **actually bound**: same deviceid, same public key,
    // same port. Reporting one thing while listening on another is the first thing the phone finds mismatched on connect.
    pp.address = impl_->bringUp.bindAddr;

    if (pumpThread_.joinable()) pumpThread_.join();
    impl_->stop.store(false);
    impl_->phase.store(int(WiredPhase::Pumping));

    auto impl = impl_;
    pumpThread_ = std::thread(
        [impl, pp, log]() mutable { runPump(impl, std::move(pp), std::move(log)); });
    return true;
}

bool WiredTask::pumpDone() const { return impl_->phase.load() == int(WiredPhase::Done); }

void WiredTask::stop()
{
    impl_->stop.store(true);
    if (pumpThread_.joinable())    pumpThread_.join();
    if (bringUpThread_.joinable()) bringUpThread_.join();
    // Once the stop flag is set, runBringUp also reaches Done when it wakes -- do not push it back to Failed,
    // or the host reports a spurious "wired failure" during shutdown.
    if (impl_->phase.load() == int(WiredPhase::Working)) impl_->phase.store(int(WiredPhase::Done));
}

}  // namespace allplay
