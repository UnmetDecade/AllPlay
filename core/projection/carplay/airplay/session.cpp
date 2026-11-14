#include "airplay/session.h"

#include "bplist/bplist.h"
#include "common/log.h"
#include "crypto/aes_ctr.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/ed25519.h"
#include "crypto/hkdf.h"
#include "crypto/tlv8.h"
#include "crypto/x25519.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

namespace core::airplay {

namespace {

// TLV types (per the AirPlay pair-setup/pair-verify protocol)
enum : uint8_t {
    T_METHOD = 0x00, T_IDENTIFIER = 0x01, T_SALT = 0x02, T_PUBLIC_KEY = 0x03,
    T_PROOF = 0x04, T_ENCRYPTED_DATA = 0x05, T_STATE = 0x06, T_ERROR = 0x07,
    T_SIGNATURE = 0x0a,
};
constexpr uint8_t kErrAuth = 2;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool pathIs(const std::string& path, const char* suffix)
{
    const std::string p = lower(path);
    const std::string s = lower(suffix);
    return p.size() >= s.size() && p.compare(p.size() - s.size(), s.size(), s) == 0;
}

std::vector<uint8_t> bytesOf(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Audio format bits -> codec/sample rate/channels (per the protocol's audio format bit definitions)
struct AudioFmt { AudioCodec codec = AudioCodec::LPCM; int rate = 44100; int channels = 2; };

AudioFmt parseAudioFormat(uint64_t bits)
{
    constexpr uint64_t kOpus = 0x10000000ULL | 0x20000000ULL | 0x40000000ULL;
    constexpr uint64_t kAac48 = 0x800000ULL;
    constexpr uint64_t kAac44 = 0x400000ULL;

    if (bits & kOpus)  return {AudioCodec::OPUS,   48000, 1};
    if (bits & kAac48) return {AudioCodec::AAC_LC, 48000, 2};
    if (bits & kAac44) return {AudioCodec::AAC_LC, 44100, 2};

    // LPCM: mapping in the low 16 bits (8k/16k/24k/32k/44.1k/48k x mono/stereo)
    const struct { uint64_t bit; int rate; int ch; } pcm[] = {
        {0x8000, 48000, 2}, {0x4000, 48000, 1}, {0x800, 44100, 2}, {0x400, 44100, 1},
        {0x200, 32000, 2},  {0x100, 32000, 1},  {0x80, 24000, 2},  {0x40, 24000, 1},
        {0x20, 16000, 2},   {0x10, 16000, 1},   {0x8,  8000,  2},  {0x4,  8000,  1},
    };
    for (const auto& p : pcm)
        if (bits & p.bit) return {AudioCodec::LPCM, p.rate, p.ch};
    return {AudioCodec::LPCM, 44100, 2};
}

}  // namespace

Session::Session(const SessionConfig& cfg)
    : identity_(cfg.identity), info_(cfg.info), stateDir_(cfg.stateDir),
      services_(cfg.services), video_(cfg.video), audio_(cfg.audio),
      width_(cfg.width), height_(cfg.height)
{
    // Identity sources: external injection (HarmonyOS HAP rawfile) > mfiDir (identity/ directory) > none.
    if (cfg.mfiIdentity != nullptr) {
        mfi_ = cfg.mfiIdentity;
        LOG_INFO("MFi identity comes from external injection (%zu-byte certificate)", mfi_->certificate().size());
    } else if (!cfg.mfiDir.empty()) {
        std::string err;
        ownedMfi_ = mfi::CertificateMfiIdentity::load(cfg.mfiDir, &err);
        if (!ownedMfi_) LOG_WARN("Failed to load MFi identity: %s (/auth-setup will return 400)", err.c_str());
        mfi_ = ownedMfi_.get();
    }
    pairings_.load(stateDir_);
}

Session::~Session()
{
    stopNtp_.store(true);
    if (ntpSock_) ntpSock_->close();
    if (ntpThread_.joinable()) ntpThread_.join();

    stopKeepAlive_.store(true);
    if (keepAliveSock_) keepAliveSock_->close();
    if (keepAliveThread_.joinable()) keepAliveThread_.join();

    stopKeyframe_.store(true);
    if (keyframeThread_.joinable()) keyframeThread_.join();
}

RtspResponse Session::handle(const RtspRequest& req)
{
    const std::string path = req.uri;
    LOG_INFO("RTSP %s %s", req.method.c_str(), path.c_str());
    connecting_ = true;   // any RTSP request received = the iPhone is connecting

    if (req.method == "OPTIONS") {
        RtspResponse r = RtspResponse::ok();
        r.setHeader("Public",
                    "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, "
                    "GET_PARAMETER, SET_PARAMETER, POST, GET");
        return r;
    }
    if (req.method == "GET" && pathIs(path, "/info")) return handleInfo();
    if (req.method == "POST" && pathIs(path, "/auth-setup")) return handleAuthSetup(req);
    if (req.method == "POST" && pathIs(path, "/pair-setup")) return handlePairSetup(req);
    if (req.method == "POST" && pathIs(path, "/pair-verify")) return handlePairVerify(req);
    if (req.method == "SETUP") return handleSetup(req);
    if (req.method == "RECORD") { sessionActive_ = true; connecting_ = false; return RtspResponse::ok(); }
    if (req.method == "TEARDOWN") { markDisconnected(); return RtspResponse::ok(); }
    if (req.method == "POST" && pathIs(path, "/command")) return handleCommand(req);
    if (req.method == "POST" && pathIs(path, "/feedback")) return handleFeedback(req);

    return RtspResponse::ok();   // unrecognized routes such as ANNOUNCE
}

RtspResponse Session::handleInfo()
{
    RtspResponse r = RtspResponse::ok();
    r.setHeader("Content-Type", "application/x-apple-binary-plist");
    r.body = buildInfoPlist(*info_);
    const std::string s(r.body.begin(), r.body.end());
    LOG_INFO("/info %zu bytes features=0x%llx audioFormats=%s audioLatencies=%s bluetoothIDs=%s",
             r.body.size(), (unsigned long long)info_->features,
             s.find("audioFormats") != std::string::npos ? "yes" : "no",
             s.find("audioLatencies") != std::string::npos ? "yes" : "no",
             s.find("bluetoothIDs") != std::string::npos ? "yes" : "no");
    return r;
}

RtspResponse Session::handleAuthSetup(const RtspRequest& req)
{
    // Request: [1B ver=1][32B peer X25519 public key]
    if (req.body.size() != 33 || req.body[0] != 1 || !mfi_) return RtspResponse::badRequest();
    const uint8_t* phonePub = req.body.data() + 1;

    auto eph = crypto::x25519Generate();
    auto shared = crypto::x25519Shared(eph.priv.data(), phonePub);
    if (shared.empty()) return RtspResponse::badRequest();

    auto mkey = [&](const char* label) {
        std::vector<uint8_t> in(label, label + std::strlen(label));
        in.insert(in.end(), shared.begin(), shared.end());
        auto h = crypto::sha1(in);
        h.resize(16);
        return h;
    };
    const auto aesKey = mkey("AES-KEY");
    const auto aesIv  = mkey("AES-IV");

    // digest = SHA256(our ephemeral X25519 public key || phone public key) (LocalMfi protocolMajor=3 -> SHA256 branch)
    std::vector<uint8_t> di(eph.pub.begin(), eph.pub.end());
    di.insert(di.end(), phonePub, phonePub + 32);
    auto digest = crypto::sha256(di);

    bool ok = false;
    auto sig = mfi_->signDigest(digest.data(), digest.size(), &ok);
    if (!ok) return RtspResponse::badRequest();
    auto encSig = crypto::aes128Ctr(aesKey.data(), aesIv.data(), sig.data(), sig.size());

    // Response: pub(32) || BE32(certLen) || cert || BE32(sigLen) || AES-CTR(sig)
    std::vector<uint8_t> out(eph.pub.begin(), eph.pub.end());
    auto putU32 = [&](uint32_t v) {
        out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
        out.push_back(uint8_t(v >> 8));  out.push_back(uint8_t(v));
    };
    const auto& cert = mfi_->certificate();
    putU32(uint32_t(cert.size()));
    out.insert(out.end(), cert.begin(), cert.end());
    putU32(uint32_t(encSig.size()));
    out.insert(out.end(), encSig.begin(), encSig.end());

    RtspResponse r = RtspResponse::ok();
    r.setHeader("Content-Type", "application/octet-stream");
    r.body = std::move(out);
    return r;
}

RtspResponse Session::handlePairSetup(const RtspRequest& req)
{
    auto items = crypto::tlv8Decode(req.body.data(), req.body.size());
    const auto* sv = crypto::tlv8Get(items, T_STATE);
    const int state = (sv && !sv->empty()) ? sv->front() : 0;

    switch (state) {
    case 1: {
        srp_ = std::make_unique<crypto::Srp6aServer>();
        std::vector<crypto::Tlv> out = {
            {T_STATE, {2}},
            {T_PUBLIC_KEY, srp_->publicKey()},
            {T_SALT, srp_->salt()},
        };
        return tlv8Response(crypto::tlv8Encode(out));
    }
    case 3: {
        const auto* A = crypto::tlv8Get(items, T_PUBLIC_KEY);
        const auto* M1 = crypto::tlv8Get(items, T_PROOF);
        std::vector<uint8_t> K, M2;
        if (!srp_ || !A || !M1 ||
            !srp_->verify(A->data(), A->size(), M1->data(), M1->size(), &K, &M2))
            return pairErr(4);
        sessionKey_ = std::move(K);
        return tlv8Response(crypto::tlv8Encode({{T_STATE, {4}}, {T_PROOF, std::move(M2)}}));
    }
    case 5: {
        const auto* enc = crypto::tlv8Get(items, T_ENCRYPTED_DATA);
        if (!enc || sessionKey_.empty() || !identity_) return pairErr(6);

        auto encKey = crypto::hkdfSha512(sessionKey_, "Pair-Setup-Encrypt-Salt",
                                         "Pair-Setup-Encrypt-Info", 32);
        std::vector<uint8_t> subBytes;
        if (!crypto::chachaOpen(encKey.data(), crypto::nonceLabel("PS-Msg05"),
                                nullptr, 0, enc->data(), enc->size(), &subBytes))
            return pairErr(6);
        auto sub = crypto::tlv8Decode(subBytes.data(), subBytes.size());
        const auto* cid  = crypto::tlv8Get(sub, T_IDENTIFIER);
        const auto* ltpk = crypto::tlv8Get(sub, T_PUBLIC_KEY);
        const auto* csig = crypto::tlv8Get(sub, T_SIGNATURE);
        if (!cid || !ltpk || ltpk->size() != 32 || !csig || csig->size() != 64) return pairErr(6);

        auto cSignKey = crypto::hkdfSha512(sessionKey_, "Pair-Setup-Controller-Sign-Salt",
                                           "Pair-Setup-Controller-Sign-Info", 32);
        std::vector<uint8_t> signData = cSignKey;
        signData.insert(signData.end(), cid->begin(), cid->end());
        signData.insert(signData.end(), ltpk->begin(), ltpk->end());
        if (!crypto::ed25519Verify(ltpk->data(), signData.data(), signData.size(), csig->data()))
            return pairErr(6);

        const std::string controllerId(cid->begin(), cid->end());
        pairings_.save(stateDir_, controllerId, *ltpk);

        // accessory sends the signature back
        const std::string accId = identity_->pi;
        auto aSignKey = crypto::hkdfSha512(sessionKey_, "Pair-Setup-Accessory-Sign-Salt",
                                           "Pair-Setup-Accessory-Sign-Info", 32);
        std::vector<uint8_t> aData = aSignKey;
        aData.insert(aData.end(), accId.begin(), accId.end());
        aData.insert(aData.end(), identity_->pub.begin(), identity_->pub.end());
        auto aSig = crypto::ed25519Sign(identity_->priv, aData.data(), aData.size());
        if (aSig.size() != 64) return pairErr(6);

        std::vector<crypto::Tlv> subOut = {
            {T_IDENTIFIER, bytesOf(accId)},
            {T_PUBLIC_KEY, identity_->pub},
            {T_SIGNATURE, aSig},
        };
        auto subEnc = crypto::tlv8Encode(subOut);
        auto sealed = crypto::chachaSeal(encKey.data(), crypto::nonceLabel("PS-Msg06"),
                                         nullptr, 0, subEnc.data(), subEnc.size());
        return tlv8Response(crypto::tlv8Encode({{T_STATE, {6}}, {T_ENCRYPTED_DATA, sealed}}));
    }
    default:
        return pairErr(state);
    }
}

RtspResponse Session::handlePairVerify(const RtspRequest& req)
{
    auto items = crypto::tlv8Decode(req.body.data(), req.body.size());
    const auto* sv = crypto::tlv8Get(items, T_STATE);
    const int state = (sv && !sv->empty()) ? sv->front() : 0;

    switch (state) {
    case 1: {
        const auto* cp = crypto::tlv8Get(items, T_PUBLIC_KEY);
        if (!cp || cp->size() != 32 || !identity_) return pairErr(2);
        clientEphPub_ = *cp;
        eph_ = crypto::x25519Generate();
        shared_ = crypto::x25519Shared(eph_.priv.data(), clientEphPub_.data());
        if (shared_.empty()) return pairErr(2);
        encKey_ = crypto::hkdfSha512(shared_, "Pair-Verify-Encrypt-Salt",
                                     "Pair-Verify-Encrypt-Info", 32);

        const std::string accId = identity_->pi;
        std::vector<uint8_t> sigData(eph_.pub.begin(), eph_.pub.end());
        sigData.insert(sigData.end(), accId.begin(), accId.end());
        sigData.insert(sigData.end(), clientEphPub_.begin(), clientEphPub_.end());
        auto sig = crypto::ed25519Sign(identity_->priv, sigData.data(), sigData.size());

        std::vector<crypto::Tlv> sub = {
            {T_IDENTIFIER, bytesOf(accId)},
            {T_SIGNATURE, sig},
        };
        auto subEnc = crypto::tlv8Encode(sub);
        auto sealed = crypto::chachaSeal(encKey_.data(), crypto::nonceLabel("PV-Msg02"),
                                         nullptr, 0, subEnc.data(), subEnc.size());
        std::vector<crypto::Tlv> out = {
            {T_STATE, {2}},
            {T_PUBLIC_KEY, std::vector<uint8_t>(eph_.pub.begin(), eph_.pub.end())},
            {T_ENCRYPTED_DATA, sealed},
        };
        return tlv8Response(crypto::tlv8Encode(out));
    }
    case 3: {
        const auto* enc = crypto::tlv8Get(items, T_ENCRYPTED_DATA);
        if (!enc || encKey_.empty() || !pairings_.hasPairing()) return pairErr(4);

        std::vector<uint8_t> subBytes;
        if (!crypto::chachaOpen(encKey_.data(), crypto::nonceLabel("PV-Msg03"),
                                nullptr, 0, enc->data(), enc->size(), &subBytes))
            return pairErr(4);
        auto sub = crypto::tlv8Decode(subBytes.data(), subBytes.size());
        const auto* cid  = crypto::tlv8Get(sub, T_IDENTIFIER);
        const auto* csig = crypto::tlv8Get(sub, T_SIGNATURE);
        if (!cid || !csig || csig->size() != 64) return pairErr(4);

        if (std::string(cid->begin(), cid->end()) != pairings_.controllerId()) return pairErr(4);

        std::vector<uint8_t> sigData(clientEphPub_.begin(), clientEphPub_.end());
        sigData.insert(sigData.end(), cid->begin(), cid->end());
        sigData.insert(sigData.end(), eph_.pub.begin(), eph_.pub.end());
        const auto& ltpk = pairings_.controllerLtpk();
        if (!crypto::ed25519Verify(ltpk.data(), sigData.data(), sigData.size(), csig->data()))
            return pairErr(4);

        // Control keys: labels are reversed (accessory read = phone write)
        auto readKey  = crypto::hkdfSha512(shared_, "Control-Salt", "Control-Write-Encryption-Key", 32);
        auto writeKey = crypto::hkdfSha512(shared_, "Control-Salt", "Control-Read-Encryption-Key", 32);
        std::copy(readKey.begin(),  readKey.end(),  controlKeys_.readKey.begin());
        std::copy(writeKey.begin(), writeKey.end(), controlKeys_.writeKey.begin());
        controlKeysReady_ = true;

        return tlv8Response(crypto::tlv8Encode({{T_STATE, {4}}}));
    }
    default:
        return pairErr(state);
    }
}

RtspResponse Session::handleSetup(const RtspRequest& req)
{
    auto root = bplist::parse(req.body.data(), req.body.size());
    if (!root) return RtspResponse::badRequest();

    auto streams = bplist::dictGet(root, "streams");
    RtspResponse r = RtspResponse::ok();
    r.setHeader("Content-Type", "application/x-apple-binary-plist");

    // Diagnostics: dump the SETUP request's keys + features (when batch 1 has no streams)
    {
        std::string keys;
        for (const auto& kv : root->dict) keys += kv.first + ",";
        auto feat = bplist::dictGet(root, "features");
        std::string fl;
        if (feat && feat->type == bplist::NodeType::Array)
            for (const auto& f : feat->array) fl += bplist::strOf(f) + ",";
        LOG_INFO("SETUP keys=[%s] features=[%s]", keys.c_str(), fl.c_str());
    }

    if (streams) {
        // Batch 2: streams[]. 110/111 create screen streams; 100/101/102 create audio streams (UDP RTP); 130 data is received and dropped.
        std::vector<bplist::Ptr> outStreams;
        for (size_t i = 0; i < streams->array.size(); ++i) {
            auto s = bplist::arrGet(streams, i);
            const uint64_t type = bplist::uintOf(bplist::dictGet(s, "type"));
            const uint64_t scid = bplist::uintOf(bplist::dictGet(s, "streamConnectionID"));
            LOG_INFO("SETUP stream type=%llu audioFormat=0x%llx scid=%llu",
                     (unsigned long long)type,
                     (unsigned long long)bplist::uintOf(bplist::dictGet(s, "audioFormat")),
                     (unsigned long long)scid);

            if (type == 110 || type == 111) {
                // screen stream (TCP dataPort)
                // Do not skip silently in either case: without a dataPort the phone just waits over ten seconds and then disconnects,
                // leaving only "disconnects right after SETUP" in the log, which is very hard to diagnose (hit once on a real device).
                if (!video_) {
                    LOG_WARN("SETUP requested a screen stream, but this process has no IVideoOutput - not creating the screen stream; "
                             "without a dataPort the phone will time out and disconnect");
                    continue;
                }
                if (!services_ || !services_->makeTcpListener || shared_.empty()) continue;
                auto listener = services_->makeTcpListener();
                std::string lerr;
                if (!listener || !listener->start("", 0, &lerr)) {
                    LOG_WARN("Failed to start screen stream listener: %s", lerr.c_str());
                    continue;
                }
                const uint16_t port = listener->port();
                streams_.push_back(std::make_unique<ScreenStream>(
                    std::move(listener), shared_, std::to_string(scid), width_, height_, video_,
                    [this] { requestKeyFrame(); }));
                outStreams.push_back(bplist::dict({
                    {"type", bplist::u(type)},
                    {"dataPort", bplist::u(port)},
                }));
            } else if (type == 100 || type == 101 || type == 102) {
                // audio stream (UDP RTP dataPort + RTCP controlPort)
                if (!services_ || !services_->makeUdpSocket || !audio_ || shared_.empty()) continue;
                const AudioFmt fmt = parseAudioFormat(bplist::uintOf(bplist::dictGet(s, "audioFormat")));
                audio_->configure(fmt.codec, fmt.rate, fmt.channels);
                audioStreams_.push_back(std::make_unique<AudioStream>(
                    services_->makeUdpSocket, shared_, std::to_string(scid),
                    fmt.codec, fmt.rate, fmt.channels, audio_));
                outStreams.push_back(bplist::dict({
                    {"type", bplist::u(type)},
                    {"dataPort", bplist::u(audioStreams_.back()->dataPort())},
                    {"controlPort", bplist::u(audioStreams_.back()->controlPort())},
                    {"streamConnectionID", bplist::u(scid)},
                }));
            }
        }
        r.body = bplist::serialize(bplist::dict({{"streams", bplist::array(outStreams)}}));
        return r;
    }

    // Batch 1: parameter negotiation. eventPort/timingPort/keepAlivePort must be real ports
    openEventChannel();   // if the channel exists, only swap in this session's keys (see the comment in the function)
    if (!ntpSock_) openNtp();
    const bool wantKeepAlive = bplist::boolOf(bplist::dictGet(root, "keepAliveLowPower"));
    if (wantKeepAlive && !keepAliveSock_) openKeepAlive();
    const uint16_t eventPort    = eventChannel_ ? eventChannel_->port() : 0;
    const uint16_t timingPort   = ntpSock_ ? ntpSock_->port() : 0;
    const uint16_t keepAlivePort = keepAliveSock_ ? keepAliveSock_->port() : 0;

    std::vector<std::pair<std::string, bplist::Ptr>> d = {
        {"timingPort", bplist::u(timingPort)},
        {"eventPort",  bplist::u(eventPort)},
        {"enabledFeatures", bplist::array({bplist::str("iAPChannel"), bplist::str("viewAreas")})},
    };
    if (keepAlivePort) d.push_back({"keepAlivePort", bplist::u(keepAlivePort)});
    r.body = bplist::serialize(bplist::dict(d));
    return r;
}

RtspResponse Session::handleCommand(const RtspRequest& req)
{
    auto root = bplist::parse(req.body.data(), req.body.size());
    const std::string type = root ? bplist::strOf(bplist::dictGet(root, "type")) : "";
    if (type == "forceKeyFrame") {
        for (auto& s : streams_) s->requestKeyFrame();
        LOG_INFO("/command forceKeyFrame: requested a keyframe (%zu screen streams)", streams_.size());
    }
    return RtspResponse::ok();
}

RtspResponse Session::handleFeedback(const RtspRequest& req)
{
    // The phone sends this every ~2s. A proper receiver replies with a timing plist reflecting its actual buffer depth,
    // and the phone adjusts bitrate/frame rate accordingly; an empty reply makes it blindly send at maximum quality.
    static int n = 0;
    if (++n <= 3 || n % 30 == 0) {
        std::string hx;
        for (size_t i = 0; i < req.body.size() && i < 64; ++i) {
            char b[3];
            std::snprintf(b, sizeof b, "%02x", req.body[i]);
            hx += b;
        }
        LOG_INFO("/feedback #%d: ct=%s body=%zu head=%s", n, req.header("Content-Type").c_str(),
                 req.body.size(), hx.c_str());
        if (!req.body.empty() && req.body[0] == '<') {
            std::string txt(req.body.begin(), req.body.end());
            if (txt.size() > 600) txt.resize(600);
            LOG_INFO("/feedback body: %s", txt.c_str());
        }
    }
    return RtspResponse::ok();
}

void Session::openEventChannel()
{
    if (!services_ || !services_->makeTcpListener || shared_.empty()) return;

    if (eventChannel_) {
        // The channel object is reused across sessions (the port stays the same), but the keys are **per session**: the write key comes from this
        // session's pair-verify shared_ via HKDF. Without swapping, the periodic forceKeyFrame the host sends
        // cannot be decrypted by the phone, and iOS kills the entire CarPlay session — measured: after the phone reconnects,
        // the session always drops exactly on the next tick of the 5-second timer (16:11:01, 16:13:01).
        eventChannel_->rekey(shared_);
        return;
    }

    auto l = services_->makeTcpListener();
    std::string lerr;
    if (!l || !l->start("", 0, &lerr)) {
        LOG_WARN("Event channel listen failed: %s", lerr.c_str());
        return;
    }
    eventChannel_.reset(new EventChannel(std::move(l), shared_, width_, height_));

    // Periodically request keyframes: reset the decoder reference chain to prevent visual corruption accumulating across P-frames (per the protocol's referenceChain + keyframe request)
    stopKeyframe_.store(false);
    keyframeThread_ = std::thread([this] {
        while (!stopKeyframe_.load()) {
            for (int i = 0; i < 50 && !stopKeyframe_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 5s
            if (stopKeyframe_.load()) break;
            requestKeyFrame();
        }
    });
}

void Session::openNtp()
{
    if (!services_ || !services_->makeUdpSocket) return;
    auto sock = services_->makeUdpSocket();
    std::string err;
    if (!sock || !sock->bind(0, &err)) {
        LOG_WARN("NTP UDP bind failed: %s", err.c_str());
        return;
    }
    ntpSock_ = std::move(sock);
    stopNtp_.store(false);
    ntpThread_ = std::thread([this] {
        uint8_t buf[128];
        while (!stopNtp_.load()) {
            const int n = ntpSock_->recv(buf, sizeof buf, 500);
            if (n < 48) continue;
            // Minimal NTP response (48B): originate = request transmit; receive/transmit = now
            std::array<uint8_t, 48> resp{};
            resp[0] = 0x1C;                                  // LI=0 VN=3 Mode=4(server)
            std::memcpy(resp.data() + 24, buf + 40, 8);      // originate
            const uint64_t now = (uint64_t(std::time(nullptr)) + 2208988800ULL) << 32;
            auto put = [&](size_t off, uint64_t v) {
                for (int i = 7; i >= 0; --i) { resp[off + i] = uint8_t(v & 0xff); v >>= 8; }
            };
            put(32, now);   // receive
            put(40, now);   // transmit
            ntpSock_->send(resp.data(), resp.size());
        }
    });
    LOG_INFO("NTP timing port ready :%u", ntpSock_->port());
}

void Session::openKeepAlive()
{
    if (!services_ || !services_->makeUdpSocket) return;
    auto sock = services_->makeUdpSocket();
    std::string err;
    if (!sock || !sock->bind(0, &err)) {
        LOG_WARN("keep-alive UDP bind failed: %s", err.c_str());
        return;
    }
    keepAliveSock_ = std::move(sock);
    stopKeepAlive_.store(false);
    keepAliveThread_ = std::thread([this] {
        uint8_t buf[512];
        while (!stopKeepAlive_.load())
            keepAliveSock_->recv(buf, sizeof buf, 1000);   // receive only, no response
    });
    LOG_INFO("keep-alive port ready :%u", keepAliveSock_->port());
}

bool Session::sendTouch(const std::vector<TouchPoint>& contacts)
{
    return eventChannel_ && eventChannel_->sendTouch(contacts);
}

void Session::requestKeyFrame()
{
    if (!eventChannel_) return;
    // Throttle: the dropped-frame callback can fire rapidly; send at most once per second to avoid flooding the event channel
    const auto now = std::chrono::steady_clock::now();
    if (now - lastKeyframeRequest_ < std::chrono::seconds(1)) return;
    lastKeyframeRequest_ = now;
    const auto cmd = bplist::dict({{"type", bplist::str("forceKeyFrame")}});
    eventChannel_->sendCommand(cmd);
}

void Session::requestKeyFrameNow()
{
    if (!eventChannel_) return;
    lastKeyframeRequest_ = std::chrono::steady_clock::now();   // refresh the throttle baseline so the periodic request right after does not send again
    const auto cmd = bplist::dict({{"type", bplist::str("forceKeyFrame")}});
    eventChannel_->sendCommand(cmd);
}

void Session::markDisconnected()
{
    sessionActive_ = false;
    connecting_ = false;
}

RtspResponse Session::tlv8Response(const std::vector<uint8_t>& body)
{
    RtspResponse r = RtspResponse::ok();
    r.setHeader("Content-Type", "application/pairing+tlv8");
    r.body = body;
    return r;
}

RtspResponse Session::pairErr(int state)
{
    std::vector<crypto::Tlv> out = {{T_STATE, {uint8_t(state)}}, {T_ERROR, {kErrAuth}}};
    return tlv8Response(crypto::tlv8Encode(out));
}

}  // namespace core::airplay
