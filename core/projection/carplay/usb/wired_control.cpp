/**
 * @file   wired_control.cpp
 * @brief  有线控制面的实现
 * @author blt
 */

#include "usb/wired_control.h"

#include "common/log.h"
#include "usb/lockdown.h"
#include "usb/tls_stream.h"
#include "usb/usbmux.h"

#include <chrono>
#include <thread>

namespace core::usb {
namespace {

constexpr const char* kTag = "wired";
constexpr uint16_t    kLockdownPort = 62078;
constexpr int         kConnectTimeoutMs = 10000;
constexpr int         kPairTimeoutMs = 15000;  ///< 单次 Pair 的等待；真正的等待靠重发轮询
/// 手机弹"信任此电脑"时等它多久（人得腾出手解锁手机、找到弹窗、点一下）
constexpr int         kTrustWaitSec = 180;
constexpr int         kSessionTimeoutMs = 15000;
constexpr const char* kCarkitService = "com.apple.carkit.service";

/// 发请求时用的 Label。手机只是回显它，随便取，但保持和 DiPlay 一致便于比对日志。
constexpr const char* kLabel = "allplay.wired";

/// 造 Pair 请求里的嵌套 PairRecord。
/// **注意三个证书都是 <data>，里面装的是 PEM 文本字节，不是 DER** —— 这点反直觉，
/// 曾经被想当然地写成 DER。另外这里**没有 RootPrivateKey 这一项**，私钥永远不发给手机。
Plist makePairRecord(const WiredPairIdentity& id)
{
    auto pem = [](const std::string& s) {
        return Plist::data(std::vector<uint8_t>(s.begin(), s.end()));
    };

    Plist rec = Plist::dict();
    rec.set("DeviceCertificate", pem(id.deviceCertPem));
    rec.set("HostCertificate", pem(id.hostCertPem));
    rec.set("RootCertificate", pem(id.rootCertPem));
    rec.set("HostID", Plist::string(id.hostId));
    rec.set("SystemBUID", Plist::string(id.systemBuid));
    return rec;
}

}  // namespace

WiredControl::WiredControl(IByteStream& pipe) : pipe_(&pipe) {}
WiredControl::WiredControl(std::unique_ptr<IMux> mux) : mux_(std::move(mux)) {}

WiredControl::~WiredControl() = default;  // 成员按逆序析构：先服务流，最后才是 mux

bool WiredControl::connect(std::string* err)
{
    if (!mux_) {
        if (pipe_ == nullptr) {
            if (err != nullptr) *err = "没有可用的 mux（既没给 USB 管道，也没给现成的 mux）";
            return false;
        }
        mux_ = std::make_unique<UsbMux>(*pipe_);
    }
    if (!mux_->begin(err)) return false;

    // ① 这对材料以前**配对成功过** → 直接开会话，**一个 Pair 都不发**。
    //
    // 为什么这才是"免点信任"的关键：复用同一套 HostID/证书只是必要条件。手机那边
    // "我信任这台电脑"是靠**配对成功之后的连接不再走 Pair、直接拿主机证书做 TLS**
    // 体现的。每次重发一次 Pair，它就当成一次新的配对，弹窗照旧 —— 实测过：
    // 同一套材料，12:53 发 Pair 过了，12:54 再发一次又弹。
    if (loadTrustedRecord()) {
        std::string serr;
        if (doSessionAndService(&serr)) {
            LOG_INFO("[%s] 用已存的配对记录直接开会话（没走 Pair）", kTag);
            return true;
        }
        LOG_WARN("[%s] 不走 Pair 直接开会话没成（%s）—— 退回完整配对", kTag, serr.c_str());
        resetSessionStreams();
        identity_ = WiredPairIdentity{};   // 退回时让 doPair 从头把材料准备一遍
    }

    // ② 完整配对（手机没认过这台机器时，这一步会弹"要信任此电脑吗？"）
    if (!doPair(err)) return false;
    return doSessionAndService(err);
}

bool WiredControl::loadTrustedRecord()
{
    if (pairKey_.empty()) return false;
    if (!isWiredPairTrusted(pairStateDir_, pairKey_)) return false;

    WiredPairIdentity id;
    bool               found = false;
    std::string        lerr;
    if (!loadWiredPairCerts(pairStateDir_, pairKey_, &id, &found, &lerr) || !found) return false;
    if (!loadWiredPairIds(pairStateDir_, pairKey_, &id.hostId, &id.systemBuid)) return false;

    identity_ = std::move(id);
    return true;
}

void WiredControl::resetSessionStreams()
{
    // 顺序不能反：service_ / sessionCh_ 是 TLS 流，引用着各自的 raw。
    service_.reset();
    serviceRaw_.reset();
    sessionCh_.reset();
    sessionRaw_.reset();
}

void WiredControl::setPairStore(std::string key, std::string stateDir)
{
    pairKey_      = std::move(key);
    pairStateDir_ = std::move(stateDir);
}

bool WiredControl::doPair(std::string* err)
{
    // 这两个 UUID 必须在流程开始前定死：SetValue 报的那个 BUID 和 Pair 里写的
    // 必须是同一个，否则手机会当成另一个"主机"，配了等于没配。
    //
    // **优先复用上次的**：HostID 一换，手机那边存的 pair record 就对不上了，
    // 于是又弹一次"要信任此电脑吗？"。DevicePublicKey 这会儿还没拿到（SetValue
    // 得排在 GetValue 前面），所以键用手机序列号，不是 DevicePublicKey。
    if (!pairKey_.empty()) {
        if (loadWiredPairIds(pairStateDir_, pairKey_, &identity_.hostId, &identity_.systemBuid)) {
            LOG_INFO("[%s] 复用上次的配对身份 HostID=%s（手机那边认得，不该再弹信任框）", kTag,
                     identity_.hostId.c_str());
        }
    }
    if (identity_.hostId.empty()) identity_.hostId = makeUuidUpper();
    if (identity_.systemBuid.empty()) identity_.systemBuid = makeUuidUpper();
    // 从这一刻起这两个值就是"已经报给手机的那对"，后面任何地方都不许再改。
    const std::string hostId     = identity_.hostId;
    const std::string systemBuid = identity_.systemBuid;

    pairCh_ = mux_->connectTcp(kLockdownPort, kConnectTimeoutMs, err);
    if (pairCh_ == nullptr) return false;
    LockdownClient lk(*pairCh_);

    // 1) 先自报家门。不报这一下，手机多半不给 DevicePublicKey。
    if (!lk.setValue("UntrustedHostBUID", Plist::string(identity_.systemBuid), err)) return false;

    // 2) 手机的公钥 —— 拿来给"它"签一张证书
    Plist devKey;
    bool  found = false;
    if (!lk.getValue("DevicePublicKey", &devKey, &found, err)) return false;
    if (!found) {
        if (err != nullptr) *err = "手机没给 DevicePublicKey（没解锁？没信任本机？）";
        return false;
    }
    if (devKey.type() != Plist::Type::Data) {
        if (err != nullptr) *err = "DevicePublicKey 不是 <data>";
        return false;
    }
    // 具体是什么编码交给 pair_record 判 —— 实测这台手机给的是 **PEM 文本**，不是文档说的
    // PKCS#1 DER，别在这条日志里替它下结论
    const std::vector<uint8_t>& devKeyBytes = devKey.asData();
    LOG_INFO("[%s] 拿到手机公钥：%zu 字节", kTag, devKeyBytes.size());

    // 3) WiFiAddress —— 有线用不上。顺序照抄 DiPlay，读不到不算失败。
    Plist wifi;
    bool  hasWifi = false;
    if (!lk.getValue("WiFiAddress", &wifi, &hasWifi, err)) return false;
    if (hasWifi) LOG_DEBUG("[%s] 手机的 WiFiAddress：%s", kTag, wifi.asString().c_str());

    // 4) 三张证书：**存过就用存的**（手机认的就是这一套，换了等于换台电脑，弹窗重来），
    //    没存过 / 是另一台手机的，现造一套再存下来。device 那张的公钥是上面拿到的。
    if (!pairKey_.empty()) {
        bool found = false;
        std::string lerr;
        if (!loadWiredPairIdentity(pairStateDir_, pairKey_, devKeyBytes, &identity_, &found,
                                   &lerr)) {
            if (err != nullptr) *err = lerr;   // 只有"读得出来但坏了"才走到这
            return false;
        }
        if (found) {
            // 证书是三张存下来的；**两个 UUID 以已经报给手机的那个为准** ——
            // 上文 SetValue(UntrustedHostBUID) 发出去的是 identity_.systemBuid，
            // Pair 里再写一个不一样的就是"另一个主机"，配了等于没配。
            // （正常情况下两者同源，这里只是不让它们有分叉的机会。）
            identity_.hostId     = hostId;
            identity_.systemBuid = systemBuid;
            LOG_INFO("[%s] 用回存好的证书（root/host/device 各一）", kTag);
        }
    }
    if (identity_.rootCertPem.empty()) {
        if (!makeWiredPairIdentity(devKeyBytes, identity_.hostId, identity_.systemBuid, &identity_,
                                   err)) {
            return false;
        }
        if (!pairKey_.empty()) {
            std::string serr;
            // 存不下不算失败：最多下次再弹一次信任框，别为此把整条配对打断。
            if (!saveWiredPairIdentity(pairStateDir_, pairKey_, devKeyBytes, identity_, &serr))
                LOG_WARN("[%s] 配对材料没存下（%s）—— 下次这台手机还得多点一次信任", kTag,
                         serr.c_str());
        }
    }

    // 5) Pair
    Plist req = Plist::dict();
    req.set("Label", Plist::string(kLabel));
    req.set("Request", Plist::string("Pair"));
    // ProtocolVersion 放在**外层**（DiPlay 如此，不是塞在 PairRecord 里）。
    // 万一手机嫌它，回的错误里会提到，届时第一个改的就是这一行。
    req.set("ProtocolVersion", Plist::string("2"));
    Plist opts = Plist::dict();
    opts.set("ExtendedPairingErrors", Plist::boolean(true));  // 失败时给出能读的原因
    req.set("PairingOptions", std::move(opts));
    req.set("PairRecord", makePairRecord(identity_));

    // 手机第一次见到这台"电脑"会弹一个"信任此电脑？"，等在手机上点。
    // 这段时间它每次都回 Error=PairingDialogResponsePending —— **这不是失败，是"接着等"**，
    // 把同一条 Pair 请求重发即可（UUID 与证书都不能变，否则等于换了一台电脑，弹窗重来）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTrustWaitSec);
    int  waited = 0;
    for (;;) {
        std::string perr;
        if (lk.request(req, nullptr, kPairTimeoutMs, &perr)) break;

        if (perr.find("PairingDialogResponsePending") != std::string::npos &&
            std::chrono::steady_clock::now() < deadline) {
            if (waited == 0) {
                LOG_WARN("[%s] 手机弹了「要信任此电脑吗？」—— 请解锁手机并点「信任」。"
                         "这里最多等 %d 秒（每秒重发一次请求，手机上看到的还是同一个弹窗）",
                         kTag, kTrustWaitSec);
            } else if (waited % 15 == 0) {
                LOG_INFO("[%s] 还在等确认…（已等 %d 秒）", kTag, waited);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++waited;
            continue;
        }
        if (waited > 0) {
            LOG_WARN("[%s] 等了 %d 秒没等到确认，放弃", kTag, waited);
        }
        if (err != nullptr) *err = perr;
        return false;
    }
    LOG_INFO("[%s] 配对成功（HostID=%s）", kTag, identity_.hostId.c_str());

    // 盖戳：**只有真配成功过**的记录，下一轮才敢跳过 Pair（见 pair_record.h 的说明）。
    if (!pairKey_.empty()) {
        std::string merr;
        if (markWiredPairTrusted(pairStateDir_, pairKey_, &merr))
            LOG_INFO("[%s] 已记下这台手机认了这套材料 —— 下次直接开会话，不再弹框", kTag);
        else
            LOG_WARN("[%s] 没能记下「已配对」（%s）—— 下次这台手机还会再弹一次", kTag,
                     merr.c_str());
    }

    pairCh_.reset();  // 第 1 条用完就关，别占着
    return true;
}

bool WiredControl::doSessionAndService(std::string* err)
{
    sessionRaw_ = mux_->connectTcp(kLockdownPort, kConnectTimeoutMs, err);
    if (sessionRaw_ == nullptr) return false;

    // StartSession 的请求里**没有 EnableSessionSSL** —— 要不要加密是手机在响应里说了算
    bool enableSsl = false;
    {
        LockdownClient lk(*sessionRaw_);
        Plist         req = Plist::dict();
        req.set("Label", Plist::string(kLabel));
        req.set("Request", Plist::string("StartSession"));
        req.set("HostID", Plist::string(identity_.hostId));
        req.set("SystemBUID", Plist::string(identity_.systemBuid));

        Plist resp;
        if (!lk.request(req, &resp, kSessionTimeoutMs, err)) return false;
        enableSsl = resp.boolValue("EnableSessionSSL", false);
    }

    // 原地升级：同一条连接直接开始 TLS 握手。客户端身份用 **root** 证书+私钥。
    if (enableSsl) {
        LOG_INFO("[%s] 会话要 TLS，原地升级", kTag);
        sessionCh_ = startTlsClient(*sessionRaw_, identity_.rootCertPem, identity_.rootKeyPem, err);
        if (sessionCh_ == nullptr) return false;
    } else {
        // 手机没要求加密。照它说的走 —— 硬套 TLS 只会把这条连接弄死。
        LOG_WARN("[%s] 手机没要求 SessionSSL，按明文继续（后面若失败，先怀疑这里）", kTag);
        sessionCh_ = std::move(sessionRaw_);
    }

    // 在这条（可能已加密的）会话上起 carkit 服务
    uint16_t port = 0;
    bool     svcSsl = false;
    {
        LockdownClient lk(*sessionCh_);
        if (!lk.startService(kCarkitService, &port, &svcSsl, err)) return false;
    }

    // 第 3 条：去那个端口开新连接。这条就是 iAP2 的载体。
    serviceRaw_ = mux_->connectTcp(port, kConnectTimeoutMs, err);
    if (serviceRaw_ == nullptr) return false;

    if (svcSsl) {
        service_ = startTlsClient(*serviceRaw_, identity_.rootCertPem, identity_.rootKeyPem, err);
        if (service_ == nullptr) return false;
    } else {
        service_ = std::move(serviceRaw_);
    }

    LOG_INFO("[%s] carkit 服务流到手（端口 %u，服务侧 TLS：%s），可以开始 iAP2 了", kTag,
             unsigned(port), svcSsl ? "是" : "否");
    return true;
}

}  // namespace core::usb
