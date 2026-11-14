/**
 * @file   session.h
 * @brief  AirPlay session state machine (M3: pair-setup/verify, auth-setup, SETUP)
 * @author blt
 */

// ============================================================================
// core/airplay/session.h - AirPlay session state machine (M3: pair-setup/verify, auth-setup, SETUP)
// ============================================================================
// One Session per phone connection (this implementation is single-phone: Receiver holds one).
// Endpoint order (real-device behavior): /info -> /pair-setup -> /pair-verify (control channel encrypted afterwards)
// -> /auth-setup -> SETUP (batch 1 / batch 2) -> RECORD (session active).
// Per the AirPlay protocol (pair-setup / pair-verify / auth-setup / SETUP flow).
#pragma once

#include "airplay/audio_stream.h"
#include "airplay/control_cipher.h"
#include "airplay/event_channel.h"
#include "airplay/info.h"
#include "airplay/rtsp.h"
#include "airplay/video_stream.h"
#include "crypto/srp6a.h"
#include "crypto/x25519.h"
#include "mfi/mfi_identity.h"
#include "platform/device_identity.h"
#include "platform/pairing_store.h"
#include "port/media.h"
#include "port/platform.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace core::airplay {

struct SessionConfig {
    const core::DeviceIdentity* identity = nullptr;   ///< local Ed25519 identity
    const DeviceInfo*           info = nullptr;       ///< used for /info
    std::string                 stateDir;             ///< pairing storage directory
    std::string                 mfiDir;               ///< MFi identity directory (identity/)
    /// Externally injected MFi identity (HarmonyOS reads it from HAP rawfile; there is no identity/ directory on disk).
    /// Takes precedence over mfiDir when non-null; the caller guarantees it outlives the Session.
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;
    const core::PlatformServices* services = nullptr;   ///< creates TCP listeners (screen stream dataPort)
    core::IVideoOutput*         video = nullptr;      ///< video output (decode + display)
    core::IAudioOutput*         audio = nullptr;      ///< audio output (decode + playback)
    int                         width = 1280;         ///< negotiated resolution
    int                         height = 720;
};

class Session {
public:
    explicit Session(const SessionConfig& cfg);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    RtspResponse handle(const RtspRequest& req);

    /// The control key is ready after pair-verify M4; the caller takes it and installs the cipher **after writing that response**.
    bool        controlKeysReady() const { return controlKeysReady_; }
    ControlKeys takeControlKeys() { controlKeysReady_ = false; return controlKeys_; }

    bool sessionActive() const { return sessionActive_; }

    /// Connecting: an RTSP request from the iPhone has been received (pairing/verification/negotiation), but RECORD has not been reached yet (session not fully established).
    bool connecting() const { return connecting_ && !sessionActive_; }

    /// Send a touch (normalized coordinates) to the event channel; returns false if the event channel is not ready.
    bool sendTouch(const std::vector<TouchPoint>& contacts);

    /// Request a keyframe from the iPhone immediately (unthrottled). Called after the surface is recreated on returning to foreground, to restore the picture promptly.
    void requestKeyFrameNow();

    /// Connection dropped / session ended: reset the active/connecting state (UI returns to the splash screen).
    void markDisconnected();

private:
    RtspResponse handleInfo();
    RtspResponse handleAuthSetup(const RtspRequest& req);
    RtspResponse handlePairSetup(const RtspRequest& req);
    RtspResponse handlePairVerify(const RtspRequest& req);
    RtspResponse handleSetup(const RtspRequest& req);
    RtspResponse handleCommand(const RtspRequest& req);
    RtspResponse handleFeedback(const RtspRequest& req);

    RtspResponse tlv8Response(const std::vector<uint8_t>& body);
    RtspResponse pairErr(int state);

    void openEventChannel();   ///< open the event channel (called during SETUP batch 1)
    void openNtp();            ///< open the NTP timing port (audio RTP clock sync)
    void openKeepAlive();      ///< open the keep-alive UDP port (when the iPhone requests keepAliveLowPower)
    void requestKeyFrame();    ///< request a keyframe from the iPhone via the event channel (resets the reference chain)

    const core::DeviceIdentity* identity_ = nullptr;
    const DeviceInfo*           info_ = nullptr;
    std::string                 stateDir_;
    const mfi::IMfiIdentity* mfi_ = nullptr;                   ///< usable when non-null (injected or owned)
    std::unique_ptr<mfi::CertificateMfiIdentity> ownedMfi_;    ///< owner when loaded from mfiDir
    PairingStore                pairings_;
    const core::PlatformServices* services_ = nullptr;
    core::IVideoOutput*         video_ = nullptr;
    core::IAudioOutput*         audio_ = nullptr;
    int                         width_ = 1280, height_ = 720;
    std::vector<std::unique_ptr<ScreenStream>> streams_;
    std::vector<std::unique_ptr<AudioStream>> audioStreams_;

    // pair-setup
    std::unique_ptr<crypto::Srp6aServer> srp_;
    std::vector<uint8_t> sessionKey_;

    // pair-verify
    crypto::X25519Key    eph_;
    std::vector<uint8_t> clientEphPub_;
    std::vector<uint8_t> shared_;
    std::vector<uint8_t> encKey_;

    ControlKeys controlKeys_;
    bool controlKeysReady_ = false;
    // Written by the RTSP connection thread and read by the main thread; with dual channels it is also read by the
    // accept thread of **the other channel** (mutual exclusion gate), so it must be atomic, not a bare bool.
    std::atomic<bool> sessionActive_{false};
    std::atomic<bool> connecting_{false};   ///< an RTSP request has been received (the iPhone is connecting)

    std::unique_ptr<EventChannel> eventChannel_;

    std::unique_ptr<core::IUdpSocket> ntpSock_;
    std::thread       ntpThread_;
    std::atomic<bool> stopNtp_{false};

    std::unique_ptr<core::IUdpSocket> keepAliveSock_;
    std::thread       keepAliveThread_;
    std::atomic<bool> stopKeepAlive_{false};

    std::thread       keyframeThread_;
    std::atomic<bool> stopKeyframe_{false};
    std::chrono::steady_clock::time_point lastKeyframeRequest_{};   // forceKeyFrame throttle (1/s)
};

}  // namespace core::airplay
