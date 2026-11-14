#include "iap2/wireless_client.h"

#include "common/log.h"
#include "iap2/hid.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace core::iap2 {

namespace {

// endpoint numbers (per the iAP2 protocol's endpoint numbering)
constexpr uint16_t kStartIdentification        = 0x1d00;
constexpr uint16_t kIdentificationInformation  = 0x1d01;
constexpr uint16_t kIdentificationAccepted     = 0x1d02;
constexpr uint16_t kIdentificationRejected     = 0x1d03;
constexpr uint16_t kRequestAuthenticationCert  = 0xaa00;
constexpr uint16_t kAuthenticationCertificate  = 0xaa01;
constexpr uint16_t kRequestChallengeResponse   = 0xaa02;
constexpr uint16_t kAuthenticationResponse     = 0xaa03;
constexpr uint16_t kAuthenticationFailed       = 0xaa04;
constexpr uint16_t kAuthenticationSucceeded    = 0xaa05;
constexpr uint16_t kCarPlayAvailability        = 0x4300;
constexpr uint16_t kCarPlayStartSession        = 0x4301;
constexpr uint16_t kWirelessCarPlayUpdate      = 0x4e0d;
constexpr uint16_t kDeviceTransportIdentifier  = 0x4e0e;
constexpr uint16_t kRequestAccessoryWifiConfig = 0x5702;
constexpr uint16_t kAccessoryWifiConfiguration = 0x5703;

// subscriptions (StartXUpdates)
constexpr uint16_t kStartNowPlayingUpdates     = 0x5000;
constexpr uint16_t kStartRouteGuidanceUpdates  = 0x5200;
constexpr uint16_t kStartPowerUpdates          = 0xae00;
constexpr uint16_t kStartCommunicationsUpdates = 0x4157;
constexpr uint16_t kStartCallStateUpdates      = 0x4154;

// "Messages we support" list in the identification message (wireless variant)
const std::vector<uint16_t> kMessagesSent = {
    0xaa01, 0xaa03, 0x5000, 0x5002, 0x5200, 0x5203, 0xae00, 0xae02,
    0x4157, 0x4159, 0x4154, 0x4156, 0x4301,
    /* wireless additions */ 0x5703,
    /* NOTE: the wireless variant does not send 0xae03 PowerSourceUpdate (the protocol requires filtering it) */
};
const std::vector<uint16_t> kMessagesReceived = {
    0xaa00, 0xaa02, 0xaa04, 0xaa05, 0xea00, 0xea01, 0x5001, 0x5201, 0x5202,
    0xae01, 0x4158, 0x4155, 0x4300,
    /* wireless additions */ 0x4e0d, 0x4e0e, 0x5702,
};

std::vector<uint8_t> macBytes(const std::string& mac)
{
    // strict "AA:BB:CC:DD:EE:FF" (the bytes for group 17 in the identification message)
    std::vector<uint8_t> out;
    if (mac.size() != 17) return out;
    for (int i = 0; i < 6; ++i) {
        char b[3] = {mac[i * 3], mac[i * 3 + 1], 0};
        out.push_back(uint8_t(strtoul(b, nullptr, 16)));
    }
    return out;
}

// How long to keep the iAP2 link after 0x4301 is sent (in practice the phone disconnecting ends it early).
constexpr uint64_t kSessionKeepAliveMs = 12ULL * 60 * 60 * 1000;
/// How long after 0x4301 without the phone dialing before nudging. 3 seconds: normal models connect within 1 second
/// (~200ms measured on iPhone 16); waiting 10 seconds just makes the user stare at a black screen.
constexpr uint64_t kAirPlayRetryMs     = 3000;
constexpr int      kAirPlayRetryMax    = 2;

uint64_t nowMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

WirelessTrigger::WirelessTrigger(IByteStream* stream, WirelessTriggerConfig cfg)
    : stream_(stream), cfg_(std::move(cfg))
{
}

WirelessTrigger::~WirelessTrigger() = default;

void WirelessTrigger::log(const std::string& s)
{
    LOG_INFO("[bt] %s", s.c_str());
    if (progress_) progress_(s);
}

void WirelessTrigger::setStage(TriggerStage s)
{
    stage_ = s;
    if (stageCb_) stageCb_(s);
}

void WirelessTrigger::sendMessage(uint16_t msgId, const Params& params)
{
    auto body = params.encode();
    auto frame = csmFrame(msgId, body);

    // Fragment into the link at the peer's capacity (per the protocol, a CSM frame may span multiple link payloads)
    const size_t maxChunk = link_.peerMaxPayload();
    for (size_t off = 0; off < frame.size(); off += maxChunk) {
        const size_t n = std::min(maxChunk, frame.size() - off);
        link_.sendControl(std::vector<uint8_t>(frame.begin() + off, frame.begin() + off + n),
                          nowMs());
    }
}

void WirelessTrigger::sendIdentification()
{
    Params g10;
    g10.u8(0, 1).str(1, cfg_.eaProtocol).u8(2, 0);

    Params g17;
    auto mac = macBytes(cfg_.btMac);
    if (mac.size() == 6) {
        g17.u16(0, 0).str(1, "blue").voidv(2).bytes(3, mac).str(4, "blue").voidv(5);
    }

    Params g24;
    g24.u16(0, 1).str(1, cfg_.ssid).voidv(2).u16(3, 1).voidv(4).voidv(5);

    Params g30;
    g30.u16(0, 42).str(1, "RouteGuidance").u16(2, 64).u16(4, 64).u16(6, 8);

    // Parameters 6/7 list the message ids "I can send / I can receive". Some models (SE2) see an entry in the list
    // with an unknown id and reject the whole thing, so narrow it step by step via idListMode_.
    static const std::vector<uint16_t> kSentMinimal = {0x4301, 0x5703, 0xaa01, 0xaa03};
    static const std::vector<uint16_t> kRecvMinimal = {0x4300, 0xaa00, 0xaa02, 0xaa04, 0xaa05};

    Params body;
    body.str(0, cfg_.name)
        .str(1, cfg_.model)
        .str(2, cfg_.manufacturer)
        .str(3, cfg_.serial)
        .str(4, cfg_.firmware)
        .str(5, cfg_.hardware);
    if (idListMode_ != IdList::Omit) {
        // Must be ascending and duplicate-free: iOS 18 does not check, iOS 13 does, and rejects the whole item if unordered (measured SE2/iOS 13)
        const bool minimal = idListMode_ == IdList::Minimal;
        std::vector<uint16_t> sent = minimal ? kSentMinimal : kMessagesSent;
        std::vector<uint16_t> recv = minimal ? kRecvMinimal : kMessagesReceived;
        for (auto* v : {&sent, &recv}) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        body.u16List(6, sent).u16List(7, recv);
    }
    body.u8(8, 0)
        .u16(9, 20)
        .group(10, g10)
        .str(12, cfg_.language)
        .strList(13, {cfg_.language});
    if (mac.size() == 6) body.group(17, g17);
    body.group(24, g24).group(30, g30);

    sendMessage(kIdentificationInformation, body);
    log("iAP2 sent identification information (0x1D01)");

    // Dump the bytes on first send: the only reference when it is rejected
    if (identRetries_ == 0 && idListMode_ == IdList::Full) {
        const auto enc = body.encode();
        std::string hx;
        char b[4];
        for (size_t i = 0; i < enc.size() && i < 200; ++i) {
            std::snprintf(b, sizeof b, "%02x", enc[i]);
            hx += b;
        }
        log("iAP2 identification message " + std::to_string(enc.size()) + " bytes: " + hx);
    }
}

void WirelessTrigger::sendSubscriptions()
{
    Params nowPlaying;
    Params np0, np1;
    for (int id : {1, 4, 6, 12, 26}) np0.voidv(uint16_t(id));
    for (int id : {0, 1, 7})       np1.voidv(uint16_t(id));
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

    log("iAP2 sent 5 subscriptions");
}

void WirelessTrigger::pumpMediaKeys()
{
    if (!cfg_.keyQueue) return;
    // Buttons pressed before the session is up are meaningless (CarPlay has not taken over media yet); drop them
    // so we do not replay a key released seconds ago to the phone at startup.
    if (!link_.ready()) {
        cfg_.keyQueue->clear();
        return;
    }
    core::MediaKeyEvent ev;
    while (cfg_.keyQueue->pop(&ev)) {
        Params p;
        p.bytes(0, hidReport(ev.key, ev.down));
        sendMessage(kAccessoryHidReport, p);
        log(std::string("Steering wheel control: ") + core::mediaKeyName(ev.key) +
            (ev.down ? " (down)" : " (up)"));
    }
}

void WirelessTrigger::sendWifiConfiguration()
{
    Params cfg;
    cfg.str(1, cfg_.ssid);
    cfg.str(2, cfg_.passphrase);   // always send (the protocol requires it; an empty string is valid for NONE)
    cfg.u8(3, uint8_t(cfg_.securityType)).u8(4, uint8_t(cfg_.channel));
    sendMessage(kAccessoryWifiConfiguration, cfg);
    log(transportSeen_ ? "iAP2 sent 0x5703 (after transport)" : "iAP2 sent 0x5703 (Wi-Fi configuration)");
}

void WirelessTrigger::sendStartSession()
{
    // group 1 = wireless
    Params w;
    w.str(0, cfg_.ssid);
    w.str(1, cfg_.passphrase);   // always send (same per the protocol)
    w.u8(2, uint8_t(cfg_.channel));
    for (const auto& ip : cfg_.ipAddresses) w.str(3, ip);
    w.u8(4, uint8_t(cfg_.securityType));

    Params body;
    body.group(1, w);
    body.u32(2, cfg_.airPlayPort);
    if (!cfg_.deviceIdentifier.empty()) body.str(3, cfg_.deviceIdentifier);
    body.str(4, cfg_.publicKeyHex);
    body.str(5, cfg_.sourceVersion);

    sendMessage(kCarPlayStartSession, body);
    setStage(TriggerStage::StartSessionSent);
    startSessionSent_ = true;
    startSessionAtMs_ = nowMs();
    log("iAP2 sent 0x4301 CarPlayStartSession (airPlayPort=" +
        std::to_string(cfg_.airPlayPort) + ") - waiting for the phone to connect AirPlay");
}

void WirelessTrigger::handleMessage(uint16_t msgId, const std::vector<uint8_t>& body)
{
    std::vector<Param> params;
    parseParams(body.data(), body.size(), &params);

    switch (msgId) {
    case kStartIdentification: {
        // The request body carries the set of identification parameters the phone requires/supports; it is the only clue when rejected
        std::string hx;
        char b[4];
        for (size_t i = 0; i < body.size() && i < 160; ++i) {
            std::snprintf(b, sizeof b, "%02x", body[i]);
            hx += b;
        }
        log("iAP2 rx=0x1d00 StartIdentification (" + std::to_string(body.size()) + " bytes: " + hx + ")");
        sendIdentification();
        break;
    }

    case kIdentificationAccepted:
        log("iAP2 rx=0x1d02 identification accepted");
        setStage(TriggerStage::Identified);
        break;

    case kIdentificationRejected: {
        // The 0x1D03 parameter body is the list of rejected parameter ids: the phone explicitly says "I do not accept these".
        // Ignore it and you will only be blindly trying one item after another after 0x4301.
        std::string ids;
        bool listRejected = false;
        for (const auto& p : params) {
            if (!ids.empty()) ids += ",";
            ids += std::to_string(p.id);
            if (p.id == 6 || p.id == 7) listRejected = true;
        }
        log("iAP2 rx=0x1d03 identification rejected (rejected parameter ids: " + (ids.empty() ? "none" : ids) + ")");

        // If parameters 6/7 (message list) are rejected, narrow and retry: other models accept them, SE2 and the like do not,
        // meaning the list contains an id it does not recognize. Narrowing does not affect the few entries CarPlay itself needs.
        if (listRejected && identRetries_ < 2) {
            ++identRetries_;
            idListMode_ = identRetries_ == 1 ? IdList::Minimal : IdList::Omit;
            log(std::string("iAP2 narrowed the message list, resending identification: ") +
                (idListMode_ == IdList::Minimal ? "only CarPlay essentials" : "no list at all"));
            error_.clear();
            sendIdentification();
            break;
        }

        error_ = "Identification rejected by the phone (parameter ids " + ids + ")";
        break;
    }

    case kRequestAuthenticationCert: {
        log("iAP2 rx=0xaa00 certificate requested");
        Params p;
        p.bytes(0, mfi_ ? mfi_->certificate() : std::vector<uint8_t>{});
        sendMessage(kAuthenticationCertificate, p);
        break;
    }

    case kRequestChallengeResponse: {
        const Param* ch = findParam(params, 0);
        if (!ch) { error_ = "0xAA02 is missing the challenge parameter"; return; }
        log("iAP2 rx=0xaa02 challenge " + std::to_string(ch->payload.size()) + " bytes");
        bool ok = false;
        auto sig = mfi_ ? mfi_->signDigest(ch->payload.data(), ch->payload.size(), &ok)
                        : std::vector<uint8_t>{};
        if (!ok) { error_ = "MFi signing failed (challenge length is not 32?)"; return; }
        Params p;
        p.bytes(0, sig);
        sendMessage(kAuthenticationResponse, p);
        break;
    }

    case kAuthenticationSucceeded:
        log("iAP2 rx=0xaa05 authentication passed");
        setStage(TriggerStage::Authenticated);
        sendSubscriptions();
        setStage(TriggerStage::Subscribed);
        break;

    case kAuthenticationFailed:
        error_ = "iPhone sent 0xAA04 AuthenticationFailed";
        break;

    case kRequestAccessoryWifiConfig: {
        log("iAP2 rx=0x5702 Wi-Fi configuration requested");
        const int limit = transportSeen_ ? 2 : 5;
        const int used  = transportSeen_ ? postTransportSends_ : wifiConfigSends_;
        if (used >= limit) { log("  (0x5703 send limit reached, ignoring)"); break; }
        sendWifiConfiguration();
        ++wifiConfigSends_;
        if (transportSeen_) ++postTransportSends_;
        if (!transportSeen_) setStage(TriggerStage::WifiConfigSent);
        break;
    }

    case kCarPlayAvailability: {
        std::string hex;
        for (uint8_t b : body) {
            char buf[3];
            std::snprintf(buf, sizeof buf, "%02x", b);
            hex += buf;
        }
        log("iAP2 rx=0x4300 CarPlayAvailability body=" + hex);
        sendStartSession();
        break;
    }

    case kWirelessCarPlayUpdate: {
        const Param* av = findParam(params, 0);
        bool ok = false;
        const uint8_t v = av ? paramU8(*av, &ok) : 0;
        log("iAP2 rx=0x4e0d WirelessCarPlayUpdate available=" + std::to_string(ok ? v : 255));
        break;
    }

    case kDeviceTransportIdentifier:
        log("iAP2 rx=0x4e0e transport notification (resending 0x5703 once)");
        transportSeen_ = true;
        if (postTransportSends_ < 2) {
            sendWifiConfiguration();
            ++postTransportSends_;
            ++wifiConfigSends_;
        }
        break;

    default: {
        char id[8];
        std::snprintf(id, sizeof id, "%04x", msgId);
        std::string hex;
        for (uint8_t b : body) {
            char buf[3];
            std::snprintf(buf, sizeof buf, "%02x", b);
            hex += buf;
        }
        log(std::string("iAP2 rx=0x") + id + " (unhandled) body=" + hex);
        break;
    }
    }
}

bool WirelessTrigger::run()
{
    error_.clear();
    stage_ = TriggerStage::Idle;
    wifiConfigSends_ = 0;
    postTransportSends_ = 0;
    transportSeen_ = false;
    startSessionSent_ = false;

    // Identity sources: external injection (HarmonyOS rawfile / future hardware chip) > mfiDir > empty certificate
    ownedMfi_.reset();
    mfi_ = cfg_.mfiIdentity;
    if (mfi_) {
        log("MFi identity comes from external injection (" + std::to_string(mfi_->certificate().size()) + "-byte certificate)");
    } else if (!cfg_.mfiDir.empty()) {
        std::string err;
        ownedMfi_ = mfi::CertificateMfiIdentity::load(cfg_.mfiDir, &err);
        if (!ownedMfi_) {
            error_ = err;
            log(std::string("Failed to load MFi identity: ") + err);
            return false;
        }
        mfi_ = ownedMfi_.get();
    } else {
        log("No MFi identity configured: 0xAA01 will send an empty certificate (always fails on a real device; test only)");
    }

    const uint64_t triggerDeadline = nowMs() + uint64_t(cfg_.timeoutMs);
    uint64_t deadline = triggerDeadline;
    link_.start(nowMs());
    log("iAP2 link starting (waiting for marker...)");

    uint8_t buf[4096];
    while (nowMs() < deadline) {
        // The host says this session no longer needs the iAP2 link (the phone left / CarPlay ended):
        // finish and return. If 0x4301 was already sent, still report success so BtTask's rescheduling logic continues.
        if (cfg_.cancel && cfg_.cancel->load()) {
            if (!startSessionSent_) error_ = "Cancelled";
            return startSessionSent_;
        }

        link_.tick(nowMs());
        // Steering wheel: upper layers may push at any time; drain each iteration here (the link is single-threaded, so send only on this thread)
        pumpMediaKeys();

        // Nudge if the phone does not dial for a while after 0x4301: iOS does not retry after declaring this route dead,
        // so the user sees "black screen forever", and it only connects after toggling Wi-Fi.
        if (startSessionSent_ && cfg_.phoneConnected && !phoneSeen_) {
            if (cfg_.phoneConnected()) {
                phoneSeen_ = true;
                log("Phone connected to AirPlay (" +
                    std::to_string(nowMs() - startSessionAtMs_) + "ms after 0x4301)");
            } else if (nowMs() - startSessionAtMs_ >= kAirPlayRetryMs &&
                       phoneRetries_ < kAirPlayRetryMax) {
                ++phoneRetries_;
                // Send Wi-Fi configuration again before resending the session: the phone likely did not get to the 0x5703
                // join action last round, so resending only 0x4301 leaves it unmoved.
                sendWifiConfiguration();
                log("Phone did not connect to AirPlay within 3 seconds; resending 0x5703 + 0x4301 (attempt " +
                    std::to_string(phoneRetries_) + ")");
                sendStartSession();
            }
        }

        // After 0x4301 this iAP2 link is the "heartbeat line" of wireless CarPlay: iOS keeps
        // sending messages on it, and the accessory must keep reading and ACKing. **Stop reading for ~30 seconds and iOS decides the accessory is offline
        // and tears down the entire CarPlay session** (measured: exiting the loop when the 90-second trigger window expired, two
        // sessions were taken down by the phone 88s / 118s after starting, with TEARDOWN always after reading stopped).
        // So do not return after the trigger window — keep the link alive until it actually breaks (read returns
        // a negative value / the link is declared dead / a write fails); that is when "the phone is gone".
        if (startSessionSent_ && deadline == triggerDeadline)
            deadline = nowMs() + kSessionKeepAliveMs;

        std::vector<uint8_t> out;
        if (link_.takeOutput(&out)) {
            if (!stream_->writeAll(out.data(), out.size())) {
                error_ = "RFCOMM write failed";
                return startSessionSent_;
            }
        }

        if (link_.dead()) {
            error_ = "Link declared dead: " + link_.deadReason();
            return startSessionSent_;
        }

        std::vector<uint8_t> ctrl;
        while (link_.nextControl(&ctrl)) {
            framer_.feed(ctrl.data(), ctrl.size());
            uint16_t msgId = 0;
            std::vector<uint8_t> body;
            while (framer_.next(&msgId, &body)) {
                handleMessage(msgId, body);
                if (!error_.empty()) return startSessionSent_;
            }
        }

        const int n = stream_->read(buf, sizeof buf, 100);
        if (n < 0) {
            error_ = "RFCOMM disconnected";
            return startSessionSent_;
        }
        if (n > 0) link_.feed(buf, size_t(n), nowMs());
    }

    if (!startSessionSent_)
        error_ = "Trigger timed out (0x4301 not reached within " + std::to_string(cfg_.timeoutMs) + "ms)";
    return startSessionSent_;
}

}  // namespace core::iap2
