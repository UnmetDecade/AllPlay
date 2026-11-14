// ============================================================================
// tools/core_selftest.cpp -- core self-test (for M1 acceptance, no SDL/display dependency)
// ============================================================================
// Usage:
//   core_selftest [--port 7000] [--ifname eth0] [--name "AllPlay"]
//                 [--screen 1280x720] [--state-dir DIR] [--no-avahi] [--duration S]
// Exit: Ctrl-C / SIGTERM / --duration elapsed.
#include "airplay/server.h"
#include "bt/bt_trigger.h"
#include "common/log.h"
#include "posix/posix.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

bool parseScreen(const char* s, int* w, int* h)
{
    return std::sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

}  // namespace

int main(int argc, char** argv)
{
    core::logInitFromEnv();

    core::airplay::ReceiverConfig cfg;
    int durationSec = 0;

    // M2: Bluetooth trigger options
    bool        btEnabled = false;
    bool        btServerMode = true;      // default is server: register SDP and wait for the phone to connect
    std::string btDevice = "iPhone";
    std::string btAddr;
    std::string btSsid;
    std::string btPass;
    std::string identityDir = "identity";

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "argument %s is missing a value\n", a); std::exit(2); }
            return argv[++i];
        };
        if (!std::strcmp(a, "--port")) cfg.port = uint16_t(std::atoi(next()));
        else if (!std::strcmp(a, "--ifname")) cfg.ifname = next();
        else if (!std::strcmp(a, "--name")) cfg.name = next();
        else if (!std::strcmp(a, "--state-dir")) cfg.stateDir = next();
        else if (!std::strcmp(a, "--no-avahi")) cfg.enableAvahi = false;
        else if (!std::strcmp(a, "--duration")) durationSec = std::atoi(next());
        else if (!std::strcmp(a, "--bt")) btEnabled = true;
        else if (!std::strcmp(a, "--bt-mode")) {
            const char* m = next();
            btServerMode = !std::strcmp(m, "server");
            if (std::strcmp(m, "server") && std::strcmp(m, "client")) {
                std::fprintf(stderr, "--bt-mode must be server or client\n");
                return 2;
            }
        }
        else if (!std::strcmp(a, "--bt-device")) btDevice = next();
        else if (!std::strcmp(a, "--bt-addr")) btAddr = next();
        else if (!std::strcmp(a, "--ssid")) btSsid = next();
        else if (!std::strcmp(a, "--wifi-pass")) btPass = next();
        else if (!std::strcmp(a, "--identity")) identityDir = next();
        else if (!std::strcmp(a, "--screen")) {
            if (!parseScreen(next(), &cfg.width, &cfg.height)) {
                std::fprintf(stderr, "--screen requires WxH\n");
                return 2;
            }
        } else {
            std::fprintf(stderr,
                         "Usage: %s [--port N] [--ifname X] [--name S] [--screen WxH]\n"
                         "        [--state-dir DIR] [--no-avahi] [--duration S]\n"
                         "        [--bt] [--bt-mode server|client] [--bt-device NAME]\n"
                         "        [--bt-addr AA-BB-..]\n"
                         "        [--ssid WIFI_NAME] [--wifi-pass PASSWORD] [--identity DIR]\n"
                         "  --bt-mode defaults to server; on Linux/macOS only **client** works with a real phone\n"
                         "            (we actively connect to the phone's iAP2 SPP service)\n", argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    core::airplay::Receiver rx;
    auto services = allplay::posix::makePlatformServices();
    std::string err;
    if (!rx.start(cfg, services, &err)) {
        std::fprintf(stderr, "[selftest] start failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("[selftest] ifname=%s ip=%s deviceid=%s\n",
                rx.ifname().c_str(), rx.ip().c_str(), rx.info().deviceId.c_str());
    std::printf("[selftest] identity: pi=%s pk=%s\n",
                rx.info().pi.c_str(), rx.info().pkHex.c_str());
    std::printf("[selftest] RTSP :%u   /info ready\n", rx.port());
    if (cfg.enableAvahi) {
        if (rx.avahiError().empty())
            std::printf("[selftest] Bonjour: advertising _airplay._tcp\n");
        else
            std::printf("[selftest] Bonjour: unavailable (%s)\n", rx.avahiError().c_str());
    } else {
        std::printf("[selftest] Bonjour: disabled (--no-avahi)\n");
    }
    std::printf("[selftest] READY\n");
    std::fflush(stdout);

    // ---- M2: Bluetooth trigger (optional) ----
    if (btEnabled) {
        core::bt::BtTriggerParams bp;
        bp.deviceName = btDevice;
        bp.deviceAddress = btAddr;
        bp.serverMode = btServerMode;
        bp.serverWaitMs = 300000;

        auto& c = bp.iap2;
        c.name         = cfg.name;
        c.model        = "AllPlay1,1";
        c.manufacturer = "AllPlay";
        c.serial       = rx.info().deviceId;
        c.firmware     = "1.0";
        c.hardware     = "1.0";
        c.btMac        = allplay::posix::controllerAddress();
        c.ssid         = btSsid.empty() ? allplay::posix::currentSSID() : btSsid;
        c.passphrase   = btPass;
        c.ipAddresses  = {rx.ip()};
        c.airPlayPort  = rx.port();
        c.deviceIdentifier = rx.info().deviceId;
        c.publicKeyHex = rx.info().pkHex;
        c.mfiDir       = identityDir;
        c.timeoutMs    = 90000;

        // After 0x4301 is sent, this iAP2 link is the wireless CarPlay heartbeat, and runBtTrigger keeps reading until
        // the phone leaves (see kSessionKeepAliveMs in wireless_client.cpp). So run it on a thread and let the
        // main thread only wait for Ctrl-C -- otherwise Ctrl-C would still have to wait for the phone to disconnect.
        std::atomic<bool> btCancel{false};
        c.cancel = &btCancel;

        std::printf("[selftest] Bluetooth: btMac=%s ssid=%s ip=%s port=%u\n",
                    c.btMac.c_str(), c.ssid.c_str(),
                    c.ipAddresses.empty() ? "?" : c.ipAddresses[0].c_str(), rx.port());
        std::fflush(stdout);

        auto transport = allplay::posix::makeBluetooth();
        core::bt::BtTriggerResult r;
        std::atomic<bool> btDone{false};
        std::thread btThread([&] {
            r = core::bt::runBtTrigger(transport.get(), bp, [](const std::string& s) {
                std::printf("[bt] %s\n", s.c_str());
                std::fflush(stdout);
            });
            btDone.store(true);
        });
        while (!btDone.load() && !g_stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        btCancel.store(true);   // Ctrl-C (or phone already gone): let the link wind down
        btThread.join();
        std::printf("[selftest] Bluetooth trigger: %s%s%s\n", r.ok ? "succeeded" : "failed",
                    r.ok ? "" : ": ", r.ok ? "" : r.error.c_str());
        std::fflush(stdout);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(durationSec);
    while (!g_stop) {
        if (durationSec > 0 && std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rx.stop();
    std::printf("[selftest] exited\n");
    return 0;
}
