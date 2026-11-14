// ============================================================================
// tools/wired_probe.cpp -- wired CarPlay control-plane probe (the stage after switching config)
// ============================================================================
// tools/usb_probe.cpp verifies "can we switch into the CarPlay configuration" (USB side, Stage 0).
// This one verifies **can we talk after switching in**:
//
//   USBMUX port (bulk) -> usbmux v2 handshake -> mini-TCP -> phone:62078 (lockdown)
//                                             -> send a QueryType -> read back a plist
//
// Why a separate tool: this stage (usbmux v2 + mini-TCP + lockdown plist channel) is the **only
// part that can't be confirmed by reading source** -- frame length, sequence numbers, port reuse are all byte-level conventions; one byte off
// hangs waiting for a reply, and the hang looks exactly like "device not plugged in" (timeout). So get this working on its own first,
// and only then write iAP2, so the two classes of problem don't get tangled when debugging.
//
// Usage:
//   wired_probe                open the link -> usbmux handshake -> connect to 62078 -> print the phone's reaction
//   wired_probe --query        after connecting, send a lockdown QueryType and print the reply verbatim
//   wired_probe --pair         run the full control plane: pairing -> session -> TLS -> carkit service,
//                              then dump the bytes the phone sends on the iAP2 stream
//   wired_probe --link         with --pair: after obtaining the carkit service stream, run the **real** iAP2 link negotiation
//                              (marker + SYN, host-initiated; see core/iap2/wired_link.h)
//   wired_probe --marker       with --pair: send only the 6-byte iAP2 start marker (FF 55 02 00 EE 10),
//                              then just wait. **This is the earliest naive approach; on real hardware the phone says nothing** --
//                              kept because it proves the converse: "sending only the marker isn't enough, we must send SYN ourselves"
//   wired_probe --port N       change port (default 62078)
//   wired_probe --hold         after it works, stop and wait for Enter (handy for checking ip -br link in another terminal)
//   wired_probe --stay         after it works, keep reading and dump subsequent bytes from the phone
//
// Before running (Linux desktop; **all three are required**, or set_configuration returns BUSY):
//   sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd
//   pkill -f gvfsd-gphoto2
//   (The third -- kernel driver ipheth -- this program detaches itself; no action needed)
// Also /dev/bus/usb/* is root-only, so **the whole program must run under sudo**.
//
// On macOS **nothing needs stopping**: the control plane goes through the system usbmuxd (the same tunnel Finder uses),
// the media-plane NIC is created by AppleUSBNCMData, and the program needs no sudo. Prerequisite: this Mac must have been
// trusted by the phone in Finder. Correspondingly, --pid / --wait (the "switch config" options) do nothing on macOS.
#include "airplay/info.h"
#include "airplay/server.h"
#include "iap2/wired_link.h"
#include "mfi/mfi_identity.h"
#include "platform/device_identity.h"
#include "posix/posix.h"
#include "posix/usb.h"
#include "usb/usbmux.h"
#include "usb/wired_control.h"

#include "common/log.h"

#include "port/media.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/// tools/wired_selftest.cpp: the offline self-test that needs no phone
int wiredSelfTest();

namespace {

/// Video output that only counts, never presents.
/// This probe needs to prove "**the stream really arrives from the phone**", not "whether the picture looks good" --
/// decoding/presenting is the host's job (host/media/sdl_video_sink), and wiring it in here would drag in SDL.
/// The consequence of not providing IVideoOutput is concrete: in SETUP the phone asks for a screen stream, we give no dataPort,
/// and the phone keeps POSTing /feedback waiting forever (that's the WARN at session.cpp:379).
class CountingVideoSink : public core::IVideoOutput {
public:
    void configure(core::VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& codecData) override
    {
        log("video config: %s %dx%d, codecData %zu bytes",
            codec == core::VideoCodec::HEVC ? "HEVC" : "H.264", width, height, codecData.size());
    }

    bool feedAnnexB(const uint8_t* data, size_t len) override
    {
        (void)data;   // count only, no decoding; the pointer itself isn't used (real decoding is in host/media)
        ++frames_;
        bytes_ += len;
        if (frames_ == 1 || frames_ % 60 == 0) {
            // report the first frame immediately, then every 60 frames -- logging every frame would drown the console's iAP2 log
            log("received frame %llu (this frame %zu bytes, %llu total)", frames_, len, bytes_);
        }
        return true;
    }

    void reset() override { log("video output reset (recovery path after dropping a keyframe)"); }
    void flush() override {}

private:
    static void log(const char* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        std::printf("  ★ %s\n", buf);
    }

    unsigned long long frames_ = 0;
    unsigned long long bytes_  = 0;
};

}  // namespace

namespace {

constexpr uint16_t kLockdownPort = 62078;

// lockdown's QueryType request. **Note it's an XML plist, not the binary plist from core/bplist**
// -- this lockdown channel only accepts XML (DiPlay sends it this way too).
const char* kQueryType =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key><string>allplay.wired_probe</string>\n"
    "  <key>Request</key><string>QueryType</string>\n"
    "</dict>\n"
    "</plist>\n";

/// Send the XML verbatim, prefixed with a 4-byte big-endian length (lockdown's framing)
bool sendLockdown(core::IByteStream& ch, const std::string& xml)
{
    std::vector<uint8_t> buf(4 + xml.size());
    const uint32_t n = uint32_t(xml.size());
    buf[0] = uint8_t(n >> 24);
    buf[1] = uint8_t(n >> 16);
    buf[2] = uint8_t(n >> 8);
    buf[3] = uint8_t(n);
    std::memcpy(buf.data() + 4, xml.data(), xml.size());
    return ch.writeAll(buf.data(), buf.size());
}

/// Read one lockdown message (4-byte big-endian length + content). Returns an empty string on timeout/disconnect.
std::string recvLockdown(core::IByteStream& ch, int timeoutMs)
{
    uint8_t hdr[4];
    size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < 4) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) return {};
        const int n = ch.read(hdr + got, 4 - got, int(left));
        if (n < 0) return {};
        if (n == 0) continue;
        got += size_t(n);
    }
    const uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) |
                         (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    if (len == 0 || len > 4 * 1024 * 1024) {
        std::printf("  ✗ unreasonable length field: %u\n", len);
        return {};
    }

    std::string out(len, '\0');
    size_t off = 0;
    while (off < len) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        const int n = ch.read(reinterpret_cast<uint8_t*>(&out[off]), len - off, int(left));
        if (n < 0) break;
        if (n == 0) continue;
        off += size_t(n);
    }
    out.resize(off);
    return out;
}

void usage()
{
    std::printf(
        "Usage:\n"
        "  wired_probe                open link → usbmux handshake → connect to 62078 → observe phone reaction\n"
        "  wired_probe --query        once connected, send a lockdown QueryType and print the reply\n"
        "  wired_probe --pair         run the full control plane (pairing→session→TLS→carkit service)\n"
        "  wired_probe --link         run the full control plane: link negotiation → identification → MFi → power/subscriptions\n"
        "                             → wait for 0x4300 → reply 0x4301 (must carry the NCM NIC's IPv6)\n"
        "  wired_probe --mfi-dir D    with --link: MFi identity directory (identity.pk8 + certificate.p7b)\n"
        "  wired_probe --state-dir D  where to store pairing material (default defaultStateDir(); without it you get the trust dialog every time)\n"
        "  wired_probe --airplay-port N  AirPlay port reported to the phone in 0x4301 (default 7000)\n"
        "  wired_probe --ncm-link-local A  IPv6 link-local address pinned on the NCM NIC\n"
        "                             (default fe80::2, same as DiPlay's config.linkLocal)\n"
        "  wired_probe --marker       with --pair: only send the 6-byte iAP2 start marker (the phone won't respond)\n"
        "  wired_probe --port N       use a different port (default 62078)\n"
        "  wired_probe --hold         stay idle waiting for Enter once connected\n"
        "  wired_probe --stay         keep reading once connected and dump subsequent bytes\n"
        "  wired_probe --pid 12a8     restrict the product ID\n"
        "  wired_probe --selftest     offline self-test (plist / certificates / TLS loopback); no phone or sudo needed\n");
}

}  // namespace

int main(int argc, char** argv)
{
    core::logInitFromEnv();
    // stdout is fully buffered when piped, and a timeout/SIGKILL discards the whole buffer --
    // these probes all get "killed midway", and the discarded part is exactly what you most want to see. Switch to line buffering.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // The offline self-test doesn't touch USB, so intercept it before argument parsing -- and it needs no sudo
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) return wiredSelfTest();
    }

    allplay::posix::WiredUsbOptions opt;
    uint16_t port = kLockdownPort;
    uint16_t airPlayPort = 7000;   ///< AirPlay port reported to the phone in 0x4301
    std::string ncmLinkLocal;      ///< link-local address pinned on the NCM NIC (empty = default fe80::2)
    bool     doQuery = false;
    bool     doHold = false;
    bool     doStay = false;
    bool     doPair = false;
    bool     doMarker = false;
    bool     doLink = false;
    std::string mfiDir;
    std::string stateDir;          ///< where pairing material is stored (empty = defaultStateDir())

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--query") == 0) doQuery = true;
        else if (std::strcmp(a, "--hold") == 0) doHold = true;
        else if (std::strcmp(a, "--stay") == 0) doStay = true;
        else if (std::strcmp(a, "--pair") == 0) doPair = true;
        else if (std::strcmp(a, "--marker") == 0) doMarker = true;
        else if (std::strcmp(a, "--link") == 0) { doLink = true; doPair = true; }
        else if (std::strcmp(a, "--mfi-dir") == 0 && i + 1 < argc) mfiDir = argv[++i];
        else if (std::strcmp(a, "--state-dir") == 0 && i + 1 < argc) stateDir = argv[++i];
        else if (std::strcmp(a, "--port") == 0 && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--airplay-port") == 0 && i + 1 < argc)
            airPlayPort = uint16_t(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--ncm-link-local") == 0 && i + 1 < argc) ncmLinkLocal = argv[++i];
        else if (std::strcmp(a, "--pid") == 0 && i + 1 < argc) opt.productId = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--wait") == 0 && i + 1 < argc) opt.switchWaitMs = std::atoi(argv[++i]) * 1000;
        else { usage(); return 2; }
    }

    // ---- ① Open the wired link ----
    std::printf("=== ① Open the wired link ===\n");
    allplay::posix::WiredUsbInfo info;
    std::string err;
#ifdef __APPLE__
    (void)opt;   // macOS doesn't enumerate USB; product ID / config-switch wait are meaningless
    // macOS doesn't switch config or claim interfaces: the control plane borrows the system usbmuxd's tunnel (the same one
    // Finder/Xcode use), and the media-plane NIC is brought up by AppleUSBNCMData. Prerequisite: this Mac was trusted by the phone.
    std::unique_ptr<core::usb::IMux> mux = allplay::posix::openSystemUsbMuxd(&info, &err);
    if (!mux) {
        std::printf("✗ open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ serial %s (system usbmuxd)\n", info.serial.c_str());
    std::printf("  ✓ USBMUX interface %u (iAP2 USBHostTransport number; the NCM NIC belongs to AppleUSBNCMData)\n",
                info.usbMuxInterface);
#else
    auto pipe = allplay::posix::openWiredCarPlay(opt, &info, &err);
    if (!pipe) {
        std::printf("✗ open failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ serial %s  bus %u addr %u  config %d\n", info.serial.c_str(), info.bus,
                info.address, info.configValue);
    // The NIC side belongs to the kernel cdc_ncm; here we only report the two interfaces it binds
    std::printf("  ✓ USBMUX interface %u, bulk OUT 0x%02x / IN 0x%02x; NCM control interface %u data interface %u"
                " (the NIC is created by the kernel cdc_ncm)\n",
                info.usbMuxInterface, info.bulkOut, info.bulkIn, info.ncmControlInterface,
                info.ncmDataInterface);
#endif

    // ---- ② The other path: the full control plane (--pair) ----
    if (doPair) {
        std::printf("\n=== ② Control plane: pairing → session → TLS → carkit service ===\n");
#ifdef __APPLE__
        core::usb::WiredControl wc(std::move(mux));
#else
        core::usb::WiredControl wc(*pipe);
#endif
        // Store pairing material keyed by the phone's serial and reuse it -- otherwise each run uses a new HostID/cert,
        // the phone treats us as a stranger every time, and **shows "Trust This Computer?" every time**.
        wc.setPairStore(info.serial, stateDir);
        if (!wc.connect(&err)) {
            std::printf("✗ control plane failed: %s\n", err.c_str());
            return 1;
        }
        core::IByteStream* svc = wc.service();
        std::printf("  ✓ iAP2 stream acquired (HostID=%s)\n", wc.hostId().c_str());

        if (doMarker) {
            // The link layer is **host-initiated**: without these 6 bytes the phone stays silent on the service stream.
            // Note there is **no** length prefix or encapsulation before it; just the bare 6 bytes.
            static const uint8_t kInit[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
            if (!svc->writeAll(kInit, sizeof kInit)) {
                std::printf("✗ failed to send the start marker\n");
                return 1;
            }
            std::printf("  ✓ sent iAP2 start marker FF 55 02 00 EE 10\n");
        }

        // ---- ③ Link negotiation (--link): marker + SYN, host-initiated ----
        if (doLink) {
            std::printf("\n=== ③ iAP2 link negotiation (host-initiated) ===\n");
            core::iap2::WiredLink link(*svc);
            std::string            lerr;
            if (!link.negotiate(30000, &lerr)) {
                std::printf("✗ link negotiation failed: %s\n", lerr.c_str());
                std::printf("  peer parameters: %s\n", link.engine().peerSummary().c_str());
                return 1;
            }
            std::printf("  ✓ link NORMAL\n");
            std::printf("  ✓ peer parameters: %s\n", link.engine().peerSummary().c_str());
            std::printf("  ✓ peer's max control payload: %zu bytes\n", link.engine().peerMaxPayload());

            std::printf("\n=== ④ Identification (0x1D00 → 0x1D01 → 0x1D02) ===\n");
            core::iap2::WiredConfig wcfg;
            // Under USBHostTransport you must fill in **the USB interface number the phone uses for CarPlay on its side**,
            // i.e. the USBMUX interface number we identified from the descriptor -- don't hard-code it.
            wcfg.carPlayUsbInterfaceNumber = int(info.usbMuxInterface);
            if (!link.identify(wcfg, &lerr)) {
                std::printf("✗ identification failed: %s\n", lerr.c_str());
                return 1;
            }
            std::printf("  ✓ identification passed (USBHostTransport interface number %d)\n", wcfg.carPlayUsbInterfaceNumber);

            std::printf("\n=== ⑤ MFi authentication (0xAA00 → 0xAA01 → 0xAA02 → 0xAA03 → 0xAA05) ===\n");
            std::unique_ptr<core::mfi::CertificateMfiIdentity> mfi;
            if (!mfiDir.empty()) {
                mfi = core::mfi::CertificateMfiIdentity::load(mfiDir, &err);
                if (!mfi) {
                    std::printf("✗ cannot load MFi identity (%s): %s\n", mfiDir.c_str(), err.c_str());
                    return 1;
                }
                std::printf("  ✓ MFi identity loaded (certificate %zu bytes, from %s)\n", mfi->certificate().size(),
                            mfiDir.c_str());
            } else {
                std::printf("  · no --mfi-dir given, sending an empty certificate (will fail on a real phone)\n");
            }
            if (!link.authenticate(mfi.get(), 30000, &lerr)) {
                std::printf("✗ authentication failed: %s\n", lerr.c_str());
                return 1;
            }
            std::printf("  ✓ MFi authentication passed\n");

            // ---- ⑥ Power + 5 subscriptions ----
            std::printf("\n=== ⑥ Power and subscriptions (0xae03 + 5 entries) ===\n");
            link.sendPowerSourceUpdate(wcfg.availableCurrentMilliAmps, /*charging=*/true);
            link.sendSubscriptions();
            std::printf("  ✓ sent 0xae03 PowerSourceUpdate (%u mA) and 5 subscriptions\n",
                        unsigned(wcfg.availableCurrentMilliAmps));

            // ---- ⑦ Wired media plane: find the kernel cdc_ncm NIC ----
            core::iap2::WiredCarPlayEndpoint ep;
            ep.sourceVersion = core::airplay::kSourceVersion;

            allplay::posix::WiredNcmLink ncm;
#ifdef __APPLE__
            std::printf("\n=== ⑦ Wired media plane (NIC created by AppleUSBNCMData) ===\n");
            if (!allplay::posix::prepareWiredNcmDarwin(info.serial, &ncm, &err)) {
                std::printf("✗ wired media plane failed to start: %s\n", err.c_str());
                return 1;
            }
#else
            std::printf("\n=== ⑦ Wired media plane (kernel cdc_ncm) ===\n");
            if (!allplay::posix::prepareWiredNcm(int(info.bus), int(info.address), ncmLinkLocal, &ncm,
                                                 &err)) {
                std::printf("✗ wired media plane failed to start: %s\n", err.c_str());
                return 1;
            }
#endif
            ep.ipv6Addresses = ncm.ipv6;
            std::printf("  ✓ NCM NIC %s: MAC %s, IPv6 %s%s\n", ncm.ifname.c_str(), ncm.mac.c_str(),
                        ncm.ipv6.front().c_str(), ncm.pinned ? " (address pinned this run)" : "");
            if (ncm.unmanaged) std::printf("  · NetworkManager detached (otherwise it would request DHCP)\n");

            // ---- ⑦b Start the AirPlay server, bound to this NIC ----
            // **Must happen before sending 0x4301**: the port the phone receives is the one written in 0x4301,
            // and "which port we actually bound to" is only known after the listener is up (it backs off if occupied).
            // The address must be the bound one too -- if 0x4301 says fe80::2 but we listen on the wildcard, the phone still can't connect.
            core::airplay::ReceiverConfig rcfg;
            rcfg.name        = "AllPlay";
            rcfg.ifname      = ncm.ifname;
            rcfg.bindAddr    = ncm.ipv6.front();
            rcfg.port        = airPlayPort;
            rcfg.btMac       = allplay::posix::controllerAddress();
            rcfg.mfiDir      = mfiDir.empty() ? "identity" : mfiDir;
            rcfg.enableAvahi = false;   // no advertising needed for wired: the phone gets the address from 0x4301
            CountingVideoSink sink;
            rcfg.video       = &sink;   // without this the phone gets no dataPort and waits forever
            core::airplay::Receiver rx;
            if (!rx.start(rcfg, allplay::posix::makePlatformServices(), &err)) {
                std::printf("✗ AirPlay server failed to start: %s\n", err.c_str());
                return 1;
            }
            std::printf("  ✓ AirPlay server listening on [%s]:%u (deviceid %s)\n",
                        rcfg.bindAddr.c_str(), unsigned(rx.port()), rx.info().deviceId.c_str());

            // What 0x4301 reports must be **exactly what the server recognizes**: same deviceid, same public key,
            // same port. Send one set while listening on another, and the phone's first act on connecting is to mismatch.
            ep.airPlayPort      = rx.port();
            ep.deviceIdentifier = rx.info().deviceId;
            ep.publicKeyHex     = rx.info().pkHex;
            std::printf("  ✓ identity %s (pk %s…)\n", rx.info().pi.c_str(),
                        rx.info().pkHex.substr(0, 16).c_str());

            // ---- ⑧ Wait for 0x4300, reply with 0x4301 ----
            std::printf("\n=== ⑧ Wait for 0x4300 CarPlayAvailability → reply 0x4301, Ctrl-C to exit ===\n");
            bool     sent4301 = false;
            uint64_t beat     = core::iap2::monoMs();
            for (;;) {
                std::vector<core::iap2::WiredLink::Message> msgs;
                if (!link.pump(200, &msgs)) { std::printf("  · disconnected\n"); break; }
                for (const auto& m : msgs) {
                    if (m.id == 0x4300) {
                        core::iap2::CarPlayAvailability av;
                        const bool ok = core::iap2::parseCarPlayAvailability(m.body, &av);
                        std::printf("  · received 0x4300: %s\n", ok ? core::iap2::describeCarPlayAvailability(av).c_str()
                                                               : " (parse failed, dumping raw bytes)");
                        if (!ok) {
                            for (size_t i = 0; i < m.body.size() && i < 96; ++i)
                                std::printf("%02x ", m.body[i]);
                            std::printf("\n");
                        }
                        // **Don't filter on available**: DiPlay also replies on receipt; the phone decides for itself.
                        if (!sent4301) {
                            std::string serr;
                            if (!link.sendCarPlayStartSession(ep, &serr)) {
                                std::printf("✗ failed to send 0x4301: %s\n", serr.c_str());
                                return 1;
                            }
                            sent4301 = true;
                            std::printf("  ✓ replied 0x4301 CarPlayStartSession -- "
                                        "waiting for the phone to connect to AirPlay at [%s]:%u\n",
                                        ep.ipv6Addresses.front().c_str(), unsigned(ep.airPlayPort));
                        } else {
                            std::printf("  · 0x4301 already sent; logging this one without replying\n");
                        }
                    } else {
                        // **Dump verbatim**: this line is how you see what the peer "wants to say" -- only a few messages are recognized;
                        // the rest (especially those carrying addresses/credentials) must be read structurally by eye.
                        std::printf("  · control frame 0x%04x, %zu bytes: ", m.id, m.body.size());
                        for (size_t i = 0; i < m.body.size() && i < 128; ++i)
                            std::printf("%02x", m.body[i]);
                        std::printf("\n");
                        std::printf("      ascii: ");
                        for (size_t i = 0; i < m.body.size() && i < 128; ++i) {
                            const unsigned char c = m.body[i];
                            std::printf("%c", (c >= 0x20 && c < 0x7f) ? char(c) : '.');
                        }
                        std::printf("\n");
                    }
                }
                if (core::iap2::monoMs() - beat >= 5000) {
                    beat = core::iap2::monoMs();
                    std::printf("  · %s, still no 0x4300\n", sent4301 ? "already replied 0x4301" : "waiting for the phone to speak");
                }
            }
            return 0;
        }

        std::printf("\n=== ③ Reading the iAP2 stream, Ctrl-C to exit ===\n");
        uint8_t buf[4096];
        for (;;) {
            const int n = svc->read(buf, sizeof buf, 2000);
            if (n < 0) { std::printf("  · disconnected\n"); break; }
            if (n == 0) continue;
            std::printf("  · received %d bytes: ", n);
            for (int i = 0; i < n; ++i) std::printf("%02x ", buf[i]);
            std::printf("\n");
        }
        return 0;
    }

    // ---- ② Get a channel to a phone port ----
#ifdef __APPLE__
    // macOS has no usbmux v2 of our own: the system daemon is that layer, so just request a tunnel.
    std::printf("\n=== ② Connect to a phone port via the system usbmuxd ===\n");
    auto ch = mux->connectTcp(port, 5000, &err);
#else
    std::printf("\n=== ② usbmux v2 handshake ===\n");
    core::usb::UsbMux mux(*pipe);
    if (!mux.begin(&err)) {
        std::printf("✗ handshake failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ version negotiation + setup both passed\n");

    // ---- ③ Open a mini-TCP to lockdown ----
    std::printf("\n=== ③ Connect to phone port %u ===\n", port);
    auto ch = mux.connectTcp(port, 5000, &err);
#endif
    if (!ch) {
        std::printf("✗ connect failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ tunnel established (port %u)\n", unsigned(port));

    // ---- ④ Optional: send a lockdown QueryType ----
    if (doQuery) {
        std::printf("\n=== ④ lockdown QueryType ===\n");
        if (!sendLockdown(*ch, kQueryType)) {
            std::printf("✗ failed to send request\n");
            return 1;
        }
        const std::string reply = recvLockdown(*ch, 5000);
        if (reply.empty()) {
            std::printf("✗ no reply within 5 seconds (channel is up but lockdown ignores us)\n");
            return 1;
        }
        std::printf("  ✓ replied %zu bytes:\n----\n%s\n----\n", reply.size(), reply.c_str());
    }

    if (doStay) {
        std::printf("\n=== Reading continuously, Ctrl-C to exit ===\n");
        uint8_t buf[4096];
        for (;;) {
            const int n = ch->read(buf, sizeof(buf), 1000);
            if (n < 0) { std::printf("  · disconnected\n"); break; }
            if (n == 0) continue;
            std::printf("  · received %d bytes: ", n);
            for (int i = 0; i < n; ++i) std::printf("%02x ", buf[i]);
            std::printf("\n");
        }
    }

    if (doHold) {
        std::printf("\nPress Enter to exit…\n");
        std::getchar();
    }

    ch->close();
    std::printf("\nDone.\n");
    return 0;
}
