/**
 * @file   wired_link.cpp
 * @brief  有线 iAP2 链路驱动的实现
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
/// 一次读的等待。既是"多久没数据就回去重发 SYN"的粒度，也是 tick 的节拍。
constexpr int kReadTimeoutMs = 100;
constexpr int kReadBufBytes  = 16 * 1024;

// 端点号
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

// 电源 / 订阅（有线 bring-up 的第二段，DiPlay 的顺序：电源 → 5 条订阅 → 等 0x4300）
constexpr uint16_t kPowerSourceUpdate          = 0xae03;
constexpr uint16_t kStartNowPlayingUpdates     = 0x5000;
constexpr uint16_t kStartRouteGuidanceUpdates  = 0x5200;
constexpr uint16_t kStartPowerUpdates          = 0xae00;
constexpr uint16_t kStartCommunicationsUpdates = 0x4157;
constexpr uint16_t kStartCallStateUpdates      = 0x4154;

constexpr uint16_t kCarPlayStartSession = 0x4301;

// 识别报文里"我们支持的消息"清单。**这是有线变体**，和无线那份有两处差别：
//   · 发得出 0xae03 PowerSourceUpdate（有线这条道上我们确实在给手机供电）
//   · 不发 0x5703 AccessoryWiFiConfiguration（有线的媒体面走 NCM，没有 WiFi 配置这一步）
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
    // 零确认：SYN 里把这四个都报 0，等于告诉对端"USB 这条道可靠，别重传、别等我 ACK"。
    // 对端的 maxRetransmissions=0 会让 LinkEngine 跳过 unacked 跟踪（见 link.cpp），
    // 所以这里报 0 是配套的，不是随手写的。
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
        // takeOutput 会把攒着的都交出来，所以这个循环实际上只转一圈
        if (!stream_.writeAll(out.data(), out.size())) {
            LOG_WARN("[%s] 链路层要发 %zu 字节，写不出去", kTag, out.size());
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
    if (n < 0) return false;               // 断开
    if (n > 0) engine_.feed(buf, size_t(n), monoMs());
    engine_.tick(monoMs());                // 重发 marker / SYN、数据重传都靠它
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
            if (err != nullptr) *err = "链路层判死：" + engine_.deadReason();
            return false;
        }
        if (!pumpOnce(kReadTimeoutMs)) {
            if (err != nullptr) *err = "协商期间传输层断开";
            return false;
        }
        // 状态变化各打一条，省得盯着一屏 DBG 找转折点
        const int st = engine_.ready() ? 3 : (engine_.dead() ? 4 : 2);
        if (st != lastState) {
            lastState = st;
            LOG_INFO("[%s] 链路态：%s", kTag,
                     st == 3 ? "NORMAL（协商完成，可以发控制消息了）"
                             : "NEGOTIATING（marker 已发，SYN 在重发）");
        }
    }

    if (engine_.ready()) return true;
    if (err != nullptr) {
        *err = "等 " + std::to_string(timeoutMs) + " ms 没协商完（对端一次都没回 SYN？）";
    }
    return false;
}

bool WiredLink::sendMediaKey(core::MediaKey key, bool down)
{
    Params p;
    p.bytes(0, hidReport(key, down));
    sendMessage(kAccessoryHidReport, p);
    LOG_INFO("[%s] 方控：%s%s", kTag, core::mediaKeyName(key), down ? "（按下）" : "（抬起）");
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
    // CSM 帧可以跨多个链路载荷，按对端报的上限切开
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

    // group 16 = USBHostTransport。**有线才有这一组**；无线那边是 17/24
    // （蓝牙 MAC + SSID）。三件套里第 3 项就是手机那侧 CarPlay 用的 USB 接口号。
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
        .u8(8, 2)      // 供电能力：USB 这条道上我们给手机供电
        .u16(9, 20)    // 手机最多能从我们这儿抽 20mA？—— 照抄 DiPlay，别改
        .group(10, g10)
        .str(12, cfg.language)
        .strList(13, {cfg.language})
        .group(16, g16)
        .group(30, g30);

    sendMessage(kIdentificationInformation, body);
    LOG_INFO("[%s] 已发 IdentificationInformation（0x1d01，USBHostTransport 接口号 %d）", kTag,
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
            if (err != nullptr) *err = "链路层判死：" + engine_.deadReason();
            return false;
        }
        std::vector<Message> msgs;
        if (!pumpMessages(kReadTimeoutMs, &msgs)) {
            if (err != nullptr) *err = "识别期间传输层断开";
            return false;
        }

        for (const Message& m : msgs) {
            if (m.id == kStartIdentification) {
                sendIdentification(cfg);
                sent = true;
            } else if (m.id == kIdentificationAccepted) {
                LOG_INFO("[%s] 手机接受了识别（0x1d02）", kTag);
                return true;
            } else if (m.id == kIdentificationRejected) {
                // 报文里列的是被拒的参数号 —— 打出来，不然只能靠猜
                std::vector<Param> ps;
                std::string        which;
                if (parseParams(m.body.data(), m.body.size(), &ps)) {
                    for (const Param& p : ps) {
                        if (!which.empty()) which += " ";
                        which += hex4(p.id);
                    }
                }
                if (err != nullptr) {
                    *err = "手机拒了识别（0x1d03），被拒的参数：" + (which.empty() ? "（没列）" : which);
                }
                return false;
            }
        }
    }

    if (err != nullptr) {
        *err = sent ? "发了 0x1d01 之后没等到 0x1d02" : "一直没收到手机的 0x1d00（StartIdentification）";
    }
    return false;
}

bool WiredLink::authenticate(const mfi::IMfiIdentity* identity, int timeoutMs, std::string* err)
{
    const uint64_t deadline = monoMs() + uint64_t(timeoutMs > 0 ? timeoutMs : 30000);
    bool sentCert = false;

    while (monoMs() < deadline) {
        if (engine_.dead()) {
            if (err != nullptr) *err = "链路层判死：" + engine_.deadReason();
            return false;
        }
        std::vector<Message> msgs;
        if (!pumpMessages(kReadTimeoutMs, &msgs)) {
            if (err != nullptr) *err = "认证期间传输层断开";
            return false;
        }

        for (const Message& m : msgs) {
            switch (m.id) {
            case kRequestAuthenticationCert: {
                // 手机要配件证书。有 identity 就发真的，没有就发空的（真机会回 0xaa04）
                Params p;
                p.bytes(0, identity != nullptr ? identity->certificate() : std::vector<uint8_t>{});
                LOG_INFO("[%s] 0xaa00 要证书，回 0xaa01（%zu 字节）", kTag,
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
                    if (err != nullptr) *err = "0xAA02 里没有挑战参数";
                    return false;
                }
                bool              ok = false;
                std::vector<uint8_t> sig;
                if (identity != nullptr) sig = identity->signDigest(ch->payload.data(), ch->payload.size(), &ok);
                if (identity == nullptr || !ok) {
                    if (err != nullptr) {
                        *err = identity == nullptr ? "没配 MFi 身份，签不出来"
                                                   : "MFi 签名失败（挑战长度不是 32？）";
                    }
                    return false;
                }
                Params p;
                p.bytes(0, sig);
                LOG_INFO("[%s] 0xaa02 挑战 %zu 字节，回 0xaa03（签名 %zu 字节）", kTag,
                         ch->payload.size(), sig.size());
                sendMessage(kAuthenticationResponse, p);
                break;
            }
            case kAuthenticationSucceeded:
                LOG_INFO("[%s] MFi 认证通过（0xaa05）", kTag);
                return true;

            case kAuthenticationFailed:
                if (err != nullptr) *err = "手机回了 0xAA04 AuthenticationFailed";
                return false;

            default:
                break;
            }
        }
    }

    if (err != nullptr) {
        *err = sentCert ? "发了 0xaa01 之后没等到 0xaa04/0xaa05"
                        : "一直没收到手机的 0xaa00（RequestAuthenticationCertificate）";
    }
    return false;
}

// ---------------------------------------------------------------------------
// 电源 + 订阅（有线 bring-up 的第二段）
// ---------------------------------------------------------------------------

void WiredLink::sendPowerSourceUpdate(uint16_t availableCurrentMilliAmps, bool charging)
{
    // 参数 0 = 可用电流（mA），参数 1 = 在不在充。
    // 其余三个（可供虹吸电流 / 保留电流 / 超出阈值）DiPlay 有线这条路都不发，照做。
    Params p;
    p.u16(0, availableCurrentMilliAmps).u8(1, charging ? 1 : 0);
    sendMessage(kPowerSourceUpdate, p);
    LOG_INFO("[%s] 已发 0xae03 PowerSourceUpdate（%u mA，charging=%d）", kTag,
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

    LOG_INFO("[%s] 已发 5 条订阅（0x5000/0x5200/0xae00/0x4157/0x4154）", kTag);
}

bool WiredLink::sendCarPlayStartSession(const WiredCarPlayEndpoint& ep, std::string* err)
{
    // 参数体检。**别省**：这几条不满足时手机多半是"收到但不理你"，
    // 现象和"链路没通"一样，回头查起来极贵。DiPlay 那边也是硬 require。
    if (ep.ipv6Addresses.empty()) {
        if (err != nullptr) *err = "0x4301 要有线组的 IPv6 地址，一个都没给";
        return false;
    }
    for (const std::string& a : ep.ipv6Addresses) {
        // 只查两条：有没有冒号（是不是 IPv6 字面量）、带没带 %zone
        // —— 带 scope 后缀的地址手机解析不了，宁可在这里就拦下来。
        if (a.find(':') == std::string::npos || a.find('%') != std::string::npos) {
            if (err != nullptr) *err = "0x4301 的地址不是 IPv6 字面量：" + a;
            return false;
        }
    }
    if (ep.airPlayPort == 0 || ep.publicKeyHex.empty() || ep.sourceVersion.empty()) {
        if (err != nullptr) *err = "0x4301 缺 airPlayPort / publicKey / sourceVersion";
        return false;
    }

    // group 0 = 有线：**每个地址一个参数**（重复 id 0），不是字符串列表。
    Params wired;
    for (const std::string& a : ep.ipv6Addresses) wired.str(0, a);

    Params body;
    body.group(0, wired);
    body.u32(2, ep.airPlayPort);
    if (!ep.deviceIdentifier.empty()) body.str(3, ep.deviceIdentifier);
    body.str(4, ep.publicKeyHex);
    body.str(5, ep.sourceVersion);

    sendMessage(kCarPlayStartSession, body);
    LOG_INFO("[%s] 已发 0x4301 CarPlayStartSession（ipv6=%s port=%u pk=%s src=%s）", kTag,
             ep.ipv6Addresses.front().c_str(), unsigned(ep.airPlayPort),
             ep.publicKeyHex.substr(0, 16).c_str(), ep.sourceVersion.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// 0x4300 CarPlayAvailability
// ---------------------------------------------------------------------------

namespace {

/// 读一个"可用性"子组：子参数 0 = u8 可用否，子参数 1 = 字符串标识（可缺）。
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

    // 参数 0 = 有线组，参数 1 = 无线组，参数 2 = 主题素材（用不上）
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
        if (!have) return std::string(label) + "=（手机没报）";
        return std::string(label) + "=" + (available ? "可用" : "不可用") +
               (id.empty() ? "" : "（标识 " + id + "）");
    };
    return side("有线", a.haveWired, a.wiredAvailable, a.wiredIdentifier) + "；" +
           side("无线", a.haveWireless, a.wirelessAvailable, a.wirelessIdentifier);
}

// ---------------------------------------------------------------------------
// 公开的泵
// ---------------------------------------------------------------------------

bool WiredLink::pump(int readTimeoutMs, std::vector<Message>* out)
{
    return pumpMessages(readTimeoutMs, out);
}

}  // namespace core::iap2
