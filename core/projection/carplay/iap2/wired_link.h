/**
 * @file   wired_link.h
 * @brief  有线 iAP2：把链路层套在 carkit 服务流上，并由主机主动发起协商
 * @author blt
 */

// ============================================================================
// core/iap2/wired_link.h —— 有线 iAP2 链路的驱动
// ============================================================================
// 有线和无线在链路层上的差别只有一条，但它是"通"和"一片死寂"的差别：
//
//   无线：手机（配件）先发 marker，本端收到后回 SYN。
//   有线：**主机先发 marker，然后不等任何回话，自己立刻发 SYN**，并且每 500ms 重发，
//         直到手机把它的 SYN 发回来。
//
// 所以 LinkEngine 只看"对端 marker"是进不了协商态的，得有人替它踩第一脚 ——
// 这个类就是那只脚：LinkConfig::initiator = true，外加一个泵（读→喂→tick→写）。
//
// 有线链路参数也照 DiPlay 的 WIRED_LINK_CONFIG 走：maxOutgoing=4、控制会话版本 2、
// 并且**声明零确认**（USB 这条道本身可靠，重传与 ACK 都不必）。
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

class Params;   // iap2/wire.h：CSM 报文体构造器

/// 有线链路参数（对比 LinkConfig 的默认值：那套是无线在用的）
LinkConfig wiredLinkConfig();

/// 有线识别（0x1D01）要报的身份。字段名沿用无线那边的叫法，值可以不一样 ——
/// 有线报的是"车机"，不是"配件"。
struct WiredConfig {
    std::string name         = "AllPlay";
    std::string model        = "AllPlay1,1";
    std::string manufacturer = "AllPlay";
    std::string serial       = "000000000000";
    std::string firmware     = "1.0";
    std::string hardware     = "1.0";
    std::string eaProtocol   = "com.allplay.receiver";
    std::string language     = "en";

    /// 手机那侧给 CarPlay 用的 USB 接口号 —— 就是描述符里 USBMUX 那个口的 bInterfaceNumber
    /// （真机实测 = 1）。**这个数不是我们挑的，是从 USB 描述符里读出来的**，
    /// 报错了手机会直接拒识别（0x1D03）。
    int carPlayUsbInterfaceNumber = 1;

    /// 0xAE03 PowerSourceUpdate 里报的"能给手机多少电流"（毫安）。照 DiPlay 的默认值。
    uint16_t availableCurrentMilliAmps = 2400;

    int timeoutMs = 30000;
};

/// 0x4301 CarPlayStartSession 里"有线那一组"要报的东西。
/// DiPlay 的对应物是 Iap2WiredCarPlayEndpoint —— 那边地址来自它自建的 VPN/NCM，
/// 这边来自**内核 cdc_ncm 挂出来的那张网卡**（见 core::INetworkInfo::interfaceForUsbDevice）。
struct WiredCarPlayEndpoint {
    /// 本端在 NCM 链路上的 IPv6 字面量，至少一个。手机拿到它就往这儿连 AirPlay。
    /// **必须是链路本地**（fe80::/10）：NCM 是点对点链路，没有路由器发前缀。
    std::vector<std::string> ipv6Addresses;
    uint16_t    airPlayPort = 7000;
    /// AirPlay 配对层公钥（Ed25519，小写 hex）—— 手机后面 pair-verify 认的就是它
    std::string publicKeyHex;
    /// 协议版本串（AirPlay 的 sourceVersion / srcvers）。**不能为空**，手机会挑版本。
    std::string sourceVersion;
    /// 可选。空则不发这个参数。
    std::string deviceIdentifier;
};

/// 0x4300 CarPlayAvailability 里我们关心的部分。**两个方向各一组**：
/// 这里收到的是手机在问"你（配件）这边有线/无线各自能不能上"。
struct CarPlayAvailability {
    bool        haveWired         = false;
    bool        wiredAvailable    = false;
    std::string wiredIdentifier;             ///< 手机那侧的有线传输标识（真机上是它的 UDID）
    bool        haveWireless      = false;
    bool        wirelessAvailable = false;
    std::string wirelessIdentifier;          ///< 手机那侧的 Wi-Fi MAC
};

/// 解析 0x4300 的报文体（CSM 头之后的参数表）。畸形时返回 false 并把能读到的留下。
bool parseCarPlayAvailability(const std::vector<uint8_t>& body, CarPlayAvailability* out);

/// 给人看的一行摘要（日志用）
std::string describeCarPlayAvailability(const CarPlayAvailability& a);

class WiredLink {
public:
    /// 一条完整的 CSM 消息
    struct Message {
        uint16_t             id = 0;
        std::vector<uint8_t> body;
    };

    /// stream 必须比本对象活得久（通常是 WiredControl::service()）
    explicit WiredLink(core::IByteStream& stream);

    /// 发 marker + SYN，进入协商。只调一次。
    void start(uint64_t nowMs);

    /// 泵一次：把待发字节写出去 → 读一段 → 喂给链路层 → 推进时间 → 再写出去。
    /// 返回 false = 传输层断了（不是"没数据"）。
    bool pumpOnce(int readTimeoutMs);

    /// 一直泵到链路进入 NORMAL。ready=false 时 err 里写的是判死原因或超时说明。
    bool negotiate(int timeoutMs, std::string* err);

    /// 识别：等手机的 0x1D00 → 回 0x1D01 → 等 0x1D02。
    /// **链路 NORMAL 之后手机第一条发的就是 0x1D00，而且它等不了太久** ——
    /// 实测干等 44 秒会被它把 USB 整个重置掉。所以识别必须紧跟着协商做。
    bool identify(const WiredConfig& cfg, std::string* err);

    /// MFi 认证：0xAA00 → 发证书 0xAA01 → 0xAA02 挑战 → 发签名 0xAA03 → 0xAA05（通过）。
    /// identity 为 null 就发空证书 —— 真机必失败，只用于离线对拍。
    bool authenticate(const mfi::IMfiIdentity* identity, int timeoutMs, std::string* err);

    /// 0xAE03 PowerSourceUpdate：告诉手机"我这边能给你多少电、在不在充"。
    /// 有线这条道上我们确实在给手机供电，所以这一步是有实义的，不是走流程。
    void sendPowerSourceUpdate(uint16_t availableCurrentMilliAmps, bool charging);

    /// DiPlay 有线 bring-up 里紧随电源之后的那 5 条订阅：
    /// 0x5000 NowPlaying / 0x5200 RouteGuidance / 0xAE00 Power /
    /// 0x4157 Communications / 0x4154 CallState。
    /// **订阅的是"手机告诉车机"的那半边** —— 不发这 5 条，手机不会推任何状态过来。
    void sendSubscriptions();

    /// 0x4301 CarPlayStartSession（有线组）。发了这一条，手机才开始往
    /// ipv6Addresses:airPlayPort 上连 AirPlay。参数不合格返回 false 并写 err。
    bool sendCarPlayStartSession(const WiredCarPlayEndpoint& ep, std::string* err);

    /// 泵一次，把这一轮攒到的完整 CSM 消息依次交出来。false = 传输层断开。
    bool pump(int readTimeoutMs, std::vector<Message>* out);

    /// 方控：发一次按键（编码见 iap2/hid.h）。**必须和 pump 在同一个线程调用**——
    /// 链路层的 seq/ack 是单线程状态。返回值只表示"进了发送队列"。
    bool sendMediaKey(core::MediaKey key, bool down);

    /// 方控：把队列里的按键一次发完，返回发出的条数。同上，只在 pump 线程里用。
    int pumpMediaKeys(core::MediaKeyQueue& q);

    LinkEngine&       engine() { return engine_; }
    const LinkEngine& engine() const { return engine_; }

private:
    void flush();
    /// 泵一次（内部），同上但供各步骤复用。
    bool pumpMessages(int readTimeoutMs, std::vector<Message>* out);
    /// 发一条 CSM 消息（按对端可承载大小分片进链路）
    void sendMessage(uint16_t msgId, const Params& params);
    void sendIdentification(const WiredConfig& cfg);

    core::IByteStream& stream_;
    LinkEngine         engine_;
    /// **跨 identify/authenticate 保留**：手机的 0xAA00 完全可能在上一步的泵里就到了，
    /// 每步各建一个 framer 会把半条消息丢掉（现象：认证永远等不到 0xAA00）。
    CsmFramer          framer_;
};

/// 单调毫秒（链路层的时间都由调用方给，这里给个统一的来源）
uint64_t monoMs();

}  // namespace core::iap2
