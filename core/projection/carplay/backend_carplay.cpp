/**
 * @file   backend_carplay.cpp
 * @brief  CarPlay 后端：有线（usbmux + iAP2 + Receiver）与无线（bt_trigger + Receiver）
 */

#include "projection/carplay/backend_carplay.h"

#include "airplay/server.h"
#include "bt/bt_trigger.h"
#include "iap2/wired_link.h"
#include "mfi/mfi_identity.h"
#include "usb/wired_control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace core::projection::carplay {

namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// 媒体桥：把 CarPlay 协议栈的 IVideoOutput / IAudioOutput 转成协议中立的 Sink
// ---------------------------------------------------------------------------
class VideoBridge final : public core::IVideoOutput {
public:
    explicit VideoBridge(IProjectionSink& sink) : sink_(sink) {}

    void configure(VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& codecData) override
    {
        VideoConfig cfg;
        cfg.codec      = codec;
        cfg.width      = width;
        cfg.height     = height;
        cfg.codec_data = codecData;
        sink_.onVideoConfig(cfg);
    }

    bool feedAnnexB(const uint8_t* data, size_t len) override
    {
        return sink_.onVideoFrame(data, len);
    }

    void reset() override { sink_.onVideoReset(); }
    void flush() override { sink_.onVideoFlush(); }

private:
    IProjectionSink& sink_;
};

class AudioBridge final : public core::IAudioOutput {
public:
    explicit AudioBridge(IProjectionSink& sink) : sink_(sink) {}

    void configure(AudioCodec codec, int sampleRate, int channels) override
    {
        AudioConfig cfg;
        cfg.codec       = codec;
        cfg.sample_rate = sampleRate;
        cfg.channels    = channels;
        sink_.onAudioConfig(cfg);
    }

    void feed(const uint8_t* payload, size_t len, uint32_t sample) override
    {
        sink_.onAudioFrame(payload, len, sample);
    }

    void reset() override { sink_.onAudioReset(); }

private:
    IProjectionSink& sink_;
};

/// 状态去重：同一个状态不重复上报；回调在锁外发，避免 Sink 反手调 stop() 时死锁。
class StateGate {
public:
    explicit StateGate(IProjectionSink& sink) : sink_(sink) {}

    void set(SessionState st, const std::string& detail = {})
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (state_ == st) return;
            state_ = st;
        }
        sink_.onState(st, detail);
    }

private:
    IProjectionSink& sink_;
    std::mutex       mu_;
    SessionState     state_ = SessionState::Idle;
};

/// 可被 stop_ 打断的 sleep。
bool waitFor(const std::atomic<bool>& stop, std::chrono::milliseconds ms)
{
    const auto end = std::chrono::steady_clock::now() + ms;
    while (!stop.load()) {
        if (std::chrono::steady_clock::now() >= end) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 有线会话：USB → lockdown/TLS → iAP2 → MFi → 0x4300/0x4301 → AirPlay
// ---------------------------------------------------------------------------
class WiredSession final : public IProjectionSession {
public:
    WiredSession(const CarPlayBackendOptions& opt, core::PlatformServices services,
                 IProjectionSink& sink)
        : opt_(opt), services_(std::move(services)),
          video_(sink), audio_(sink), state_(sink)
    {
    }

    ~WiredSession() override { stop(); }

    bool start(std::string* err) override
    {
        if (thread_.joinable()) {
            if (err) *err = "会话已经启动过";
            return false;
        }
        stop_.store(false);
        state_.set(SessionState::Connecting, "USB 链路准备中");
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        // 先停媒体面，再拆协议栈：Receiver 的线程还持有 video_/audio_ 指针。
        if (rx_) { rx_->stop(); rx_.reset(); }
        link_.reset();
        control_.reset();
        pipe_.reset();
        mux_.reset();
        state_.set(SessionState::Idle);
    }

    bool sendInput(const InputEvent& ev, std::string* err) override
    {
        if (!rx_ || !rx_->sessionActive()) {
            if (err) *err = "会话未到 Running";
            return false;
        }
        if (ev.kind != InputKind::Touch) {
            if (err) *err = "CarPlay 后端暂不支持该输入类型";
            return false;
        }
        std::vector<core::airplay::TouchPoint> pts(1);
        pts[0].x    = ev.x;
        pts[0].y    = ev.y;
        pts[0].down = ev.phase == TouchPhase::Down || ev.phase == TouchPhase::Move;
        return rx_->sendTouch(pts);
    }

    void requestKeyFrame() override
    {
        if (rx_) rx_->requestKeyFrameNow();
    }

    Capabilities capabilities() const override
    {
        Capabilities c;
        c.touch = true;
        c.max_width  = opt_.width;
        c.max_height = opt_.height;
        c.video_codecs = {VideoCodec::H264, VideoCodec::HEVC};
        c.audio_codecs = {AudioCodec::AAC_LC, AudioCodec::OPUS, AudioCodec::LPCM};
        return c;
    }

private:
    void fail(const std::string& why)
    {
        state_.set(SessionState::Failed, why);
    }

    void run()
    {
        WiredPhoneInfo info;
        std::string err;

        // ---- ① 传输层：Linux 自开 pipe；macOS 借系统 usbmuxd ----
        if (opt_.wired.openMux) {
            auto mux = opt_.wired.openMux(&info, &err);
            if (!mux) return fail("接上系统 usbmuxd 失败：" + err);
            control_ = std::make_unique<core::usb::WiredControl>(std::move(mux));
        } else if (opt_.wired.openPipe) {
            pipe_ = opt_.wired.openPipe(opt_.productId, opt_.switchWaitMs, &info, &err);
            if (!pipe_) return fail("打开 USB 失败：" + err);
            control_ = std::make_unique<core::usb::WiredControl>(*pipe_);
        } else {
            return fail("这个构建没有有线传输层");
        }

        // ---- ② lockdown → TLS → carkit ----
        control_->setPairStore(info.serial, opt_.stateDir);
        if (!control_->connect(&err)) return fail("控制面没跑通：" + err);

        // ---- ③ iAP2 链路 + 识别 ----
        link_ = std::make_unique<core::iap2::WiredLink>(*control_->service());
        if (!link_->negotiate(30000, &err)) return fail("iAP2 链路没协商起来：" + err);

        core::iap2::WiredConfig wcfg;
        wcfg.name = opt_.name;
        wcfg.carPlayUsbInterfaceNumber = int(info.usbMuxInterface);
        if (!link_->identify(wcfg, &err)) return fail("识别没通过：" + err);

        // ---- ④ MFi ----
        std::unique_ptr<core::mfi::CertificateMfiIdentity> ownedMfi;
        const core::mfi::IMfiIdentity* mfi = opt_.mfiIdentity;
        if (!mfi) {
            ownedMfi = core::mfi::CertificateMfiIdentity::load(opt_.mfiDir, &err);
            if (!ownedMfi) return fail("装不上 MFi 身份（" + opt_.mfiDir + "）：" + err);
            mfi = ownedMfi.get();
        }
        if (!link_->authenticate(mfi, 30000, &err)) return fail("MFi 认证没通过：" + err);

        link_->sendPowerSourceUpdate(wcfg.availableCurrentMilliAmps, /*charging=*/true);
        link_->sendSubscriptions();
        state_.set(SessionState::Negotiating, "iAP2 已识别，等 CarPlay 会话");

        // ---- ⑤ 媒体面网卡 ----
        if (!opt_.wired.prepareNcm) return fail("这个构建没有有线媒体面");
        WiredNcm ncm;
        if (!opt_.wired.prepareNcm(info, opt_.ncmLinkLocal, &ncm, &err))
            return fail("有线媒体面起不来：" + err);
        if (ncm.ipv6.empty()) return fail("NCM 网卡上一个 IPv6 链路本地都没有");

        // ---- ⑥ AirPlay 接收端 ----
        core::airplay::ReceiverConfig cfg;
        cfg.name      = opt_.name;
        cfg.ifname    = ncm.ifname;
        cfg.bindAddr  = ncm.ipv6.front();
        cfg.port      = opt_.port;
        cfg.width     = opt_.width;
        cfg.height    = opt_.height;
        cfg.fps       = opt_.fps;
        cfg.stateDir  = opt_.stateDir;
        cfg.mfiDir    = opt_.mfiDir;
        cfg.mfiIdentity = mfi;
        cfg.btMac     = opt_.controllerAddress ? opt_.controllerAddress() : std::string();
        cfg.video     = &video_;
        cfg.audio     = &audio_;
        cfg.enableAvahi = false;   // 有线不广播：地址由 0x4301 直接告诉手机

        rx_ = std::make_unique<core::airplay::Receiver>();
        if (!rx_->start(cfg, services_, &err)) {
            rx_.reset();
            return fail("AirPlay 接收端起不来：" + err);
        }

        // ---- ⑦ 等 0x4300 → 回 0x4301 → 保活 ----
        core::iap2::WiredCarPlayEndpoint ep;
        ep.ipv6Addresses.push_back(cfg.bindAddr);
        ep.airPlayPort      = rx_->port();
        ep.deviceIdentifier = rx_->info().deviceId;
        ep.publicKeyHex     = rx_->info().pkHex;
        ep.sourceVersion    = core::airplay::kSourceVersion;

        bool sent    = false;
        bool running = false;
        while (!stop_.load()) {
            std::vector<core::iap2::WiredLink::Message> msgs;
            if (!link_->pump(200, &msgs)) {
                if (running) state_.set(SessionState::Disconnected, "iAP2 链路断开");
                else         state_.set(SessionState::Failed, "iAP2 链路断开");
                return;
            }
            for (const auto& m : msgs) {
                if (m.id != 0x4300 || sent) continue;
                std::string serr;
                if (!link_->sendCarPlayStartSession(ep, &serr))
                    return fail("0x4301 发不出去：" + serr);
                sent = true;
                state_.set(SessionState::Negotiating, "已回 0x4301，等手机连 AirPlay");
            }
            const bool active = rx_->sessionActive();
            if (!running && active) {
                running = true;
                state_.set(SessionState::Running, "投屏中");
            } else if (running && !active) {
                running = false;
                state_.set(SessionState::Disconnected, "AirPlay 会话结束");
                return;
            }
        }
    }

    CarPlayBackendOptions         opt_;
    core::PlatformServices        services_;
    VideoBridge                   video_;
    AudioBridge                   audio_;
    StateGate                     state_;

    // 声明顺序 = 依赖顺序；析构/stop 时反向拆（link → control → pipe/mux）。
    std::unique_ptr<core::usb::IMux>          mux_;
    std::unique_ptr<core::IByteStream>        pipe_;
    std::unique_ptr<core::usb::WiredControl>  control_;
    std::unique_ptr<core::iap2::WiredLink>    link_;
    std::unique_ptr<core::airplay::Receiver>  rx_;

    std::thread       thread_;
    std::atomic<bool> stop_{false};
};

// ---------------------------------------------------------------------------
// 无线会话：Receiver 先起 → 蓝牙触发（0x5703/0x4301）→ 等手机连 AirPlay
// ---------------------------------------------------------------------------
class WirelessSession final : public IProjectionSession {
public:
    WirelessSession(const CarPlayBackendOptions& opt, core::PlatformServices services,
                    IProjectionSink& sink)
        : opt_(opt), services_(std::move(services)),
          video_(sink), audio_(sink), state_(sink)
    {
    }

    ~WirelessSession() override { stop(); }

    bool start(std::string* err) override
    {
        if (thread_.joinable()) {
            if (err) *err = "会话已经启动过";
            return false;
        }
        stop_.store(false);

        core::airplay::ReceiverConfig cfg;
        cfg.name        = opt_.name;
        cfg.ifname      = opt_.ifname;
        cfg.port        = opt_.port;
        cfg.width       = opt_.width;
        cfg.height      = opt_.height;
        cfg.fps         = opt_.fps;
        cfg.stateDir    = opt_.stateDir;
        cfg.mfiDir      = opt_.mfiDir;
        cfg.mfiIdentity = opt_.mfiIdentity;
        cfg.btMac       = opt_.controllerAddress ? opt_.controllerAddress() : std::string();
        cfg.video       = &video_;
        cfg.audio       = &audio_;
        cfg.enableAvahi = opt_.enableAvahi;

        rx_ = std::make_unique<core::airplay::Receiver>();
        if (!rx_->start(cfg, services_, err)) {
            rx_.reset();
            return false;
        }
        state_.set(SessionState::Connecting, "等蓝牙触发");

        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        stop_.store(true);
        // 让 runBtTrigger 的 100ms 轮询收工
        if (cancel_) cancel_->store(true);
        if (thread_.joinable()) thread_.join();
        if (rx_) { rx_->stop(); rx_.reset(); }
        cancel_.reset();
        state_.set(SessionState::Idle);
    }

    bool sendInput(const InputEvent& ev, std::string* err) override
    {
        if (!rx_ || !rx_->sessionActive()) {
            if (err) *err = "会话未到 Running";
            return false;
        }
        if (ev.kind != InputKind::Touch) {
            if (err) *err = "CarPlay 后端暂不支持该输入类型";
            return false;
        }
        std::vector<core::airplay::TouchPoint> pts(1);
        pts[0].x    = ev.x;
        pts[0].y    = ev.y;
        pts[0].down = ev.phase == TouchPhase::Down || ev.phase == TouchPhase::Move;
        return rx_->sendTouch(pts);
    }

    void requestKeyFrame() override
    {
        if (rx_) rx_->requestKeyFrameNow();
    }

    Capabilities capabilities() const override
    {
        Capabilities c;
        c.touch = true;
        c.max_width  = opt_.width;
        c.max_height = opt_.height;
        c.video_codecs = {VideoCodec::H264, VideoCodec::HEVC};
        c.audio_codecs = {AudioCodec::AAC_LC, AudioCodec::OPUS, AudioCodec::LPCM};
        return c;
    }

private:
    void run()
    {
        if (!services_.bluetooth) {
            state_.set(SessionState::Failed, "平台没有可用的 IBluetooth 实现");
            return;
        }

        auto cancel = std::make_shared<std::atomic<bool>>(false);
        cancel_ = cancel;

        core::bt::BtTriggerParams bp;
        bp.deviceName    = opt_.deviceName;
        bp.deviceAddress = opt_.deviceAddress;
        bp.serverMode    = false;
        bp.serverWaitMs  = 300000;

        auto& c = bp.iap2;
        c.name             = opt_.name;
        c.model            = "AllPlay1,1";
        c.manufacturer     = "AllPlay";
        c.serial           = rx_->info().deviceId;
        c.firmware         = "1.0";
        c.hardware         = "1.0";
        c.btMac            = opt_.controllerAddress ? opt_.controllerAddress() : std::string();
        c.ssid             = opt_.ssid.empty()
                                 ? (opt_.currentSSID ? opt_.currentSSID() : std::string())
                                 : opt_.ssid;
        c.passphrase       = opt_.passphrase;
        c.ipAddresses      = {rx_->ip()};
        c.airPlayPort      = rx_->port();
        c.deviceIdentifier = rx_->info().deviceId;
        c.publicKeyHex     = rx_->info().pkHex;
        c.mfiDir           = opt_.mfiDir;
        c.mfiIdentity      = opt_.mfiIdentity;
        c.timeoutMs        = 90000;
        c.cancel           = cancel.get();
        c.phoneConnected   = [this] { return rx_ && rx_->sessionActive(); };

        bool ok = false;
        std::string lastErr;
        for (int attempt = 1; attempt <= opt_.retry && !stop_.load(); ++attempt) {
            auto r = core::bt::runBtTrigger(services_.bluetooth, bp, nullptr);
            if (r.ok) { ok = true; break; }
            lastErr = r.error;
            if (attempt < opt_.retry) {
                if (!waitFor(stop_, std::chrono::milliseconds(opt_.retryDelayMs))) return;
            }
        }
        if (!ok) {
            if (!stop_.load())
                state_.set(SessionState::Failed, "蓝牙触发失败：" + lastErr);
            return;
        }
        state_.set(SessionState::Negotiating, "0x4301 已发，等手机连 AirPlay");

        bool running = false;
        while (!stop_.load()) {
            const bool active = rx_ && rx_->sessionActive();
            if (!running && active) {
                running = true;
                state_.set(SessionState::Running, "投屏中");
            } else if (running && !active) {
                state_.set(SessionState::Disconnected, "AirPlay 会话结束");
                return;
            }
            if (!waitFor(stop_, 200ms)) return;
        }
    }

    CarPlayBackendOptions         opt_;
    core::PlatformServices        services_;
    VideoBridge                   video_;
    AudioBridge                   audio_;
    StateGate                     state_;

    std::unique_ptr<core::airplay::Receiver> rx_;
    std::shared_ptr<std::atomic<bool>>       cancel_;
    std::thread                              thread_;
    std::atomic<bool>                        stop_{false};
};

}  // namespace

// ---------------------------------------------------------------------------
// CarPlayBackend
// ---------------------------------------------------------------------------
CarPlayBackend::CarPlayBackend(CarPlayBackendOptions opt) : opt_(std::move(opt)) {}

std::vector<ProjectionTarget> CarPlayBackend::probe(const core::PlatformServices& services,
                                                    std::string* err)
{
    std::vector<ProjectionTarget> out;

    // 有线：插着的 iPhone 优先级最高
    if (opt_.wired.probePhones) {
        std::string e;
        for (const auto& p : opt_.wired.probePhones(&e)) {
            ProjectionTarget t;
            t.protocol  = Protocol::CarPlay;
            t.id        = p.serial;
            t.name      = p.serial.empty() ? "iPhone" : p.serial;
            t.transport = "usb";
            t.handle    = std::make_shared<WiredPhoneInfo>(p);
            out.push_back(std::move(t));
        }
        if (out.empty() && err) *err = e;
    }

    // 无线：已配对的手机就是候选目标（我们主动去连它）
    if (services.bluetooth) {
        std::string e;
        for (const auto& d : services.bluetooth->pairedDevices(&e)) {
            const bool match = !opt_.deviceAddress.empty()
                                   ? d.address == opt_.deviceAddress
                                   : (opt_.deviceName.empty() ||
                                      d.name.find(opt_.deviceName) != std::string::npos);
            if (!match) continue;
            ProjectionTarget t;
            t.protocol  = Protocol::CarPlay;
            t.id        = d.address;
            t.name      = d.name;
            t.transport = "bt";
            t.handle    = std::make_shared<core::BtDevice>(d);
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::unique_ptr<IProjectionSession> CarPlayBackend::connect(const ProjectionTarget& target,
                                                            const core::PlatformServices& services,
                                                            IProjectionSink& sink,
                                                            std::string* err)
{
    if (target.protocol != Protocol::CarPlay) {
        if (err) *err = "不是 CarPlay 目标";
        return nullptr;
    }

    CarPlayBackendOptions opt = opt_;
    if (target.transport == "usb") {
        return std::make_unique<WiredSession>(opt, services, sink);
    }

    // 无线：目标里带着选中设备的地址（probe 时放进去的）
    if (auto dev = std::static_pointer_cast<core::BtDevice>(target.handle)) {
        if (!dev->address.empty()) opt.deviceAddress = dev->address;
    }
    if (target.transport != "bt") {
        if (err) *err = "不支持的传输：" + target.transport;
        return nullptr;
    }
    return std::make_unique<WirelessSession>(opt, services, sink);
}

}  // namespace core::projection::carplay
