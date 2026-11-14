/**
 * @file   backend_androidauto.cpp
 * @brief  Android Auto wired backend: AOAP -> AAP -> TLS -> service discovery -> audio/video/input
 */

#include "projection/androidauto/backend_androidauto.h"

#include "projection/androidauto/aap.h"
#include "projection/androidauto/proto.h"
#include "projection/androidauto/tls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace core::projection::androidauto {

namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
uint64_t readU64BE(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// ---------------------------------------------------------------------------
// protobuf message construction
// ---------------------------------------------------------------------------
std::vector<uint8_t> pbVideoConfig(uint32_t resolution, uint32_t fps)
{
    std::vector<uint8_t> c;
    pb::putUInt(c, 1, resolution);
    pb::putUInt(c, 2, fps);
    return c;
}

std::vector<uint8_t> pbAudioConfig(int rate, int bits, int channels)
{
    std::vector<uint8_t> c;
    pb::putUInt(c, 1, uint32_t(rate));
    pb::putUInt(c, 2, uint32_t(bits));
    pb::putUInt(c, 3, uint32_t(channels));
    return c;
}

std::vector<uint8_t> pbAvVideo(int fps)
{
    std::vector<uint8_t> a;
    pb::putUInt(a, 1, kAvStreamVideo);
    // Advertise a single resolution: config_index is therefore always 0, and touch coordinates map 1:1 to video pixels.
    pb::putMessage(a, 4, pbVideoConfig(kRes720p, fps >= 60 ? kFps60 : kFps30));
    return a;
}

std::vector<uint8_t> pbAvAudio(int rate, int bits, int channels)
{
    std::vector<uint8_t> a;
    pb::putUInt(a, 1, kAvStreamAudio);
    pb::putUInt(a, 2, kAudioTypeMedia);
    pb::putMessage(a, 3, pbAudioConfig(rate, bits, channels));
    return a;
}

std::vector<uint8_t> pbTouchConfig(int width, int height)
{
    std::vector<uint8_t> t;
    pb::putUInt(t, 1, uint32_t(width));
    pb::putUInt(t, 2, uint32_t(height));
    return t;
}

std::vector<uint8_t> pbInputChannel(int width, int height)
{
    std::vector<uint8_t> c;
    // Supported keys (Android KeyEvent values, matching AASDK ButtonCodeEnum):
    // HOME=3 BACK=4 CALL=5 ENDCALL=6 DPAD_UP/DOWN/LEFT/RIGHT/CENTER=19..23
    for (uint32_t code : {3u, 4u, 5u, 6u, 19u, 20u, 21u, 22u, 23u})
        pb::putUInt(c, 1, code);
    pb::putMessage(c, 2, pbTouchConfig(width, height));
    return c;
}

std::vector<uint8_t> pbSensorChannel()
{
    std::vector<uint8_t> c;
    for (uint32_t type : {uint32_t(kSensorNightMode), uint32_t(kSensorDrivingStatus)}) {
        std::vector<uint8_t> s;
        pb::putUInt(s, 1, type);
        pb::putMessage(c, 1, s);
    }
    return c;
}

std::vector<uint8_t> pbChannelDescriptor(uint32_t channelId, uint32_t field,
                                         const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> d;
    pb::putUInt(d, 1, channelId);
    pb::putMessage(d, field, body);
    return d;
}

std::vector<uint8_t> pbServiceDiscovery(const AndroidAutoOptions& opt)
{
    std::vector<uint8_t> r;
    pb::putMessage(r, 1, pbChannelDescriptor(uint32_t(Channel::Video), 3,
                                             pbAvVideo(opt.fps)));
    pb::putMessage(r, 1, pbChannelDescriptor(uint32_t(Channel::MediaAudio), 3,
                                             pbAvAudio(opt.audio_rate, opt.audio_bits,
                                                       opt.audio_channels)));
    pb::putMessage(r, 1, pbChannelDescriptor(uint32_t(Channel::Input), 4,
                                             pbInputChannel(opt.width, opt.height)));
    pb::putMessage(r, 1, pbChannelDescriptor(uint32_t(Channel::Sensor), 2,
                                             pbSensorChannel()));
    pb::putString(r, 2, opt.name);
    pb::putString(r, 7, opt.manufacturer);
    pb::putString(r, 8, opt.model);
    pb::putString(r, 9, "1.0");
    pb::putString(r, 10, "1.0");
    pb::putBool(r, 6, true);   // left-hand drive
    return r;
}

// ---------------------------------------------------------------------------
// H.264 parameter sets: AA sends no avcC, only in-band SPS/PPS, so the decoder must build it itself
// ---------------------------------------------------------------------------
bool startCodeAt(const uint8_t* p, size_t avail, size_t* len)
{
    if (avail >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { *len = 4; return true; }
    if (avail >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) { *len = 3; return true; }
    return false;
}

void scanParamSets(const uint8_t* data, size_t len,
                   std::vector<uint8_t>* sps, std::vector<uint8_t>* pps)
{
    size_t i = 0;
    while (i < len) {
        size_t sc = 0;
        if (!startCodeAt(data + i, len - i, &sc)) { ++i; continue; }
        const size_t nalStart = i + sc;
        size_t next = nalStart;
        while (next < len) {
            size_t sc2 = 0;
            if (startCodeAt(data + next, len - next, &sc2)) break;
            ++next;
        }
        if (nalStart < len) {
            const uint8_t type = data[nalStart] & 0x1F;
            if (type == 7 && sps->empty()) sps->assign(data + nalStart, data + next);
            if (type == 8 && pps->empty()) pps->assign(data + nalStart, data + next);
        }
        i = next > i ? next : i + 1;
    }
}

bool makeAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
              std::vector<uint8_t>* out)
{
    if (sps.size() < 4 || pps.empty()) return false;
    out->clear();
    out->push_back(1);
    out->push_back(sps[1]);
    out->push_back(sps[2]);
    out->push_back(sps[3]);
    out->push_back(0xFF);                 // 4-byte length prefix + 1 SPS
    out->push_back(0xE1);
    out->push_back(uint8_t(sps.size() >> 8));
    out->push_back(uint8_t(sps.size()));
    out->insert(out->end(), sps.begin(), sps.end());
    out->push_back(1);
    out->push_back(uint8_t(pps.size() >> 8));
    out->push_back(uint8_t(pps.size()));
    out->insert(out->end(), pps.begin(), pps.end());
    return true;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
class AaSession final : public IProjectionSession {
public:
    AaSession(AndroidAutoOptions opt, const core::PlatformServices& services,
              IProjectionSink& sink, std::string deviceId)
        : opt_(std::move(opt)), sink_(sink), deviceId_(std::move(deviceId))
    {
        (void)services;   // Wired AA needs no platform services (Bluetooth / network)
    }

    ~AaSession() override { stop(); }

    bool start(std::string* err) override
    {
        if (thread_.joinable()) {
            if (err) *err = "Session has already been started";
            return false;
        }
        stop_.store(false);
        sink_.onState(SessionState::Connecting, "AOAP accessory mode");
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        dev_.reset();
    }

    bool sendInput(const InputEvent& ev, std::string* err) override
    {
        if (ev.kind != InputKind::Touch) {
            if (err) *err = "Android Auto currently supports touch input only";
            return false;
        }
        if (!running_.load()) {
            if (err) *err = "Session has not reached Running yet";
            return false;
        }

        uint32_t action = kTouchDrag;
        if (ev.phase == TouchPhase::Down) action = kTouchPress;
        else if (ev.phase == TouchPhase::Up || ev.phase == TouchPhase::Cancel) action = kTouchRelease;
        else action = kTouchDrag;

        const int w = opt_.width > 0 ? opt_.width : 1280;
        const int h = opt_.height > 0 ? opt_.height : 720;
        uint32_t x = uint32_t(std::clamp(ev.x, 0.f, 1.f) * float(w));
        uint32_t y = uint32_t(std::clamp(ev.y, 0.f, 1.f) * float(h));

        std::vector<uint8_t> loc;
        pb::putUInt(loc, 1, x);
        pb::putUInt(loc, 2, y);
        pb::putUInt(loc, 3, ev.pointer);

        std::vector<uint8_t> touch;
        pb::putMessage(touch, 1, loc);
        pb::putUInt(touch, 2, touchIndex_);
        pb::putUInt(touch, 3, action);

        std::vector<uint8_t> body;
        // AA timestamps are in microseconds; use a monotonic clock, since the phone only uses it to time gestures.
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        pb::putUInt(body, 1, uint64_t(nowUs));   // timestamp
        pb::putMessage(body, 3, touch);          // touch_event

        if (action == kTouchRelease) ++touchIndex_;

        return sendMessage(Channel::Input, kMsgInputEvent, body,
                           /*control=*/false, /*encrypted=*/true, err);
    }

    void requestKeyFrame() override {}   // AA has no key frame request message

    Capabilities capabilities() const override
    {
        Capabilities c;
        c.touch      = true;
        c.max_width  = opt_.width;
        c.max_height = opt_.height;
        c.video_codecs = {VideoCodec::H264};
        c.audio_codecs = {AudioCodec::LPCM};
        return c;
    }

private:
    enum class Stage { Aoap, Version, Tls, Discovery, Running };

    void fail(const std::string& why)
    {
        sink_.onState(SessionState::Failed, why);
    }

    void run()
    {
        std::string err;

        // ---- ① AOAP: version query -> six strings -> START -> wait for re-enumeration ----
        if (!openAoap(&err)) return fail(err);

        // ---- ② AAP version ----
        sink_.onState(SessionState::Negotiating, "AAP version negotiation");
        if (!sendVersionRequest(&err)) return fail(err);
        stage_ = Stage::Version;
        const auto versionDeadline = std::chrono::steady_clock::now() + 10s;
        while (!stop_.load() && stage_ == Stage::Version) {
            if (!readOnce(500, &err)) return fail(err);
            if (std::chrono::steady_clock::now() > versionDeadline)
                return fail("Timed out waiting for VERSION_RESPONSE");
        }
        if (stop_.load()) return;

        // ---- ③ TLS (records wrapped in 0x0003) ----
        if (!tls_.init(&err)) return fail("TLS init failed: " + err);
        if (!driveHandshake(&err)) return fail(err);

        // ---- ④ Main loop ----
        while (!stop_.load()) {
            if (!readOnce(500, &err)) return fail(err);
        }
    }

    bool openAoap(std::string* err)
    {
        if (!opt_.usb.open) { if (err) *err = "This build has no Android Auto USB transport"; return false; }
        dev_ = opt_.usb.open(deviceId_, err);
        if (!dev_) return false;

        uint8_t ver[2] = {0, 0};
        if (!dev_->controlTransfer(0xC0, 51, 0, 0, ver, 2, 1000, err)) {
            if (err) *err = "AOAP GET_PROTOCOL failed: " + *err;
            return false;
        }
        const uint16_t protocol = uint16_t(ver[0] | (ver[1] << 8));
        if (protocol < 1) { if (err) *err = "The phone does not support AOAP"; return false; }

        const std::string strings[6] = {
            opt_.manufacturer, opt_.model, "Android Auto", "2.0",
            "https://github.com/UnmetDecade/AllPlay", opt_.serial,
        };
        for (uint16_t i = 0; i < 6; ++i) {
            std::vector<uint8_t> s(strings[i].begin(), strings[i].end());
            s.push_back(0);
            if (!dev_->controlTransfer(0x40, 52, 0, i, s.data(), uint16_t(s.size()),
                                       1000, err))
                return false;
        }
        if (!dev_->controlTransfer(0x40, 53, 0, 0, nullptr, 0, 1000, err))
            return false;

        dev_ = dev_->waitForAccessory(15000, err);
        if (!dev_) return false;
        if (!dev_->openBulk(err)) return false;
        return true;
    }

    bool sendVersionRequest(std::string* err)
    {
        // control channel / SPECIFIC / PLAIN: major=1 minor=1
        const std::vector<uint8_t> body = {0x00, 0x01, 0x00, 0x01};
        return sendMessage(Channel::Control, kMsgVersionRequest, body,
                           /*control=*/false, /*encrypted=*/false, err);
    }

    bool driveHandshake(std::string* err)
    {
        Tls12Client::Handshake hs;
        std::vector<uint8_t> out;
        {
            std::lock_guard<std::mutex> lk(tlsMu_);
            hs  = tls_.handshake(err);
            out = tls_.takeOutput();
        }
        if (!out.empty() && !sendMessage(Channel::Control, kMsgSslHandshake, out,
                                         /*control=*/true, /*encrypted=*/false, err))
            return false;

        if (hs == Tls12Client::Handshake::Failed) return false;
        if (hs == Tls12Client::Handshake::Done) {
            stage_ = Stage::Discovery;
            // aasdk still marks AUTH_COMPLETE as PLAIN; follow suit for now (risks in docs).
            std::vector<uint8_t> auth;
            pb::putUInt(auth, 1, kStatusOk);
            if (!sendMessage(Channel::Control, kMsgAuthComplete, auth,
                             /*control=*/true, /*encrypted=*/false, err))
                return false;
        }
        return true;
    }

    bool readOnce(int timeoutMs, std::string* err)
    {
        if (!dev_) { if (err) *err = "USB device is not ready"; return false; }
        uint8_t buf[64 * 1024];
        const int n = dev_->readBulk(buf, sizeof buf, timeoutMs, err);
        if (n < 0) return false;
        if (n == 0) return true;
        parser_.feed(buf, size_t(n));
        Frame f;
        while (parser_.next(&f)) {
            if (!handleFrame(f, err)) return false;
        }
        return true;
    }

    bool handleFrame(Frame& f, std::string* err)
    {
        std::vector<uint8_t> plain;
        if (f.encrypted) {
            {
                std::lock_guard<std::mutex> lk(tlsMu_);
                if (!tls_.decrypt(f.payload.data(), f.payload.size(), &plain, err))
                    return false;
            }
            if (plain.empty()) return true;   // incomplete record; wait for the next frame
        } else {
            plain = f.payload;
        }

        if (f.first) {
            asmActive_  = true;
            asmChannel_ = f.channel;
            asmBuf_.clear();
        }
        if (asmActive_) {
            asmBuf_.insert(asmBuf_.end(), plain.begin(), plain.end());
            if (!f.last) return true;
            plain.swap(asmBuf_);
            asmActive_ = false;
        }
        if (plain.size() < 2) return true;

        const uint16_t msgId = uint16_t((uint16_t(plain[0]) << 8) | plain[1]);
        std::vector<uint8_t> body(plain.begin() + 2, plain.end());
        return handleMessage(f.channel, msgId, body, err);
    }

    bool handleMessage(Channel ch, uint16_t msgId, const std::vector<uint8_t>& body,
                       std::string* err)
    {
        // TLS handshake records: may arrive at any time
        if (ch == Channel::Control && msgId == kMsgSslHandshake) {
            {
                std::lock_guard<std::mutex> lk(tlsMu_);
                tls_.ingest(body.data(), body.size());
            }
            return driveHandshake(err);
        }

        // Version response
        if (ch == Channel::Control && msgId == kMsgVersionResponse &&
            stage_ == Stage::Version) {
            if (body.size() < 6) { if (err) *err = "VERSION_RESPONSE is too short"; return false; }
            const uint16_t statusBE = uint16_t((uint16_t(body[4]) << 8) | body[5]);
            const uint16_t statusLE = uint16_t((uint16_t(body[5]) << 8) | body[4]);
            if (statusBE != 0 && statusLE != 0) {
                if (err) *err = "The phone rejected the AAP version (MISMATCH)";
                return false;
            }
            stage_ = Stage::Tls;
            return true;
        }

        if (ch == Channel::Control) {
            if (msgId == kMsgServiceDiscoveryReq) {
                return sendMessage(Channel::Control, kMsgServiceDiscoveryRsp,
                                   pbServiceDiscovery(opt_), true, true, err);
            }
            if (msgId == kMsgPingRequest) {
                // as in aasdk: PING still uses PLAIN
                return sendMessage(Channel::Control, kMsgPingResponse, body, true, false, err);
            }
            if (msgId == kMsgAudioFocusRequest) {
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kAudioFocusGain);
                return sendMessage(Channel::Control, kMsgAudioFocusResponse, rsp, true, true, err);
            }
            if (msgId == kMsgShutdownRequest) {
                sendMessage(Channel::Control, kMsgShutdownResponse, {}, true, true, err);
                stop_.store(true);
                return true;
            }
            if (msgId == kMsgChannelOpenRequest) {
                const uint32_t id = uint32_t(pb::Reader::getVarint(body.data(), body.size(), 2,
                                                                   uint32_t(ch)));
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(Channel(id), kMsgChannelOpenResponse, rsp, true, true, err);
            }
            return true;   // ignore other control messages
        }

        if (ch == Channel::Video) {
            if (msgId == kMsgChannelOpenRequest) {
                videoOpen_ = true;
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgChannelOpenResponse, rsp, true, true, err);
            }
            if (msgId == kMsgAvSetupRequest) {
                return sendAvSetupRsp(ch, body, err);
            }
            if (msgId == kMsgAvStart) {
                markRunning();
                return true;
            }
            if (msgId == kMsgAvStop) {
                sink_.onVideoReset();
                return true;
            }
            if (msgId == kMsgVideoFocusRequest) {
                std::vector<uint8_t> ind;
                pb::putUInt(ind, 1, kVideoFocusFocused);
                pb::putBool(ind, 2, false);
                return sendMessage(ch, kMsgVideoFocusIndication, ind, false, true, err);
            }
            if (msgId == kMsgAvMediaWithTimestamp || msgId == kMsgAvMedia) {
                return onVideoPayload(ch, msgId, body, err);
            }
            return true;
        }

        if (ch == Channel::MediaAudio || ch == Channel::SpeechAudio ||
            ch == Channel::SystemAudio) {
            if (msgId == kMsgChannelOpenRequest) {
                audioOpen_ = true;
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgChannelOpenResponse, rsp, true, true, err);
            }
            if (msgId == kMsgAvSetupRequest) {
                AudioConfig ac;
                ac.codec       = AudioCodec::LPCM;
                ac.sample_rate = opt_.audio_rate;
                ac.channels    = opt_.audio_channels;
                sink_.onAudioConfig(ac);
                return sendAvSetupRsp(ch, body, err);
            }
            if (msgId == kMsgAvStart) {
                markRunning();
                return true;
            }
            if (msgId == kMsgAvStop) {
                sink_.onAudioReset();
                return true;
            }
            if (msgId == kMsgAvMediaWithTimestamp || msgId == kMsgAvMedia) {
                return onAudioPayload(ch, msgId, body, err);
            }
            return true;
        }

        if (ch == Channel::Input) {
            if (msgId == kMsgChannelOpenRequest) {
                inputOpen_ = true;
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgChannelOpenResponse, rsp, true, true, err);
            }
            if (msgId == kMsgBindingRequest) {
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgBindingResponse, rsp, true, true, err);
            }
            return true;
        }

        if (ch == Channel::Sensor) {
            if (msgId == kMsgChannelOpenRequest) {
                sensorOpen_ = true;
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgChannelOpenResponse, rsp, true, true, err);
            }
            if (msgId == kMsgSensorStartReq) {
                std::vector<uint8_t> rsp;
                pb::putUInt(rsp, 1, kStatusOk);
                return sendMessage(ch, kMsgSensorStartRsp, rsp, true, true, err);
            }
            return true;
        }

        return true;
    }

    bool sendAvSetupRsp(Channel ch, const std::vector<uint8_t>& req, std::string* err)
    {
        const uint32_t configIndex =
            uint32_t(pb::Reader::getVarint(req.data(), req.size(), 1, 0));
        std::vector<uint8_t> rsp;
        pb::putUInt(rsp, 1, kSetupStatusOk);
        pb::putUInt(rsp, 2, 2);            // max_unacked
        pb::putUInt(rsp, 3, configIndex);  // accept the config chosen by the phone
        return sendMessage(ch, kMsgAvSetupResponse, rsp, false, true, err);
    }

    bool onVideoPayload(Channel ch, uint16_t msgId, const std::vector<uint8_t>& body,
                        std::string* err)
    {
        (void)ch;
        (void)err;
        const uint8_t* data = body.data();
        size_t         len  = body.size();
        if (msgId == kMsgAvMediaWithTimestamp) {
            if (len < 8) return true;
            data += 8;
            len  -= 8;
        }
        if (len == 0) return true;

        if (!videoConfigured_) {
            std::vector<uint8_t> sps, pps;
            scanParamSets(data, len, &sps, &pps);
            std::vector<uint8_t> avcC;
            if (makeAvcC(sps, pps, &avcC)) {
                VideoConfig vc;
                vc.codec      = VideoCodec::H264;
                vc.width      = opt_.width;
                vc.height     = opt_.height;
                vc.codec_data = std::move(avcC);
                sink_.onVideoConfig(vc);
                videoConfigured_ = true;
            }
        }
        if (!sink_.onVideoFrame(data, len)) {
            // The host could not enqueue: have it wait for a key frame again. AA has no key frame request, so we can only wait for the next IDR.
        }
        if (!sendMediaAck(ch, err)) return false;
        markRunning();
        return true;
    }

    bool onAudioPayload(Channel ch, uint16_t msgId, const std::vector<uint8_t>& body,
                        std::string* err)
    {
        const uint8_t* data = body.data();
        size_t         len  = body.size();
        uint32_t       pts  = 0;
        if (msgId == kMsgAvMediaWithTimestamp) {
            if (len < 8) return true;
            pts   = uint32_t(readU64BE(data));
            data += 8;
            len  -= 8;
        }
        if (len == 0) return true;

        // AA PCM is little-endian; the host's LPCM path follows the AirPlay convention (big-endian),
        // so convert to big-endian here and let the host convert back to host order uniformly.
        audioSwap_.resize(len & ~size_t(1));
        for (size_t i = 0; i + 1 < len; i += 2) {
            audioSwap_[i]     = data[i + 1];
            audioSwap_[i + 1] = data[i];
        }
        sink_.onAudioFrame(audioSwap_.data(), audioSwap_.size(), pts);
        if (!sendMediaAck(ch, err)) return false;
        markRunning();
        return true;
    }

    /// We advertised max_unacked=2; without ACKs the phone stops after two frames.
    bool sendMediaAck(Channel ch, std::string* err)
    {
        std::vector<uint8_t> ack;
        pb::putUInt(ack, 1, 0);   // session
        pb::putUInt(ack, 2, 1);   // one ACK per frame
        return sendMessage(ch, kMsgAvMediaAck, ack, /*control=*/false,
                           /*encrypted=*/true, err);
    }

    void markRunning()
    {
        if (running_.exchange(true)) return;
        sink_.onState(SessionState::Running, "Mirroring");
    }

    bool writeAll(const std::vector<uint8_t>& data, std::string* err)
    {
        size_t off = 0;
        while (off < data.size()) {
            const int n = dev_->writeBulk(data.data() + off, data.size() - off, 10000, err);
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }

    bool sendMessage(Channel ch, uint16_t msgId, const std::vector<uint8_t>& body,
                     bool control, bool encrypted, std::string* err)
    {
        if (!dev_) { if (err) *err = "USB device is not ready"; return false; }

        std::vector<uint8_t> plain;
        plain.reserve(2 + body.size());
        plain.push_back(uint8_t(msgId >> 8));
        plain.push_back(uint8_t(msgId));
        plain.insert(plain.end(), body.begin(), body.end());

        // When encrypting, split into 0x4000 plaintext chunks and run each through TLS separately (aasdk convention)
        std::vector<uint8_t> out;
        bool first = true;
        size_t off = 0;
        while (off < plain.size()) {
            const size_t n = std::min(kMaxFramePayload, plain.size() - off);
            const bool last = (off + n == plain.size());

            std::vector<uint8_t> chunk(plain.begin() + std::ptrdiff_t(off),
                                       plain.begin() + std::ptrdiff_t(off + n));
            std::vector<uint8_t> payload;
            if (encrypted) {
                std::lock_guard<std::mutex> lk(tlsMu_);
                if (!tls_.encrypt(chunk, &payload, err)) return false;
            } else {
                payload = std::move(chunk);
            }

            auto frame = encodeFrame(ch, encrypted, control, payload.data(), payload.size(),
                                     first, last, uint32_t(plain.size()));
            out.insert(out.end(), frame.begin(), frame.end());
            first = false;
            off += n;
        }

        std::lock_guard<std::mutex> lk(sendMu_);
        return writeAll(out, err);
    }

    AndroidAutoOptions     opt_;
    IProjectionSink&       sink_;
    std::string            deviceId_;

    std::unique_ptr<IAaUsbDevice> dev_;
    FrameParser                   parser_;
    Tls12Client                   tls_;
    std::mutex                    tlsMu_;
    std::mutex                    sendMu_;

    std::thread       thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    Stage             stage_ = Stage::Aoap;

    bool                 asmActive_ = false;
    Channel              asmChannel_ = Channel::None;
    std::vector<uint8_t> asmBuf_;

    bool videoOpen_ = false, audioOpen_ = false, inputOpen_ = false, sensorOpen_ = false;
    bool videoConfigured_ = false;
    uint32_t touchIndex_ = 0;
    std::vector<uint8_t> audioSwap_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------
AndroidAutoBackend::AndroidAutoBackend(AndroidAutoOptions opt) : opt_(std::move(opt)) {}

std::vector<ProjectionTarget> AndroidAutoBackend::probe(
    const core::PlatformServices&, std::string* err)
{
    std::vector<ProjectionTarget> out;
    if (!opt_.usb.probeDevices) {
        if (err) *err = "This build has no Android Auto USB transport";
        return out;
    }
    std::string e;
    for (const auto& id : opt_.usb.probeDevices(&e)) {
        ProjectionTarget t;
        t.protocol  = Protocol::AndroidAuto;
        t.id        = id;
        t.name      = id;
        t.transport = "usb";
        out.push_back(std::move(t));
    }
    if (out.empty() && err) *err = e;
    return out;
}

std::unique_ptr<IProjectionSession> AndroidAutoBackend::connect(
    const ProjectionTarget& target, const core::PlatformServices& services,
    IProjectionSink& sink, std::string* err)
{
    if (target.protocol != Protocol::AndroidAuto) {
        if (err) *err = "Not an Android Auto target";
        return nullptr;
    }
    if (!opt_.usb.open) {
        if (err) *err = "This build has no Android Auto USB transport";
        return nullptr;
    }
    return std::make_unique<AaSession>(opt_, services, sink, target.id);
}

}  // namespace core::projection::androidauto
