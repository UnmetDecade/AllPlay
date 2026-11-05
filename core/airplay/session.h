/**
 * @file   session.h
 * @brief  AirPlay 会话状态机（M3：pair-setup/verify、auth-setup、SETUP）
 * @author blt
 */

// ============================================================================
// core/airplay/session.h —— AirPlay 会话状态机（M3：pair-setup/verify、auth-setup、SETUP）
// ============================================================================
// 每个手机连接一个 Session（本实现单手机：Receiver 持有一个）。
// 端点顺序（真机行为）：/info → /pair-setup → /pair-verify（此后控制通道加密）
// → /auth-setup → SETUP（第一批/第二批）→ RECORD（会话活跃）。
// 参照 AirPlay 协议（pair-setup / pair-verify / auth-setup / SETUP 流程）。
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
    const core::DeviceIdentity* identity = nullptr;   ///< 本机 Ed25519 身份
    const DeviceInfo*           info = nullptr;       ///< /info 用
    std::string                 stateDir;             ///< 配对存储目录
    std::string                 mfiDir;               ///< MFi 身份目录（identity/）
    /// 外部注入的 MFi 身份（鸿蒙从 HAP rawfile 读进来，磁盘上没有 identity/ 目录）。
    /// 非空时优先于 mfiDir；调用方保证它比 Session 活得久。
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;
    const core::PlatformServices* services = nullptr;   ///< 建 TCP 监听器（屏幕流 dataPort）
    core::IVideoOutput*         video = nullptr;      ///< 视频输出（解码+上屏）
    core::IAudioOutput*         audio = nullptr;      ///< 音频输出（解码+播放）
    int                         width = 1280;         ///< 协商分辨率
    int                         height = 720;
};

class Session {
public:
    explicit Session(const SessionConfig& cfg);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    RtspResponse handle(const RtspRequest& req);

    /// pair-verify M4 之后控制密钥就绪；调用方在**写完该响应后**取走并安装 cipher。
    bool        controlKeysReady() const { return controlKeysReady_; }
    ControlKeys takeControlKeys() { controlKeysReady_ = false; return controlKeys_; }

    bool sessionActive() const { return sessionActive_; }

    /// 连接中：已收到 iPhone 的 RTSP 请求（配对/验证/协商），但还没到 RECORD（会话未完全建立）。
    bool connecting() const { return connecting_ && !sessionActive_; }

    /// 发触摸（归一化坐标）到事件通道；事件通道未就绪返回 false。
    bool sendTouch(const std::vector<TouchPoint>& contacts);

    /// 立即向 iPhone 请求一个关键帧（不限频）。回前台 Surface 重建后调用，让画面尽快恢复。
    void requestKeyFrameNow();

    /// 连接断开/会话结束：复位 active/connecting 状态（UI 回到启动页）。
    void markDisconnected();

private:
    RtspResponse handleInfo();
    RtspResponse handleAuthSetup(const RtspRequest& req);
    RtspResponse handlePairSetup(const RtspRequest& req);
    RtspResponse handlePairVerify(const RtspRequest& req);
    RtspResponse handleSetup(const RtspRequest& req);
    RtspResponse handleCommand(const RtspRequest& req);

    RtspResponse tlv8Response(const std::vector<uint8_t>& body);
    RtspResponse pairErr(int state);

    void openEventChannel();   ///< 开事件通道（SETUP 第一批时调）
    void openNtp();            ///< 开 NTP 时序端口（音频 RTP 时钟同步）
    void openKeepAlive();      ///< 开 keep-alive UDP 端口（iPhone 请求 keepAliveLowPower 时）
    void requestKeyFrame();    ///< 经事件通道向 iPhone 请求关键帧（重置参考链）

    const core::DeviceIdentity* identity_ = nullptr;
    const DeviceInfo*           info_ = nullptr;
    std::string                 stateDir_;
    const mfi::IMfiIdentity* mfi_ = nullptr;                   ///< 非空即可用（注入或自有）
    std::unique_ptr<mfi::CertificateMfiIdentity> ownedMfi_;    ///< 从 mfiDir 加载时的持有者
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
    // 由处理 RTSP 的连接线程写、主线程读；双通道下还会被**另一条通道**的
    // accept 线程读（互斥闸门），所以必须是原子的，不能是裸 bool。
    std::atomic<bool> sessionActive_{false};
    std::atomic<bool> connecting_{false};   ///< 已收到过 RTSP 请求（表示 iPhone 正在连接）

    std::unique_ptr<EventChannel> eventChannel_;

    std::unique_ptr<core::IUdpSocket> ntpSock_;
    std::thread       ntpThread_;
    std::atomic<bool> stopNtp_{false};

    std::unique_ptr<core::IUdpSocket> keepAliveSock_;
    std::thread       keepAliveThread_;
    std::atomic<bool> stopKeepAlive_{false};

    std::thread       keyframeThread_;
    std::atomic<bool> stopKeyframe_{false};
    std::chrono::steady_clock::time_point lastKeyframeRequest_{};   // forceKeyFrame 限频（1/s）
};

}  // namespace core::airplay
