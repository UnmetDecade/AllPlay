/**
 * @file   backend_androidauto.cpp
 * @brief  Android Auto 有线后端：AOAP → AAP → TLS → 服务发现 → 音视频/输入
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
// 小工具
// ---------------------------------------------------------------------------
uint64_t readU64BE(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// ---------------------------------------------------------------------------
// protobuf 消息构造
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
    // 只报一种分辨率：config_index 就固定是 0，触摸坐标与视频像素一一对应。
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
    // 支持的按键（Android KeyEvent 值，与 AASDK ButtonCodeEnum 一致）：
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
    pb::putBool(r, 6, true);   // 左舵
    return r;
}

// ---------------------------------------------------------------------------
// H.264 参数集：AA 不发 avcC，只有带内 SPS/PPS，解码器需要自己合成
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
    out->push_back(0xFF);                 // 4 字节长度前缀 + 1 个 SPS
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
// 会话
// ---------------------------------------------------------------------------
class AaSession final : public IProjectionSession {
public:
    AaSession(AndroidAutoOptions opt, const core::PlatformServices& services,
              IProjectionSink& sink, std::string deviceId)
        : opt_(std::move(opt)), sink_(sink), deviceId_(std::move(deviceId))
    {
        (void)services;   // AA 有线不需要平台服务（蓝牙 / 网络）
    }

    ~AaSession() override { stop(); }

    bool start(std::string* err) override
    {
        if (thread_.joinable()) {
            if (err) *err = "会话已经启动过";
            return false;
        }
        stop_.store(false);
        sink_.onState(SessionState::Connecting, "AOAP 配件模式");
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
            if (err) *err = "Android Auto 暂只支持触摸输入";
            return false;
        }
        if (!running_.load()) {
            if (err) *err = "会话尚未进入 Running";
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
        // AA 的时间戳单位是微秒；用单调时钟，手机只拿它算手势时序。
        const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        pb::putUInt(body, 1, uint64_t(nowUs));   // timestamp
        pb::putMessage(body, 3, touch);          // touch_event

        if (action == kTouchRelease) ++touchIndex_;

        return sendMessage(Channel::Input, kMsgInputEvent, body,
                           /*control=*/false, /*encrypted=*/true, err);
    }

    void requestKeyFrame() override {}   // AA 没有关键帧请求消息

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

        // ---- ① AOAP：版本查询 → 六条字符串 → START → 等重枚举 ----
        if (!openAoap(&err)) return fail(err);

        // ---- ② AAP 版本 ----
        sink_.onState(SessionState::Negotiating, "AAP 版本协商");
        if (!sendVersionRequest(&err)) return fail(err);
        stage_ = Stage::Version;
        const auto versionDeadline = std::chrono::steady_clock::now() + 10s;
        while (!stop_.load() && stage_ == Stage::Version) {
            if (!readOnce(500, &err)) return fail(err);
            if (std::chrono::steady_clock::now() > versionDeadline)
                return fail("等待 VERSION_RESPONSE 超时");
        }
        if (stop_.load()) return;

        // ---- ③ TLS（记录装在 0x0003 里）----
        if (!tls_.init(&err)) return fail("TLS 初始化失败：" + err);
        if (!driveHandshake(&err)) return fail(err);

        // ---- ④ 主循环 ----
        while (!stop_.load()) {
            if (!readOnce(500, &err)) return fail(err);
        }
    }

    bool openAoap(std::string* err)
    {
        if (!opt_.usb.open) { if (err) *err = "这个构建没有 Android Auto USB 传输层"; return false; }
        dev_ = opt_.usb.open(deviceId_, err);
        if (!dev_) return false;

        uint8_t ver[2] = {0, 0};
        if (!dev_->controlTransfer(0xC0, 51, 0, 0, ver, 2, 1000, err)) {
            if (err) *err = "AOAP GET_PROTOCOL 失败：" + *err;
            return false;
        }
        const uint16_t protocol = uint16_t(ver[0] | (ver[1] << 8));
        if (protocol < 1) { if (err) *err = "手机不支持 AOAP"; return false; }

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
        // control channel / SPECIFIC / PLAIN：major=1 minor=1
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
            // aasdk 在 AUTH_COMPLETE 上仍标 PLAIN；先照做（风险见 docs）。
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
        if (!dev_) { if (err) *err = "USB 设备未就绪"; return false; }
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
            if (plain.empty()) return true;   // 记录还不完整，等下一帧
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
        // TLS 握手记录：任何时候都可能来
        if (ch == Channel::Control && msgId == kMsgSslHandshake) {
            {
                std::lock_guard<std::mutex> lk(tlsMu_);
                tls_.ingest(body.data(), body.size());
            }
            return driveHandshake(err);
        }

        // 版本应答
        if (ch == Channel::Control && msgId == kMsgVersionResponse &&
            stage_ == Stage::Version) {
            if (body.size() < 6) { if (err) *err = "VERSION_RESPONSE 太短"; return false; }
            const uint16_t statusBE = uint16_t((uint16_t(body[4]) << 8) | body[5]);
            const uint16_t statusLE = uint16_t((uint16_t(body[5]) << 8) | body[4]);
            if (statusBE != 0 && statusLE != 0) {
                if (err) *err = "手机拒绝 AAP 版本（MISMATCH）";
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
                // 照 aasdk：PING 仍用 PLAIN
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
            return true;   // 其余控制消息忽略
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
        pb::putUInt(rsp, 3, configIndex);  // 接受手机选的那份配置
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
            // 宿主没能入队：让它重新等关键帧。AA 没有关键帧请求，只能等下一帧 IDR。
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

        // AA 的 PCM 是小端；宿主的 LPCM 通道按 AirPlay 约定（大端）处理，
        // 这里先转成大端，宿主那边统一换回主机序。
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

    /// 我们声明了 max_unacked=2；不回 ACK 手机发两帧就停。
    bool sendMediaAck(Channel ch, std::string* err)
    {
        std::vector<uint8_t> ack;
        pb::putUInt(ack, 1, 0);   // session
        pb::putUInt(ack, 2, 1);   // 一帧一确认
        return sendMessage(ch, kMsgAvMediaAck, ack, /*control=*/false,
                           /*encrypted=*/true, err);
    }

    void markRunning()
    {
        if (running_.exchange(true)) return;
        sink_.onState(SessionState::Running, "投屏中");
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
        if (!dev_) { if (err) *err = "USB 设备未就绪"; return false; }

        std::vector<uint8_t> plain;
        plain.reserve(2 + body.size());
        plain.push_back(uint8_t(msgId >> 8));
        plain.push_back(uint8_t(msgId));
        plain.insert(plain.end(), body.begin(), body.end());

        // 加密时按 0x4000 明文分片，每片单独过 TLS（aasdk 的约定）
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
        if (err) *err = "这个构建没有 Android Auto USB 传输层";
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
        if (err) *err = "不是 Android Auto 目标";
        return nullptr;
    }
    if (!opt_.usb.open) {
        if (err) *err = "这个构建没有 Android Auto USB 传输层";
        return nullptr;
    }
    return std::make_unique<AaSession>(opt_, services, sink, target.id);
}

}  // namespace core::projection::androidauto
