/**
 * @file   wired_link.h
 * @brief  Wired iAP2: link layer over the carkit service stream, with host-initiated negotiation
 * @author blt
 */

// ============================================================================
// core/iap2/wired_link.h - Driver for the wired iAP2 link
// ============================================================================
// Wired and wireless differ in only one link-layer detail, but it is the difference between "works" and "total silence":
//
//   wireless: the phone (accessory) sends the marker first; we reply with SYN after receiving it.
//   wired: **the host sends the marker first, then without waiting for any reply immediately sends its own SYN**, resending every 500ms,
//         until the phone sends its SYN back.
//
// So LinkEngine alone cannot enter negotiation just by watching for the peer marker; someone must take the first step for it —
// this class is that step: LinkConfig::initiator = true, plus a pump (read -> feed -> tick -> write).
//
// Wired link parameters also follow DiPlay's WIRED_LINK_CONFIG: maxOutgoing=4, control session version 2,
// and **zero acknowledgements declared** (the USB path is reliable, so neither retransmission nor ACK is needed).
#pragma once

#include "iap2/link.h"
#include "iap2/wire.h"
#include "port/control.h"
#include "port/stream.h"

#include <cstdint>
#include <string>
#include <vector>

namespace core::mfi {
class IMfiIdentity;
}

namespace core::iap2 {

class Params;   // iap2/wire.h: CSM message body builder

/// Wired link parameters (compare LinkConfig defaults: those are used by wireless)
LinkConfig wiredLinkConfig();

/// Identity reported in wired identification (0x1D01). Field names follow the wireless naming; values may differ —
/// wired reports a "head unit", not an "accessory".
struct WiredConfig {
    std::string name         = "AllPlay";
    std::string model        = "AllPlay1,1";
    std::string manufacturer = "AllPlay";
    std::string serial       = "000000000000";
    std::string firmware     = "1.0";
    std::string hardware     = "1.0";
    std::string eaProtocol   = "com.allplay.receiver";
    std::string language     = "en";

    /// USB interface number the phone exposes for CarPlay — that is the bInterfaceNumber of the USBMUX interface in the descriptor
    /// (measured = 1 on a real device). **We do not choose this number; it is read from the USB descriptor**,
    /// and reporting it wrong makes the phone reject identification outright (0x1D03).
    int carPlayUsbInterfaceNumber = 1;

    /// "How much current we can supply to the phone" reported in 0xAE03 PowerSourceUpdate (mA). Matches DiPlay's default.
    uint16_t availableCurrentMilliAmps = 2400;

    int timeoutMs = 30000;
};

/// What the "wired group" reports in 0x4301 CarPlayStartSession.
/// The DiPlay counterpart is Iap2WiredCarPlayEndpoint — there the address comes from its own VPN/NCM,
/// while here it comes from **the NIC created by the kernel cdc_ncm** (see core::INetworkInfo::interfaceForUsbDevice).
struct WiredCarPlayEndpoint {
    /// IPv6 literal(s) of our side on the NCM link, at least one. The phone uses it to connect AirPlay here.
    /// **Must be link-local** (fe80::/10): NCM is a point-to-point link with no router advertising a prefix.
    std::vector<std::string> ipv6Addresses;
    uint16_t    airPlayPort = 7000;
    /// AirPlay pairing-layer public key (Ed25519, lowercase hex) — this is what the phone verifies in pair-verify
    std::string publicKeyHex;
    /// Protocol version string (AirPlay sourceVersion / srcvers). **Must not be empty**; the phone selects versions based on it.
    std::string sourceVersion;
    /// Optional. If empty, this parameter is not sent.
    std::string deviceIdentifier;
};

/// The part of 0x4300 CarPlayAvailability we care about. **One group per direction**:
/// what we receive here is the phone asking "can your (accessory) side do wired/wireless each".
struct CarPlayAvailability {
    bool        haveWired         = false;
    bool        wiredAvailable    = false;
    std::string wiredIdentifier;             ///< phone-side wired transport identifier (its UDID on a real device)
    bool        haveWireless      = false;
    bool        wirelessAvailable = false;
    std::string wirelessIdentifier;          ///< phone-side Wi-Fi MAC
};

/// Parse the 0x4300 message body (the parameter table after the CSM header). Returns false on malformed input and keeps whatever could be read.
bool parseCarPlayAvailability(const std::vector<uint8_t>& body, CarPlayAvailability* out);

/// One-line summary for humans (for logs)
std::string describeCarPlayAvailability(const CarPlayAvailability& a);

class WiredLink {
public:
    /// One complete CSM message
    struct Message {
        uint16_t             id = 0;
        std::vector<uint8_t> body;
    };

    /// stream must outlive this object (usually WiredControl::service())
    explicit WiredLink(core::IByteStream& stream);

    /// Send marker + SYN to enter negotiation. Call only once.
    void start(uint64_t nowMs);

    /// Pump once: write pending bytes -> read a chunk -> feed the link layer -> advance time -> write again.
    /// Returns false = the transport is broken (not "no data").
    bool pumpOnce(int readTimeoutMs);

    /// Pump until the link reaches NORMAL. When ready=false, err contains the cause of death or a timeout description.
    bool negotiate(int timeoutMs, std::string* err);

    /// Identification: wait for the phone's 0x1D00 -> reply 0x1D01 -> wait for 0x1D02.
    /// **The first thing the phone sends after the link reaches NORMAL is 0x1D00, and it will not wait long** —
    /// measured: waiting 44 seconds gets the USB reset entirely. So identification must follow negotiation immediately.
    bool identify(const WiredConfig& cfg, std::string* err);

    /// MFi authentication: 0xAA00 -> send certificate 0xAA01 -> 0xAA02 challenge -> send signature 0xAA03 -> 0xAA05 (passed).
    /// If identity is null, send an empty certificate — guaranteed to fail on a real device; only for offline comparison.
    bool authenticate(const mfi::IMfiIdentity* identity, int timeoutMs, std::string* err);

    /// 0xAE03 PowerSourceUpdate: tell the phone "how much power I can give you and whether I am charging".
    /// On the wired path we really are powering the phone, so this step is meaningful rather than a formality.
    void sendPowerSourceUpdate(uint16_t availableCurrentMilliAmps, bool charging);

    /// The 5 subscriptions immediately after power in DiPlay's wired bring-up:
    /// 0x5000 NowPlaying / 0x5200 RouteGuidance / 0xAE00 Power /
    /// 0x4157 Communications / 0x4154 CallState.
    /// **These subscribe to the "phone tells the head unit" side** — without sending these 5, the phone pushes no state at all.
    void sendSubscriptions();

    /// 0x4301 CarPlayStartSession (wired group). Only after this is sent does the phone start connecting
    /// AirPlay to ipv6Addresses:airPlayPort. Returns false and writes err if the parameters are invalid.
    bool sendCarPlayStartSession(const WiredCarPlayEndpoint& ep, std::string* err);

    /// Pump once and hand over the complete CSM messages accumulated this round, in order. false = transport disconnected.
    bool pump(int readTimeoutMs, std::vector<Message>* out);

    /// Steering wheel: send one button event (encoding in iap2/hid.h). **Must be called on the same thread as pump** —
    /// the link layer's seq/ack is single-threaded state. The return value only means "entered the send queue".
    bool sendMediaKey(core::MediaKey key, bool down);

    /// Steering wheel: drain all queued button events, returning how many were sent. As above, use only on the pump thread.
    int pumpMediaKeys(core::MediaKeyQueue& q);

    LinkEngine&       engine() { return engine_; }
    const LinkEngine& engine() const { return engine_; }

private:
    void flush();
    /// Pump once (internal); same as above but reused by each step.
    bool pumpMessages(int readTimeoutMs, std::vector<Message>* out);
    /// Send one CSM message (fragmented into the link at the peer's capacity)
    void sendMessage(uint16_t msgId, const Params& params);
    void sendIdentification(const WiredConfig& cfg);

    core::IByteStream& stream_;
    LinkEngine         engine_;
    /// **Kept across identify/authenticate**: the phone's 0xAA00 may well arrive during the previous step's pump,
    /// and creating a framer per step would drop half a message (symptom: authentication never sees 0xAA00).
    CsmFramer          framer_;
};

/// Monotonic milliseconds (the link layer takes time from the caller; this provides a single source)
uint64_t monoMs();

}  // namespace core::iap2
