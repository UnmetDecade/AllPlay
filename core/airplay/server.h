/**
 * @file   server.h
 * @brief  接收端总装（平台无关；依赖注入 PlatformServices）
 * @author blt
 */

// ============================================================================
// core/airplay/server.h —— 接收端总装（平台无关；依赖注入 PlatformServices）
// ============================================================================
// 组装顺序（对应真机行为）：
//   选网卡/IP → 取 MAC 当 deviceid → 载入/生成本机身份(Ed25519+pi)
//   → 建会话(Session) → 起 RTSP 服务器（端口被占自动 +1）→ mDNS 广播 _airplay._tcp
// M3：Session 实现 pair-setup/verify、auth-setup、SETUP；pair-verify 后给 RTSP 装控制加密。
#pragma once

#include "airplay/info.h"
#include "airplay/rtsp.h"
#include "airplay/session.h"
#include "platform/device_identity.h"
#include "port/mdns.h"
#include "port/media.h"
#include "port/platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace core::airplay {

struct ReceiverConfig {
    std::string name     = "AllPlay";
    std::string ifname;                     ///< 空 = 自动挑（第一块有 IPv4 的非 lo）
    /// 显式绑定的本端地址。**空 = 通配**（无线那条路就是这样，靠 mDNS 广播 + 手机自己找）。
    /// 有线必须给：手机是从 0x4301 里拿到地址直接来连的，绑到通配等于把地址说错。
    /// 填 NCM 网卡上的链路本地（配 ifname 一起给，链路本地会自动补 %scope）。
    std::string bindAddr;
    uint16_t    port     = 7000;            ///< 被占自动 +1，最多试 20 次；0 = 内核选
    int         width    = 1280;
    int         height   = 720;
    int         fps      = 60;
    std::string stateDir;                   ///< 空 = defaultStateDir()
    std::string mfiDir   = "identity";      ///< MFi 身份目录（identity.pk8 + certificate.p7b）
    /// 外部注入的 MFi 身份（鸿蒙 HAP rawfile）。非空时优先于 mfiDir。
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;
    std::string btMac;                      ///< 本机蓝牙 MAC（/info bluetoothIDs 用）
    core::IVideoOutput* video = nullptr;    ///< 视频输出（解码+上屏；空则屏幕流不建）
    core::IAudioOutput* audio = nullptr;    ///< 音频输出（解码+播放；空则音频流不建）
    bool        enableAvahi = true;
};

class Receiver {
public:
    ~Receiver();
    Receiver() = default;
    Receiver(const Receiver&) = delete;

    bool start(const ReceiverConfig& cfg, const core::PlatformServices& services, std::string* err);
    void stop();
    bool running() const { return rtsp_ != nullptr; }

    uint16_t            port() const { return rtsp_ ? rtsp_->port() : 0; }
    const std::string&  ifname() const { return ifname_; }
    const std::string&  ip() const { return ip_; }
    const DeviceInfo&   info() const { return info_; }
    const std::string&  avahiError() const { return avahiErr_; }
    bool                sessionActive() const { return session_ && session_->sessionActive(); }

    /// 连接中（iPhone 正在配对/协商，尚未到 RECORD）。
    bool connecting() const { return session_ && session_->connecting(); }

    /// 发触摸（归一化坐标）到事件通道（M4 HID 回传）。
    bool sendTouch(const std::vector<TouchPoint>& contacts) {
        return session_ && session_->sendTouch(contacts);
    }

    /// 双通道互斥：接不接新来的连接由组合根说了算（见 RtspServer::setConnectionGate）。
    /// 必须在 start() 之后调 —— start() 之前还没有 RtspServer，设了也没地方挂。
    void setConnectionGate(std::function<bool()> g) {
        if (rtsp_) rtsp_->setConnectionGate(std::move(g));
    }

    /// 立即向 iPhone 请求关键帧（回前台 Surface 重建后调用，尽快恢复画面）。
    void requestKeyFrameNow() {
        if (session_) session_->requestKeyFrameNow();
    }

private:
    ReceiverConfig  cfg_;
    /// ★ 自己持有一份 PlatformServices（而不是存 start() 参数的地址）。
    /// Session 比 start() 活得久：它到 SETUP 才第一次用 services_->makeTcpListener
    /// 去建数据/事件通道，而调用方常传临时对象（host/main.cpp:332）。
    /// 存参数地址 = 悬垂指针，症状是 SETUP 阶段 SIGSEGV。
    core::PlatformServices services_;
    DeviceIdentity  identity_;
    DeviceInfo      info_;
    std::string     ifname_, ip_, avahiErr_;
    std::unique_ptr<Session>              session_;
    std::unique_ptr<RtspServer>           rtsp_;
    std::unique_ptr<core::IMdnsAdvertiser> bonjour_;
};

}  // namespace core::airplay
