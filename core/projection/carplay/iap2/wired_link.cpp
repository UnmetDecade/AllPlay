/**
 * @file   wired_link.cpp
 * @brief  Implementation of the wired iAP2 link driver
 * @author blt
 */

#include "iap2/wired_link.h"

#include "common/log.h"
#include "iap2/hid.h"
#include "iap2/wire.h"
#include "mfi/mfi_identity.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace core::iap2 {
namespace {

constexpr const char* kTag = "wired-link";
/// Wait per read. Both the granularity for "how long without data before resending SYN" and the tick interval.
constexpr int kReadTimeoutMs = 100;
constexpr int kReadBufBytes  = 16 * 1024;

// endpoint numbers
constexpr uint16_t kStartIdentification       = 0x1d00;
constexpr uint16_t kIdentificationInformation = 0x1d01;
constexpr uint16_t kIdentificationAccepted    = 0x1d02;
constexpr uint16_t kIdentificationRejected    = 0x1d03;
constexpr uint16_t kRequestAuthenticationCert = 0xaa00;
constexpr uint16_t kAuthenticationCertificate = 0xaa01;
constexpr uint16_t kRequestChallengeResponse  = 0xaa02;
constexpr uint16_t kAuthenticationResponse    = 0xaa03;
constexpr uint16_t kAuthenticationFailed      = 0xaa04;
constexpr uint16_t kAuthenticationSucceeded   = 0xaa05;

// Power / subscriptions (the second stage of wired bring-up; DiPlay order: power -> 5 subscriptions -> wait for 0x4300)
constexpr uint16_t kPowerSourceUpdate          = 0xae03;
constexpr uint16_t kStartNowPlayingUpdates     = 0x5000;
constexpr uint16_t kStartRouteGuidanceUpdates  = 0x5200;
constexpr uint16_t kStartPowerUpdates          = 0xae00;
constexpr uint16_t kStartCommunicationsUpdates = 0x4157;
constexpr uint16_t kStartCallStateUpdates      = 0x4154;

constexpr uint16_t kCarPlayStartSession = 0x4301;

// "Messages we support" list in the identification message. **This is the wired variant**, differing from the wireless one in two ways:
//   · 0xae03 PowerSourceUpdate is supported (on the wired path we really are powering the phone)
//   · 0x5703 AccessoryWiFiConfiguration is not sent (wired media goes over NCM; there is no Wi-Fi configuration step)
const std::vector<uint16_t> kMessagesSent = {
    0xaa01, 0xaa03, 0x5000, 0x5002, 0x5200, 0x5203, 0xae00,
    0xae02, 0x4157, 0x4159, 0x4154, 0x4156, 0xae03, 0x4301,
};
const std::vector<uint16_t> kMessagesReceived = {
    0xaa00, 0xaa02, 0xaa04, 0xaa05, 0xea00, 0xea01, 0x5001,
    0x5201, 0x5202, 0xae01, 0x4158, 0x4155, 0x4300,
};

std::string hex4(uint16_t v)
{
    static const char* kHex = "0123456789abcdef";
    std::string        s = "0x";
    s += kHex[(v >> 12) & 0xf];
    s += kHex[(v >> 8) & 0xf];
    s += kHex[(v >> 4) & 0xf];
    s += kHex[v & 0xf];
    return s;
}

}  // namespace

uint64_t monoMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

LinkConfig wiredLinkConfig()
{
    LinkConfig c;
    c.maxOutgoing           = 4;    // DiPlay WIRED_LINK_CONFIG
    c.controlSessionVersion = 2;
    // Zero acknowledgements: reporting all four as 0 in SYN tells the peer "the USB path is reliable; do not retransmit or wait for my ACK".
    // The peer's maxRetransmissions=0 makes LinkEngine skip unacked tracking (see link.cpp),
    // so reporting 0 here is part of the pairing, not arbitrary.
    c.retransmissionTimeoutMs = 0;
    c.ackTimeoutMs            = 0;
    c.maxRetransmissions      = 0;
    c.maxAcknowledgements     = 0;
    c.initiator               = true;
    return c;
}

WiredLink::WiredLink(core::IByteStream& stream)
    : stream_(stream), engine_(wiredLinkConfig())
{
}

void WiredLink::flush()
{
    std::vector<uint8_t> out;
    while (engine_.takeOutput(&out)) {
        // takeOutput hands over everything accumulated, so this loop really runs only once
        if (!stream_.writeAll(out.data(), out.size())) {
            LOG_WARN("[%s] Link layer has %zu bytes to send but cannot write", kTag, out.size());
            return;
        }
        out.clear();
    }
}

void WiredLink::start(uint64_t nowMs)
{
    engine_.start(nowMs);
    flush();
}

bool WiredLink::pumpOnce(int readTimeoutMs)
{
    uint8_t buf[kReadBufBytes];
    const int n = stream_.read(buf, sizeof buf, readTimeoutMs);
    if (n < 0) return false;               // disconnected
    if (n > 0) engine_.feed(buf, size_t(n), monoMs());
    engine_.tick(monoMs());                // marker/SYN resend and data retransmission rely on this
    flush();
    return true;
}

bool WiredLink::negotiate(int timeoutMs, std::string* err)
{
    const uint64_t deadline = monoMs() + uint64_t(timeoutMs > 0 ? timeoutMs : 0);
    start(monoMs());

    int lastState = -1;
    while (monoMs() < deadline) {
        if (engine_.ready()) return true;
        if (engine_.dead()) {
            if (err != nullptr) *err = "Link layer declared dead: " + engine_.deadReason();
            return false;
        }
        if (!pumpOnce(kReadTimeoutMs)) {
            if (err != nullptr) *err = "Transport disconnected during negotiation";
            return false;
        }
        // Log each state change so you do not have to scan a screen of DBG lines for the transition
        const int st = engine_.ready() ? 3 : (engine_.dead() ? 4 : 2);
        if (st != lastState) {
            lastState = st;
            LOG_INFO("[%s] Link state: %s", kTag,
                     st == 3 ? "NORMAL (negotiation complete, control messages can be sent)"
                             : "NEGOTIATING (marker sent, resending SYN)");
        }
    }

    if (engine_.ready()) return true;
    if (err != nullptr) {
        *err = "Negotiation did not finish within " + std::to_string(timeoutMs) + " ms (peer never replied to SYN?)";
    }
    return false;
}

bool WiredLink::sendMediaKey(core::MediaKey key, bool down)
{
    Params p;
    p.bytes(0, hidReport(key, down));
    sendMessage(kAccessoryHidReport, p);
    LOG_INFO("[%s] Steering wheel control: %s%s", kTag, core::mediaKeyName(key), down ? " (down)" : " (up)");
    return true;
}

int WiredLink::pumpMediaKeys(core::MediaKeyQueue& q)
{
    int n = 0;
    core::MediaKeyEvent ev;
    while (q.pop(&ev)) {
        sendMediaKey(ev.key, ev.down);
        ++n;
    }
    return n;
}

void WiredLink::sendMessage(uint16_t msgId, const Params& params)
{
    const auto frame = csmFrame(msgId, params.encode());
    // A CSM frame may span multiple link payloads; split at the peer's reported limit
    const size_t maxChunk = engine_.peerMaxPayload();
    for (size_t off = 0; off < frame.size(); off += maxChunk) {
        const size_t n = std::min(maxChunk, frame.size() - off);
        engine_.sendControl(std::vector<uint8_t>(frame.begin() + off, frame.begin() + off + n),
                            monoMs());
    }
    flush();
}

void WiredLink::sendIdentification(const WiredConfig& cfg)
{
    Params g10;
    g10.u8(0, 1).str(1, cfg.eaProtocol).u8(2, 0);

    // group 16 = USBHostTransport. **Only wired has this group**; wireless uses 17/24
    // (Bluetooth MAC + SSID). The 3rd item in the triple is the USB interface number the phone uses for CarPlay.
    Params g16;
    g16.u16(0, 0).str(1, "USBHostTransport").voidv(2)
        .u8(3, uint8_t(cfg.carPlayUsbInterfaceNumber)).voidv(4);

    Params g30;
    g30.u16(0, 42).str(1, "RouteGuidance").u16(2, 64).u16(4, 64).u16(6, 8);

    Params body;
    body.str(0, cfg.name)
        .str(1, cfg.model)
        .str(2, cfg.manufacturer)
        .str(3, cfg.serial)
        .str(4, cfg.firmware)
        .str(5, cfg.hardware)
        .u16List(6, kMessagesSent)
        .u16List(7, kMessagesReceived)
        .u8(8, 2)      // power capability: we power the phone over the USB path
        .u16(9, 20)    // the phone can draw at most 20mA from us? — copied from DiPlay, do not change
        .group(10, g10)
        .str(12, cfg.language)
        .strList(13, {cfg.language})
        .group(16, g16)
        .group(30, g30);

    sendMessage(kIdentificationInformation, body);
    LOG_INFO("[%s] Sent IdentificationInformation (0x1d01, USBHostTransport interface number %d)", kTag,
             cfg.carPlayUsbInterfaceNumber);
}

bool WiredLink::pumpMessages(int readTimeoutMs, std::vector<Message>* out)
{
    if (!pumpOnce(readTimeoutMs)) return false;
    std::vector<uint8_t> payload;
    while (engine_.nextControl(&payload)) framer_.feed(payload.data(), payload.size());
    Message m;
    while (framer_.next(&m.id, &m.body)) {
        LOG_INFO("[%s] iAP2 rx=%s", kTag, hex4(m.id).c_str());
        out->push_back(m);
    }
    return true;
}

bool WiredLink::identify(const WiredConfig& cfg, std::string* err)
{
    const uint64_t deadline = monoMs() + uint64_t(cfg.timeoutMs > 0 ? cfg.timeoutMs : 30000);
    bool sent = false;

    while (monoMs() < deadline) {
        if (engine_.dead()) {
            if (err != nullptr) *err = "Link layer declared dead: " + engine_.deadReason();
            return false;
        }
        std::vector<Message> msgs;
        if (!pumpMessages(kReadTimeoutMs, &msgs)) {
            if (err != nullptr) *err = "Transport disconnected during identification";
            return false;
        }

        for (const Message& m : msgs) {
            if (m.id == kStartIdentification) {
                sendIdentification(cfg);
                sent = true;
            } else if (m.id == kIdentificationAccepted) {
                LOG_INFO("[%s] Phone accepted identification (0x1d02)", kTag);
                return true;
            } else if (m.id == kIdentificationRejected) {
                // the message lists the rejected parameter ids — log them, otherwise we can only guess
                std::vector<Param> ps;
                std::string        which;
                if (parseParams(m.body.data(), m.body.size(), &ps)) {
                    for (const Param& p : ps) {
                        if (!which.empty()) which += " ";
                        which += hex4(p.id);
                    }
                }
                if (err != nullptr) {
                    *err = "Phone rejected identification (0x1d03), rejected parameters: " + (which.empty() ? "(not listed)" : which);
                }
                return false;
            }
        }
    }

    if (err != nullptr) {
        *err = sent ? "No 0x1d02 after sending 0x1d01" : "Never received 0x1d00 (StartIdentification) from the phone";
    }
    return false;
}

bool WiredLink::authenticate(const mfi::IMfiIdentity* identity, int timeoutMs, std::string* err)
{
    const uint64_t deadline = monoMs() + uint64_t(timeoutMs > 0 ? timeoutMs : 30000);
    bool sentCert = false;

    while (monoMs() < deadline) {
        if (engine_.dead()) {
            if (err != nullptr) *err = "Link layer declared dead: " + engine_.deadReason();
            return false;
        }
        std::vector<Message> msgs;
        if (!pumpMessages(kReadTimeoutMs, &msgs)) {
            if (err != nullptr) *err = "Transport disconnected during authentication";
            return false;
        }

        for (const Message& m : msgs) {
            switch (m.id) {
            case kRequestAuthenticationCert: {
                // the phone wants the accessory certificate. Send the real one if identity exists, otherwise an empty one (a real device replies 0xaa04)
                Params p;
                p.bytes(0, identity != nullptr ? identity->certificate() : std::vector<uint8_t>{});
                LOG_INFO("[%s] 0xaa00 requested a certificate; replying 0xaa01 (%zu bytes)", kTag,
                         identity != nullptr ? identity->certificate().size() : 0);
                sendMessage(kAuthenticationCertificate, p);
                sentCert = true;
                break;
            }
            case kRequestChallengeResponse: {
                std::vector<Param> ps;
                parseParams(m.body.data(), m.body.size(), &ps);
                const Param* ch = nullptr;
                for (const Param& p : ps)
                    if (p.id == 0) ch = &p;
                if (ch == nullptr) {
                    if (err != nullptr) *err = "No challenge parameter in 0xAA02";
                    return false;
                }
                bool              ok = false;
                std::vector<uint8_t> sig;
                if (identity != nullptr) sig = identity->signDigest(ch->payload.data(), ch->payload.size(), &ok);
                if (identity == nullptr || !ok) {
                    if (err != nullptr) {
                        *err = identity == nullptr ? "No MFi identity configured, cannot sign"
                                                   : "MFi signing failed (challenge length is not 32?)";
                    }
                    return false;
                }
                Params p;
                p.bytes(0, sig);
                LOG_INFO("[%s] 0xaa02 challenge %zu bytes; replying 0xaa03 (signature %zu bytes)", kTag,
                         ch->payload.size(), sig.size());
                sendMessage(kAuthenticationResponse, p);
                break;
            }
            case kAuthenticationSucceeded:
                LOG_INFO("[%s] MFi authentication passed (0xaa05)", kTag);
                return true;

            case kAuthenticationFailed:
                if (err != nullptr) *err = "Phone replied 0xAA04 AuthenticationFailed";
                return false;

            default:
                break;
            }
        }
    }

    if (err != nullptr) {
        *err = sentCert ? "No 0xaa04/0xaa05 after sending 0xaa01"
                        : "Never received 0xaa00 (RequestAuthenticationCertificate) from the phone";
    }
    return false;
}

// ---------------------------------------------------------------------------
// Power + subscriptions (the second stage of wired bring-up)
// ---------------------------------------------------------------------------

void WiredLink::sendPowerSourceUpdate(uint16_t availableCurrentMilliAmps, bool charging)
{
    // Parameter 0 = available current (mA), parameter 1 = whether charging.
    // The other three (siphon current available / reserved current / over-threshold) are not sent on DiPlay's wired path; follow suit.
    Params p;
    p.u16(0, availableCurrentMilliAmps).u8(1, charging ? 1 : 0);
    sendMessage(kPowerSourceUpdate, p);
    LOG_INFO("[%s] Sent 0xae03 PowerSourceUpdate (%u mA, charging=%d)", kTag,
             unsigned(availableCurrentMilliAmps), charging ? 1 : 0);
}

void WiredLink::sendSubscriptions()
{
    Params nowPlaying;
    Params np0, np1;
    for (int id : {1, 4, 6, 12, 26}) np0.voidv(uint16_t(id));
    for (int id : {0, 1, 7}) np1.voidv(uint16_t(id));
    nowPlaying.group(0, np0).group(1, np1);
    sendMessage(kStartNowPlayingUpdates, nowPlaying);

    Params route;
    route.u16(0, 42).voidv(1).voidv(2);
    sendMessage(kStartRouteGuidanceUpdates, route);

    Params power;
    power.voidv(4).voidv(5).voidv(6);
    sendMessage(kStartPowerUpdates, power);

    Params comms;
    comms.voidv(0).voidv(4).voidv(5);
    sendMessage(kStartCommunicationsUpdates, comms);

    Params calls;
    for (int id : {0, 1, 2, 3, 4, 11}) calls.voidv(uint16_t(id));
    sendMessage(kStartCallStateUpdates, calls);

    LOG_INFO("[%s] Sent 5 subscriptions (0x5000/0x5200/0xae00/0x4157/0x4154)", kTag);
}

bool WiredLink::sendCarPlayStartSession(const WiredCarPlayEndpoint& ep, std::string* err)
{
    // Parameter sanity checks. **Do not skip**: when these are not met, the phone mostly "receives but ignores you",
    // which looks the same as "link not up" and is very expensive to diagnose later. DiPlay also hard-requires them.
    if (ep.ipv6Addresses.empty()) {
        if (err != nullptr) *err = "0x4301 needs an IPv6 address for the wired group, but none was provided";
        return false;
    }
    for (const std::string& a : ep.ipv6Addresses) {
        // Check only two things: has a colon (is an IPv6 literal), and has no %zone
        // — the phone cannot parse an address with a scope suffix, so block it here instead.
        if (a.find(':') == std::string::npos || a.find('%') != std::string::npos) {
            if (err != nullptr) *err = "0x4301 address is not an IPv6 literal: " + a;
            return false;
        }
    }
    if (ep.airPlayPort == 0 || ep.publicKeyHex.empty() || ep.sourceVersion.empty()) {
        if (err != nullptr) *err = "0x4301 is missing airPlayPort / publicKey / sourceVersion";
        return false;
    }

    // group 0 = wired: **one parameter per address** (repeated id 0), not a string list.
    Params wired;
    for (const std::string& a : ep.ipv6Addresses) wired.str(0, a);

    Params body;
    body.group(0, wired);
    body.u32(2, ep.airPlayPort);
    if (!ep.deviceIdentifier.empty()) body.str(3, ep.deviceIdentifier);
    body.str(4, ep.publicKeyHex);
    body.str(5, ep.sourceVersion);

    sendMessage(kCarPlayStartSession, body);
    LOG_INFO("[%s] Sent 0x4301 CarPlayStartSession (ipv6=%s port=%u pk=%s src=%s)", kTag,
             ep.ipv6Addresses.front().c_str(), unsigned(ep.airPlayPort),
             ep.publicKeyHex.substr(0, 16).c_str(), ep.sourceVersion.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 0x4300 CarPlayAvailability
// ---------------------------------------------------------------------------

namespace {

/// Read one "availability" subgroup: sub-parameter 0 = u8 available flag, sub-parameter 1 = string identifier (may be missing).
void readAvailabilityGroup(const Param& group, bool* have, bool* available, std::string* identifier)
{
    std::vector<Param> subs;
    if (!parseParams(group.payload.data(), group.payload.size(), &subs)) return;

    if (const Param* p = findParam(subs, 0)) {
        bool ok = false;
        const uint8_t v = paramU8(*p, &ok);
        if (ok) {
            *have      = true;
            *available = (v != 0);
        }
    }
    if (const Param* p = findParam(subs, 1)) {
        bool              ok = false;
        const std::string s  = paramString(*p, &ok);
        if (ok) *identifier = s;
    }
}

}  // namespace

bool parseCarPlayAvailability(const std::vector<uint8_t>& body, CarPlayAvailability* out)
{
    if (out == nullptr) return false;
    *out = CarPlayAvailability{};

    std::vector<Param> groups;
    if (!parseParams(body.data(), body.size(), &groups)) return false;

    // Parameter 0 = wired group, parameter 1 = wireless group, parameter 2 = theme assets (unused)
    if (const Param* p = findParam(groups, 0)) {
        readAvailabilityGroup(*p, &out->haveWired, &out->wiredAvailable, &out->wiredIdentifier);
    }
    if (const Param* p = findParam(groups, 1)) {
        readAvailabilityGroup(*p, &out->haveWireless, &out->wirelessAvailable,
                              &out->wirelessIdentifier);
    }
    return out->haveWired || out->haveWireless;
}

std::string describeCarPlayAvailability(const CarPlayAvailability& a)
{
    auto side = [](const char* label, bool have, bool available, const std::string& id) {
        if (!have) return std::string(label) + "=(not reported by phone)";
        return std::string(label) + "=" + (available ? "available" : "unavailable") +
               (id.empty() ? "" : " (identifier " + id + ")");
    };
    return side("wired", a.haveWired, a.wiredAvailable, a.wiredIdentifier) + "; " +
           side("wireless", a.haveWireless, a.wirelessAvailable, a.wirelessIdentifier);
}

// ---------------------------------------------------------------------------
// Public pump
// ---------------------------------------------------------------------------

bool WiredLink::pump(int readTimeoutMs, std::vector<Message>* out)
{
    return pumpMessages(readTimeoutMs, out);
}

}  // namespace core::iap2
