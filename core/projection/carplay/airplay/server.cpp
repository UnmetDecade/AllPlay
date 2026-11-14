#include "airplay/server.h"

#include "common/log.h"
#include "port/network.h"

#include <cstdio>
#include <cstring>

namespace core::airplay {

Receiver::~Receiver() { stop(); }

bool Receiver::start(const ReceiverConfig& cfg, const core::PlatformServices& services,
                     std::string* err)
{
    cfg_ = cfg;

    if (!services.network || !services.makeTcpListener || !services.makeMdnsAdvertiser) {
        if (err) *err = "PlatformServices is not fully wired up (network/tcpListener/mdnsAdvertiser)";
        return false;
    }

    // ---- 1) NIC / IP ----
    // Wired and wireless diverge cleanly at this step:
    //   wireless — we do not know which NIC the phone comes in on, so pick one with IPv4 and tell the phone via mDNS;
    //   wired — the phone reads the address from 0x4301; **we choose the address in advance** (the
    //           link-local on the NCM NIC, see allplay::posix::prepareWiredNcm), and that NIC has no
    //           IPv4. So "has IPv4" cannot be used as the success criterion here.
    if (cfg_.bindAddr.empty()) {
        if (cfg_.ifname.empty()) {
            ip_ = services.network->pickInterfaceIPv4(&ifname_);
        } else {
            ifname_ = cfg_.ifname;
            ip_ = services.network->interfaceIPv4(ifname_);
        }
        if (ip_.empty()) {
            if (err) *err = "No usable network interface found (specify one explicitly with --ifname)";
            return false;
        }
    } else {
        ifname_ = cfg_.ifname;
        ip_     = cfg_.bindAddr;
    }
    // ---- 2) local identity (Ed25519 + pi) ----
    const std::string stateDir = cfg_.stateDir.empty() ? defaultStateDir() : cfg_.stateDir;
    if (!identity_.loadOrCreate(stateDir, err)) return false;

    // ---- 3) deviceid: a persistent stable value (Android randomizes the Wi-Fi MAC and the hotspot interface MAC differs,
    //      so it cannot serve as an identity; use a persistent synthetic value that is stable across networks/restarts) ----
    bool devCreated = false;
    const std::string deviceId = loadOrCreateSyntheticDeviceId(stateDir, &devCreated);
    if (devCreated) LOG_INFO("deviceid uses a persistent synthetic value: %s (newly generated)", deviceId.c_str());

    // ---- 4) assembly for /info and TXT ----
    info_.name     = cfg_.name;
    info_.deviceId = deviceId;
    info_.btMac    = cfg_.btMac;
    info_.pi       = identity_.pi;
    info_.pkHex    = identity_.pkHex;
    info_.width    = cfg_.width;
    info_.height   = cfg_.height;
    info_.fps      = cfg_.fps;

    // ---- 5) session (pair-setup/verify, auth-setup, SETUP) ----
    SessionConfig sc;
    sc.identity = &identity_;
    sc.info     = &info_;
    sc.stateDir = stateDir;
    sc.mfiDir   = cfg_.mfiDir;
    sc.mfiIdentity = cfg_.mfiIdentity;
    // Make a copy to hold and pass its address to the Session — see the services_ comment in server.h.
    // Storing &services (the argument's address) works inside start() but dangles once the function returns.
    services_   = services;
    sc.services = &services_;   // NOLINT: points to a member of this object and stays valid until Receiver is destroyed
    sc.video    = cfg_.video;
    sc.audio    = cfg_.audio;
    sc.width    = cfg_.width;
    sc.height   = cfg_.height;
    session_ = std::make_unique<Session>(sc);

    // ---- 6) RTSP server (auto-retreat when the port is taken) ----
    // Bind address: link-local needs the %interface suffix (otherwise the platform bind returns EINVAL) — this is done
    // here rather than by the caller, because the caller only has the address and interface name as two separate values.
    std::string bindAddr = cfg_.bindAddr;
    if (!bindAddr.empty() && bindAddr.find('%') == std::string::npos &&
        !ifname_.empty() && bindAddr.rfind("fe80:") == 0) {
        bindAddr += "%" + ifname_;
    }

    std::unique_ptr<core::ITcpListener> listener;
    std::string rerr;
    if (cfg_.port == 0) {
        auto l = services.makeTcpListener();
        if (l && l->start(bindAddr, 0, &rerr)) listener = std::move(l);
    } else {
        for (uint16_t p = cfg_.port; p < cfg_.port + 20; ++p) {
            auto l = services.makeTcpListener();
            if (l && l->start(bindAddr, p, &rerr)) {
                listener = std::move(l);
                if (p != cfg_.port) LOG_WARN("Port %u is in use, switching to %u", cfg_.port, p);
                break;
            }
        }
    }
    if (!listener) {
        if (err) *err = rerr.empty() ? "RTSP listen failed" : rerr;
        session_.reset();
        return false;
    }
    rtsp_.reset(new RtspServer);
    rtsp_->start(std::move(listener), [this](const RtspRequest& r) { return session_->handle(r); });
    rtsp_->setAfterResponse([this] {
        // pair-verify M4 has been written in plaintext; enable control channel encryption now
        if (session_->controlKeysReady()) {
            rtsp_->setCipher(std::make_shared<ControlCipher>(session_->takeControlKeys()));
            LOG_INFO("Control channel encryption enabled");
        }
    });
    rtsp_->setOnDisconnect([this] {
        // phone disconnected/exited: reset session state (UI returns to the splash screen instead of a black screen)
        if (session_) session_->markDisconnected();
        LOG_INFO("Control connection dropped; session state reset");
    });

    // ---- 7) Bonjour advertisement (failure is non-fatal) ----
    if (cfg_.enableAvahi) {
        // Bonjour TXT features use "0x{low 32 bits}[,0x{high 32 bits}]" (per the protocol's features TXT format)
        const uint64_t f = info_.features;
        const uint32_t lo = uint32_t(f & 0xFFFFFFFFULL);
        const uint32_t hi = uint32_t(f >> 32);
        char feat[48];
        if (hi == 0) std::snprintf(feat, sizeof feat, "0x%x", lo);
        else         std::snprintf(feat, sizeof feat, "0x%x,0x%x", lo, hi);
        std::vector<std::string> txt = {
            "deviceid=" + deviceId,
            std::string("features=") + feat,
            "flags=0x4",
            "model=AllPlay1,1",
            "srcvers=" + std::string(kSourceVersion),
            std::string("protovers=") + kProtoVersion,
            "pi=" + identity_.pi,
            "pk=" + identity_.pkHex,
        };
        bonjour_ = services.makeMdnsAdvertiser();
        if (!bonjour_->start(cfg_.name, rtsp_->port(), txt, &avahiErr_)) {
            LOG_WARN("Bonjour advertisement unavailable: %s (the phone will not see this device on the network)", avahiErr_.c_str());
            bonjour_.reset();
        }
    }

    LOG_INFO("Receiver ready: ifname=%s ip=%s deviceid=%s port=%u",
             ifname_.c_str(), ip_.c_str(), deviceId.c_str(), rtsp_->port());
    return true;
}

void Receiver::stop()
{
    if (bonjour_) { bonjour_->stop(); bonjour_.reset(); }
    if (rtsp_)    { rtsp_->stop();    rtsp_.reset(); }
}

}  // namespace core::airplay
