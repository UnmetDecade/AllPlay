#include "iap2/wireless_client.h"

#include "common/log.h"
#include "iap2/hid.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace core::iap2 {

namespace {

// 端点号（参照 iAP2 协议的端点编号）
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

// 订阅（StartXUpdates）
constexpr uint16_t kStartNowPlayingUpdates     = 0x5000;
constexpr uint16_t kStartRouteGuidanceUpdates  = 0x5200;
constexpr uint16_t kStartPowerUpdates          = 0xae00;
constexpr uint16_t kStartCommunicationsUpdates = 0x4157;
constexpr uint16_t kStartCallStateUpdates      = 0x4154;

// 识别报文的"我们支持的消息"清单（无线变体）
const std::vector<uint16_t> kMessagesSent = {
    0xaa01, 0xaa03, 0x5000, 0x5002, 0x5200, 0x5203, 0xae00, 0xae02,
    0x4157, 0x4159, 0x4154, 0x4156, 0x4301,
    /* 无线追加 */ 0x5703,
    /* 注意：无线版不发 0xae03 PowerSourceUpdate（协议要求过滤） */
};
const std::vector<uint16_t> kMessagesReceived = {
    0xaa00, 0xaa02, 0xaa04, 0xaa05, 0xea00, 0xea01, 0x5001, 0x5201, 0x5202,
    0xae01, 0x4158, 0x4155, 0x4300,
    /* 无线追加 */ 0x4e0d, 0x4e0e, 0x5702,
};

std::vector<uint8_t> macBytes(const std::string& mac)
{
    // 严格 "AA:BB:CC:DD:EE:FF"（识别报文 group 17 的字节）
    std::vector<uint8_t> out;
    if (mac.size() != 17) return out;
    for (int i = 0; i < 6; ++i) {
        char b[3] = {mac[i * 3], mac[i * 3 + 1], 0};
        out.push_back(uint8_t(strtoul(b, nullptr, 16)));
    }
    return out;
}

// 0x4301 发出之后 iAP2 链路要保持多久（实际上由手机断开链路来提前结束）。
constexpr uint64_t kSessionKeepAliveMs = 12ULL * 60 * 60 * 1000;
/// 0x4301 发出后多久没看到手机拨号就催一次。3 秒：正常机型在 1 秒内就连上了
/// （iPhone 16 实测 ~200ms），等 10 秒纯粹是让用户对着黑屏干等。
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

    // 按对端可承载的大小分片进链路（协议上 CSM 帧可以跨多个链路载荷）
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

    // 参数 6/7 是"我能发的 / 我能收的"消息号清单。有的机型（SE2）见到清单里
    // 有不认识的号会整条拒，所以按 idListMode_ 逐级收窄。
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
        // 必须升序、无重复：iOS 18 不校验，iOS 13 会校验，乱了就整项拒（实测 SE2/iOS 13）
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
    log("iAP2 已发识别信息（0x1D01）");

    // 首次发送把字节打出来：被拒时唯一能对照的东西
    if (identRetries_ == 0 && idListMode_ == IdList::Full) {
        const auto enc = body.encode();
        std::string hx;
        char b[4];
        for (size_t i = 0; i < enc.size() && i < 200; ++i) {
            std::snprintf(b, sizeof b, "%02x", enc[i]);
            hx += b;
        }
        log("iAP2 识别报文 " + std::to_string(enc.size()) + " 字节：" + hx);
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

    log("iAP2 已发 5 条订阅");
}

void WirelessTrigger::pumpMediaKeys()
{
    if (!cfg_.keyQueue) return;
    // 会话没起来之前按下的键没有意义（CarPlay 还没接管媒体），直接丢掉，
    // 免得一开场就把几秒前松开的键补发给手机。
    if (!link_.ready()) {
        cfg_.keyQueue->clear();
        return;
    }
    core::MediaKeyEvent ev;
    while (cfg_.keyQueue->pop(&ev)) {
        Params p;
        p.bytes(0, hidReport(ev.key, ev.down));
        sendMessage(kAccessoryHidReport, p);
        log(std::string("方控：") + core::mediaKeyName(ev.key) +
            (ev.down ? "（按下）" : "（抬起）"));
    }
}

void WirelessTrigger::sendWifiConfiguration()
{
    Params cfg;
    cfg.str(1, cfg_.ssid);
    cfg.str(2, cfg_.passphrase);   // 始终发（协议要求始终发；空串对 NONE 合法）
    cfg.u8(3, uint8_t(cfg_.securityType)).u8(4, uint8_t(cfg_.channel));
    sendMessage(kAccessoryWifiConfiguration, cfg);
    log(transportSeen_ ? "iAP2 已发 0x5703（transport 后）" : "iAP2 已发 0x5703（Wi-Fi 配置）");
}

void WirelessTrigger::sendStartSession()
{
    // group 1 = wireless
    Params w;
    w.str(0, cfg_.ssid);
    w.str(1, cfg_.passphrase);   // 始终发（协议同款）
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
    log("iAP2 已发 0x4301 CarPlayStartSession（airPlayPort=" +
        std::to_string(cfg_.airPlayPort) + "）——等手机连 AirPlay");
}

void WirelessTrigger::handleMessage(uint16_t msgId, const std::vector<uint8_t>& body)
{
    std::vector<Param> params;
    parseParams(body.data(), body.size(), &params);

    switch (msgId) {
    case kStartIdentification: {
        // 请求体里带的是手机要求/支持的识别参数集合，被拒时是唯一线索
        std::string hx;
        char b[4];
        for (size_t i = 0; i < body.size() && i < 160; ++i) {
            std::snprintf(b, sizeof b, "%02x", body[i]);
            hx += b;
        }
        log("iAP2 rx=0x1d00 StartIdentification（" + std::to_string(body.size()) + " 字节：" + hx + "）");
        sendIdentification();
        break;
    }

    case kIdentificationAccepted:
        log("iAP2 rx=0x1d02 识别被接受");
        setStage(TriggerStage::Identified);
        break;

    case kIdentificationRejected: {
        // 0x1D03 的参数体是被拒的参数 id 列表：手机明确告诉你"这几项我不收"。
        // 不看这个就只会在 0x4301 之后一条一条盲试。
        std::string ids;
        bool listRejected = false;
        for (const auto& p : params) {
            if (!ids.empty()) ids += ",";
            ids += std::to_string(p.id);
            if (p.id == 6 || p.id == 7) listRejected = true;
        }
        log("iAP2 rx=0x1d03 识别被拒（被拒参数 id：" + (ids.empty() ? "无" : ids) + "）");

        // 参数 6/7（消息清单）被拒就收窄再试：别的机型收、SE2 这类不收，
        // 说明是清单里有它不认识的号。收窄不影响 CarPlay 本身要用的那几条。
        if (listRejected && identRetries_ < 2) {
            ++identRetries_;
            idListMode_ = identRetries_ == 1 ? IdList::Minimal : IdList::Omit;
            log(std::string("iAP2 收窄消息清单后重发识别：") +
                (idListMode_ == IdList::Minimal ? "只报 CarPlay 必需的" : "不再报清单"));
            error_.clear();
            sendIdentification();
            break;
        }

        error_ = "识别被手机拒绝（参数 id " + ids + "）";
        break;
    }

    case kRequestAuthenticationCert: {
        log("iAP2 rx=0xaa00 请求证书");
        Params p;
        p.bytes(0, mfi_ ? mfi_->certificate() : std::vector<uint8_t>{});
        sendMessage(kAuthenticationCertificate, p);
        break;
    }

    case kRequestChallengeResponse: {
        const Param* ch = findParam(params, 0);
        if (!ch) { error_ = "0xAA02 缺挑战参数"; return; }
        log("iAP2 rx=0xaa02 挑战 " + std::to_string(ch->payload.size()) + " 字节");
        bool ok = false;
        auto sig = mfi_ ? mfi_->signDigest(ch->payload.data(), ch->payload.size(), &ok)
                        : std::vector<uint8_t>{};
        if (!ok) { error_ = "MFi 签名失败（挑战长度不是 32？）"; return; }
        Params p;
        p.bytes(0, sig);
        sendMessage(kAuthenticationResponse, p);
        break;
    }

    case kAuthenticationSucceeded:
        log("iAP2 rx=0xaa05 认证通过");
        setStage(TriggerStage::Authenticated);
        sendSubscriptions();
        setStage(TriggerStage::Subscribed);
        break;

    case kAuthenticationFailed:
        error_ = "iPhone 发了 0xAA04 AuthenticationFailed";
        break;

    case kRequestAccessoryWifiConfig: {
        log("iAP2 rx=0x5702 请求 Wi-Fi 配置");
        const int limit = transportSeen_ ? 2 : 5;
        const int used  = transportSeen_ ? postTransportSends_ : wifiConfigSends_;
        if (used >= limit) { log("  （已达 0x5703 发送上限，忽略）"); break; }
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
        log("iAP2 rx=0x4e0e transport 通知（补发一次 0x5703）");
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
        log(std::string("iAP2 rx=0x") + id + "（未处理）body=" + hex);
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

    // 身份的来源：外部注入（鸿蒙 rawfile / 将来的硬件芯片）> mfiDir > 空证书
    ownedMfi_.reset();
    mfi_ = cfg_.mfiIdentity;
    if (mfi_) {
        log("MFi 身份来自外部注入（" + std::to_string(mfi_->certificate().size()) + " 字节证书）");
    } else if (!cfg_.mfiDir.empty()) {
        std::string err;
        ownedMfi_ = mfi::CertificateMfiIdentity::load(cfg_.mfiDir, &err);
        if (!ownedMfi_) {
            error_ = err;
            log(std::string("MFi 身份加载失败：") + err);
            return false;
        }
        mfi_ = ownedMfi_.get();
    } else {
        log("未配置 MFi 身份：0xAA01 会发空证书（真机必失败，仅测试用）");
    }

    const uint64_t triggerDeadline = nowMs() + uint64_t(cfg_.timeoutMs);
    uint64_t deadline = triggerDeadline;
    link_.start(nowMs());
    log("iAP2 链路启动（等 marker…）");

    uint8_t buf[4096];
    while (nowMs() < deadline) {
        // 宿主说这一场已经不需要这条 iAP2 链路了（手机走了/CarPlay 已收场）：
        // 收工返回。0x4301 已经发过就仍报成功，让 BtTask 的重排逻辑继续。
        if (cfg_.cancel && cfg_.cancel->load()) {
            if (!startSessionSent_) error_ = "已取消";
            return startSessionSent_;
        }

        link_.tick(nowMs());
        // 方控：上层随时可以 push，这里每轮排干（链路单线程，发包只能在这个线程）
        pumpMediaKeys();

        // 0x4301 之后手机迟迟不拨号就催：iOS 把这条路由判死之后不会自己重试，
        // 用户看到的就是"一直黑屏"，只有关开一次 Wi-Fi 才会连上来。
        if (startSessionSent_ && cfg_.phoneConnected && !phoneSeen_) {
            if (cfg_.phoneConnected()) {
                phoneSeen_ = true;
                log("手机已连上 AirPlay（0x4301 之后 " +
                    std::to_string(nowMs() - startSessionAtMs_) + "ms）");
            } else if (nowMs() - startSessionAtMs_ >= kAirPlayRetryMs &&
                       phoneRetries_ < kAirPlayRetryMax) {
                ++phoneRetries_;
                // 先补一份 Wi-Fi 配置再重发会话：手机上一轮很可能没走到 0x5703 的
                // 落网动作，只重发 0x4301 它照样不动。
                sendWifiConfiguration();
                log("3 秒没等到手机连 AirPlay，重发 0x5703 + 0x4301（第 " +
                    std::to_string(phoneRetries_) + " 次）");
                sendStartSession();
            }
        }

        // 0x4301 发出后这条 iAP2 链路就是无线 CarPlay 的"心跳线"：iOS 会一直在上面
        // 发消息，配件必须继续读、继续 ACK。**停读约 30 秒，iOS 就判定配件掉线，
        // 主动 TEARDOWN 整场 CarPlay**（实测：触发窗口 90 秒一到就退出循环，两场
        // 会话分别在开始后 88s / 118s 被手机收走，TEARDOWN 都落在停读之后）。
        // 所以过了触发窗口不能返回 —— 改成长期保持，直到链路真的断（read 返回
        // 负值 / 链路判死 / 写失败），那才是"手机不在了"。
        if (startSessionSent_ && deadline == triggerDeadline)
            deadline = nowMs() + kSessionKeepAliveMs;

        std::vector<uint8_t> out;
        if (link_.takeOutput(&out)) {
            if (!stream_->writeAll(out.data(), out.size())) {
                error_ = "RFCOMM 写失败";
                return startSessionSent_;
            }
        }

        if (link_.dead()) {
            error_ = "链路判死：" + link_.deadReason();
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
            error_ = "RFCOMM 断开";
            return startSessionSent_;
        }
        if (n > 0) link_.feed(buf, size_t(n), nowMs());
    }

    if (!startSessionSent_)
        error_ = "触发超时（" + std::to_string(cfg_.timeoutMs) + "ms 内未到 0x4301）";
    return startSessionSent_;
}

}  // namespace core::iap2
