/**
 * @file   server.h
 * @brief  Receiver assembly (platform-independent; PlatformServices injected)
 * @author blt
 */

// ============================================================================
// core/airplay/server.h - Receiver assembly (platform-independent; PlatformServices injected)
// ============================================================================
// Assembly order (matching real-device behavior):
//   pick NIC/IP -> take MAC as deviceid -> load/generate local identity (Ed25519+pi)
//   -> create Session -> start RTSP server (auto +1 if the port is taken) -> mDNS advertise _airplay._tcp
// M3: Session implements pair-setup/verify, auth-setup, SETUP; control encryption is installed on RTSP after pair-verify.
#pragma once

#include "airplay/info.h"
#include "airplay/rtsp.h"
#include "airplay/session.h"
#include "platform/device_identity.h"
#include "port/mdns.h"
#include "port/media.h"
#include "port/platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace core::airplay {

struct ReceiverConfig {
    std::string name     = "AllPlay";
    std::string ifname;                     ///< empty = choose automatically (first non-lo interface with IPv4)
    /// Explicit local bind address. **Empty = wildcard** (that is how the wireless path works: mDNS advertisement + the phone finds us).
    /// Required for wired: the phone gets the address from 0x4301 and connects directly; binding to wildcard means advertising the wrong address.
    /// Fill in the link-local address on the NCM NIC (pass together with ifname; link-local automatically gets %scope appended).
    std::string bindAddr;
    uint16_t    port     = 7000;            ///< auto +1 if taken, up to 20 attempts; 0 = kernel chooses
    int         width    = 1280;
    int         height   = 720;
    int         fps      = 60;
    std::string stateDir;                   ///< empty = defaultStateDir()
    std::string mfiDir   = "identity";      ///< MFi identity directory (identity.pk8 + certificate.p7b)
    /// Externally injected MFi identity (HarmonyOS HAP rawfile). Takes precedence over mfiDir when non-null.
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;
    std::string btMac;                      ///< local Bluetooth MAC (for /info bluetoothIDs)
    core::IVideoOutput* video = nullptr;    ///< video output (decode + display; if null, the screen stream is not created)
    core::IAudioOutput* audio = nullptr;    ///< audio output (decode + playback; if null, the audio stream is not created)
    bool        enableAvahi = true;
};

class Receiver {
public:
    ~Receiver();
    Receiver() = default;
    Receiver(const Receiver&) = delete;

    bool start(const ReceiverConfig& cfg, const core::PlatformServices& services, std::string* err);
    void stop();
    bool running() const { return rtsp_ != nullptr; }

    uint16_t            port() const { return rtsp_ ? rtsp_->port() : 0; }
    const std::string&  ifname() const { return ifname_; }
    const std::string&  ip() const { return ip_; }
    const DeviceInfo&   info() const { return info_; }
    const std::string&  avahiError() const { return avahiErr_; }
    bool                sessionActive() const { return session_ && session_->sessionActive(); }

    /// Connecting (the iPhone is pairing/negotiating, RECORD not reached yet).
    bool connecting() const { return session_ && session_->connecting(); }

    /// Send a touch (normalized coordinates) to the event channel (M4 HID feedback).
    bool sendTouch(const std::vector<TouchPoint>& contacts) {
        return session_ && session_->sendTouch(contacts);
    }

    /// Dual-channel exclusion: whether to accept a new connection is decided by the composition root (see RtspServer::setConnectionGate).
    /// Must be called after start() — before start() there is no RtspServer to attach it to.
    void setConnectionGate(std::function<bool()> g) {
        if (rtsp_) rtsp_->setConnectionGate(std::move(g));
    }

    /// Request a keyframe from the iPhone immediately (called after the surface is recreated on returning to foreground, to restore the picture promptly).
    void requestKeyFrameNow() {
        if (session_) session_->requestKeyFrameNow();
    }

private:
    ReceiverConfig  cfg_;
    /// ★ Owns its own PlatformServices copy (rather than storing the address of the start() argument).
    /// The Session outlives start(): it first uses services_->makeTcpListener at SETUP
    /// to create the data/event channels, and callers often pass a temporary object (host/main.cpp:332).
    /// Storing the argument's address = dangling pointer, with SIGSEGV during SETUP as the symptom.
    core::PlatformServices services_;
    DeviceIdentity  identity_;
    DeviceInfo      info_;
    std::string     ifname_, ip_, avahiErr_;
    std::unique_ptr<Session>              session_;
    std::unique_ptr<RtspServer>           rtsp_;
    std::unique_ptr<core::IMdnsAdvertiser> bonjour_;
};

}  // namespace core::airplay
