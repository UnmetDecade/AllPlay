/**
 * @file   wireless_client.h
 * @brief  Wireless CarPlay trigger state machine (iAP2 control flow over Bluetooth)
 * @author blt
 */

// ============================================================================
// core/iap2/wireless_client.h - Wireless CarPlay trigger state machine (iAP2 control flow over Bluetooth)
// ============================================================================
// Flow (per the iAP2 wireless control flow):
//   identify: 0x1D00 -> send 0x1D01 -> receive 0x1D02
//   MFi: 0xAA00 -> send 0xAA01 (certificate) -> 0xAA02 -> send 0xAA03 (signature) -> receive 0xAA05
//   subscribe: send 5 messages (0x5000/0x5200/0xAE00/0x4157/0x4154)
//   wait for 0x5702 -> send 0x5703 (Wi-Fi configuration, up to 5 times)
//   wait for 0x4300 -> send 0x4301 (AirPlay endpoint, the success point)
//   0x4E0E -> resend 0x5703 once (up to 2 times)
// Transport is abstracted as IByteStream (real device = RFCOMM; tests = in-memory pair).
#pragma once

#include "iap2/link.h"
#include "iap2/wire.h"
#include "mfi/mfi_identity.h"
#include "port/control.h"
#include "port/stream.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::iap2 {

struct WirelessTriggerConfig {
    // identification identity (0x1D01)
    std::string name         = "AllPlay";
    std::string model        = "AllPlay1,1";
    std::string manufacturer = "AllPlay";
    std::string serial       = "000000000000";
    std::string firmware     = "1.0";
    std::string hardware     = "1.0";
    std::string eaProtocol   = "com.allplay.receiver";
    std::string language     = "en";

    // wireless (group 17/24, 0x5703, 0x4301)
    std::string btMac;        ///< local Bluetooth MAC "AA:BB:CC:DD:EE:FF"
    std::string ssid;         ///< same-network SSID (the phone is already on this network)
    std::string passphrase;   ///< may be empty when securityType=0
    int         channel    = 0;
    int         securityType = 2;   ///< 0=NONE 1=WEP 2=WPA/WPA2 3=WPA3 transition 4=WPA3
    std::vector<std::string> ipAddresses;   ///< local reachable IPs (same network)

    // AirPlay endpoint (0x4301)
    uint16_t    airPlayPort      = 7000;
    std::string deviceIdentifier;
    std::string publicKeyHex;
    std::string sourceVersion    = "950.7.1";

    // MFi (one of two sources; mfiIdentity takes precedence)
    std::string mfiDir;       ///< identity/ (identity.pk8 + certificate.p7b)
    /// Externally injected identity (not owned; must outlive run()). For platforms where "the identity is not on the filesystem":
    /// HarmonyOS reads the bytes from HAP rawfile, and future hardware MFi chips will use the same path.
    const mfi::IMfiIdentity* mfiIdentity = nullptr;

    /// External cancellation flag (optional). After 0x4301 is sent, run() keeps the link alive for a long time until the phone disconnects;
    /// the host sets it after the AirPlay session ends so this trigger finishes and the next one can be scheduled.
    const std::atomic<bool>* cancel = nullptr;

    /// Steering-wheel button queue (optional). Upper layers (UI / system media keys / future hardware buttons) push here,
    /// and run() drains it each iteration and encodes the events as iAP2 HID reports. **Must outlive this object**.
    /// Pass null = this link does not support steering-wheel controls (behavior identical to before).
    core::MediaKeyQueue* keyQueue = nullptr;

    /// Whether the phone has connected to the AirPlay port (optional). After 0x4301, iOS may **not dial**:
    /// measured on wireless: 0x4301 -> 14.5 seconds before the phone connected, and in the black-screen cases it never connected at all.
    /// Only toggling Wi-Fi manually made it reconnect — meaning the phone does not retry after declaring this route dead.
    /// With this callback, run() can resend 0x5703+0x4301 when no connection appears within kAirPlayRetryMs,
    /// nudging it to retry via messages so the user does not have to touch Wi-Fi. Pass null = no nudge (old behavior).
    std::function<bool()> phoneConnected;

    int timeoutMs = 60000;
};

enum class TriggerStage {
    Idle,
    Identified,
    Authenticated,
    Subscribed,
    WifiConfigSent,
    StartSessionSent,
};

class WirelessTrigger {
public:
    using Progress = std::function<void(const std::string&)>;
    using StageCb  = std::function<void(TriggerStage)>;

    WirelessTrigger(IByteStream* stream, WirelessTriggerConfig cfg);
    ~WirelessTrigger();
    WirelessTrigger(const WirelessTrigger&) = delete;

    void setProgress(Progress p) { progress_ = std::move(p); }
    void setStageCb(StageCb c)   { stageCb_ = std::move(c); }

    /// Block until 0x4301 is sent (success) or failure/timeout. May be called repeatedly (restarts the link state machine each time).
    bool run();

    TriggerStage stage() const { return stage_; }
    const std::string& error() const { return error_; }

private:
    void log(const std::string& s);
    void setStage(TriggerStage s);
    void sendMessage(uint16_t msgId, const Params& params);
    void sendIdentification();
    void sendSubscriptions();
    /// Drain the steering-wheel queue (call on run()'s thread; the link is single-threaded, do not send from other threads)
    void pumpMediaKeys();
    void sendWifiConfiguration();
    void sendStartSession();
    void handleMessage(uint16_t msgId, const std::vector<uint8_t>& body);

    IByteStream*           stream_;
    WirelessTriggerConfig cfg_;
    LinkEngine            link_;
    CsmFramer             framer_;
    std::unique_ptr<mfi::IMfiIdentity> ownedMfi_;   ///< present only when loaded from mfiDir
    const mfi::IMfiIdentity*           mfi_ = nullptr;   ///< points to ownedMfi_ or an externally injected identity
    Progress              progress_;
    StageCb               stageCb_;

    TriggerStage stage_ = TriggerStage::Idle;
    std::string  error_;
    int wifiConfigSends_ = 0;
    int postTransportSends_ = 0;
    bool transportSeen_ = false;
    bool startSessionSent_ = false;   ///< 0x4301 sent (after that the control loop keeps running until disconnect/timeout)
    bool phoneSeen_ = false;          ///< the phone has connected AirPlay (only the first time counts; no further nudges)
    uint64_t startSessionAtMs_ = 0;   ///< time of the last 0x4301 (to compute "how long since it connected")
    int  phoneRetries_ = 0;           ///< how many nudges have been sent

    /// Verbosity of the "message list" (parameters 6/7) in the identification message.
    /// iOS 16/18 (iPhone 13 mini, 16) accept the full list; iOS 13 (iPhone SE2), whether full,
    /// minimal, or omitted entirely, always replies 0x1D03 and rejects — three experiments all failed, so it was judged that
    /// the old system simply does not accept it, and no further compatibility is done for it. The "narrow the list and retry on rejection" fallback is kept
    /// in case another picky model shows up in the future and automatic degradation is needed.
    enum class IdList { Full, Minimal, Omit };
    IdList idListMode_ = IdList::Full;
    int    identRetries_ = 0;
};

}  // namespace core::iap2
