/**
 * @file   wireless_client.h
 * @brief  无线 CarPlay 触发状态机（蓝牙侧的 iAP2 控制流程）
 * @author blt
 */

// ============================================================================
// core/iap2/wireless_client.h —— 无线 CarPlay 触发状态机（蓝牙侧的 iAP2 控制流程）
// ============================================================================
// 流程（对照 iAP2 无线控制流程）：
//   识别 0x1D00→ 发 0x1D01 → 收 0x1D02
//   MFi  0xAA00→发 0xAA01(证书) → 0xAA02→发 0xAA03(签名) → 收 0xAA05
//   订阅 发 5 条（0x5000/0x5200/0xAE00/0x4157/0x4154）
//   等待 0x5702 → 发 0x5703（Wi-Fi 配置，最多 5 次）
//   等待 0x4300 → 发 0x4301（AirPlay 端点，成功点）
//   0x4E0E → 再发一次 0x5703（最多 2 次）
// 传输抽象成 IByteStream（真机=RFCOMM；测试=内存对）。
#pragma once

#include "iap2/link.h"
#include "iap2/wire.h"
#include "mfi/mfi_identity.h"
#include "port/stream.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::iap2 {

struct WirelessTriggerConfig {
    // 识别身份（0x1D01）
    std::string name         = "AllPlay";
    std::string model        = "AllPlay1,1";
    std::string manufacturer = "AllPlay";
    std::string serial       = "000000000000";
    std::string firmware     = "1.0";
    std::string hardware     = "1.0";
    std::string eaProtocol   = "com.allplay.receiver";
    std::string language     = "en";

    // 无线（group 17/24、0x5703、0x4301）
    std::string btMac;        ///< 本机蓝牙 MAC "AA:BB:CC:DD:EE:FF"
    std::string ssid;         ///< 同网 SSID（手机已在该网）
    std::string passphrase;   ///< securityType=0 时可空
    int         channel    = 0;
    int         securityType = 2;   ///< 0=NONE 1=WEP 2=WPA/WPA2 3=WPA3 过渡 4=WPA3
    std::vector<std::string> ipAddresses;   ///< 本机可达 IP（同网）

    // AirPlay 端点（0x4301）
    uint16_t    airPlayPort      = 7000;
    std::string deviceIdentifier;
    std::string publicKeyHex;
    std::string sourceVersion    = "950.7.1";

    // MFi
    std::string mfiDir;       ///< identity/（identity.pk8 + certificate.p7b）

    /// 外部取消旗标（可空）。0x4301 发出后 run() 会长期保持链路，直到手机断开；
    /// 宿主在 AirPlay 会话结束后置位它，好让这次触发收工、重新排下一次。
    const std::atomic<bool>* cancel = nullptr;

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

    /// 阻塞跑到 0x4301 发出（成功）或失败/超时。可重复调用（每次重开链路状态机）。
    bool run();

    TriggerStage stage() const { return stage_; }
    const std::string& error() const { return error_; }

private:
    void log(const std::string& s);
    void setStage(TriggerStage s);
    void sendMessage(uint16_t msgId, const Params& params);
    void sendIdentification();
    void sendSubscriptions();
    void sendWifiConfiguration();
    void sendStartSession();
    void handleMessage(uint16_t msgId, const std::vector<uint8_t>& body);

    IByteStream*           stream_;
    WirelessTriggerConfig cfg_;
    LinkEngine            link_;
    CsmFramer             framer_;
    std::unique_ptr<mfi::IMfiIdentity> mfi_;
    Progress              progress_;
    StageCb               stageCb_;

    TriggerStage stage_ = TriggerStage::Idle;
    std::string  error_;
    int wifiConfigSends_ = 0;
    int postTransportSends_ = 0;
    bool transportSeen_ = false;
    bool startSessionSent_ = false;   ///< 0x4301 已发出（发出后控制循环继续跑，直到断链/超时）
};

}  // namespace core::iap2
