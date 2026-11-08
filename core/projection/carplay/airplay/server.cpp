#include "airplay/server.h"

#include "common/log.h"
#include "port/network.h"

#include <cstdio>
#include <cstring>

namespace core::airplay {

Receiver::~Receiver() { stop(); }

bool Receiver::start(const ReceiverConfig& cfg, const core::PlatformServices& services,
                     std::string* err)
{
    cfg_ = cfg;

    if (!services.network || !services.makeTcpListener || !services.makeMdnsAdvertiser) {
        if (err) *err = "PlatformServices 未装配完整（network/tcpListener/mdnsAdvertiser）";
        return false;
    }

    // ---- 1) 网卡 / IP ----
    // 有线和无线在这一步分叉得很干脆：
    //   无线 —— 不知道手机从哪张网卡过来，挑一块有 IPv4 的，再靠 mDNS 告诉手机；
    //   有线 —— 手机是从 0x4301 里读地址的，**地址是我们先定好的**（NCM 网卡上的
    //           链路本地，见 allplay::posix::prepareWiredNcm），而且那张网卡上没有
    //           IPv4。所以这里不能拿"有没有 IPv4"当成功判据。
    if (cfg_.bindAddr.empty()) {
        if (cfg_.ifname.empty()) {
            ip_ = services.network->pickInterfaceIPv4(&ifname_);
        } else {
            ifname_ = cfg_.ifname;
            ip_ = services.network->interfaceIPv4(ifname_);
        }
        if (ip_.empty()) {
            if (err) *err = "找不到可用网卡（用 --ifname 显式指定）";
            return false;
        }
    } else {
        ifname_ = cfg_.ifname;
        ip_     = cfg_.bindAddr;
    }
    // ---- 2) 本机身份（Ed25519 + pi）----
    const std::string stateDir = cfg_.stateDir.empty() ? defaultStateDir() : cfg_.stateDir;
    if (!identity_.loadOrCreate(stateDir, err)) return false;

    // ---- 3) deviceid：持久化稳定值（Android 的 Wi-Fi MAC 随机化、热点接口 MAC 也不同，
    //      不能当身份；统一用持久化合成值，跨网络/重启稳定）----
    bool devCreated = false;
    const std::string deviceId = loadOrCreateSyntheticDeviceId(stateDir, &devCreated);
    if (devCreated) LOG_INFO("deviceid 用持久化合成值：%s（本次新生成）", deviceId.c_str());

    // ---- 4) /info 与 TXT 用的装配 ----
    info_.name     = cfg_.name;
    info_.deviceId = deviceId;
    info_.btMac    = cfg_.btMac;
    info_.pi       = identity_.pi;
    info_.pkHex    = identity_.pkHex;
    info_.width    = cfg_.width;
    info_.height   = cfg_.height;
    info_.fps      = cfg_.fps;

    // ---- 5) 会话（pair-setup/verify、auth-setup、SETUP）----
    SessionConfig sc;
    sc.identity = &identity_;
    sc.info     = &info_;
    sc.stateDir = stateDir;
    sc.mfiDir   = cfg_.mfiDir;
    sc.mfiIdentity = cfg_.mfiIdentity;
    // 拷一份自己拿着，再把地址给 Session —— 见 server.h 里 services_ 的注释。
    // 存 &services（参数地址）在 start() 内是好的，出函数就悬垂。
    services_   = services;
    sc.services = &services_;   // NOLINT：指向本对象的成员，Receiver 析构前一直有效
    sc.video    = cfg_.video;
    sc.audio    = cfg_.audio;
    sc.width    = cfg_.width;
    sc.height   = cfg_.height;
    session_ = std::make_unique<Session>(sc);

    // ---- 6) RTSP 服务器（端口被占自动往后退）----
    // 绑定地址：链路本地要补 %网卡（不补的话平台层 bind 会 EINVAL）—— 这个补法放在
    // 这里而不是让调用方拼，是因为调用方手里只有"地址 + 网卡名"两个独立的东西。
    std::string bindAddr = cfg_.bindAddr;
    if (!bindAddr.empty() && bindAddr.find('%') == std::string::npos &&
        !ifname_.empty() && bindAddr.rfind("fe80:") == 0) {
        bindAddr += "%" + ifname_;
    }

    std::unique_ptr<core::ITcpListener> listener;
    std::string rerr;
    if (cfg_.port == 0) {
        auto l = services.makeTcpListener();
        if (l && l->start(bindAddr, 0, &rerr)) listener = std::move(l);
    } else {
        for (uint16_t p = cfg_.port; p < cfg_.port + 20; ++p) {
            auto l = services.makeTcpListener();
            if (l && l->start(bindAddr, p, &rerr)) {
                listener = std::move(l);
                if (p != cfg_.port) LOG_WARN("端口 %u 被占，改用 %u", cfg_.port, p);
                break;
            }
        }
    }
    if (!listener) {
        if (err) *err = rerr.empty() ? "RTSP 监听失败" : rerr;
        session_.reset();
        return false;
    }
    rtsp_.reset(new RtspServer);
    rtsp_->start(std::move(listener), [this](const RtspRequest& r) { return session_->handle(r); });
    rtsp_->setAfterResponse([this] {
        // pair-verify M4 已明文写出，此刻启用控制通道加密
        if (session_->controlKeysReady()) {
            rtsp_->setCipher(std::make_shared<ControlCipher>(session_->takeControlKeys()));
            LOG_INFO("控制通道加密已启用");
        }
    });
    rtsp_->setOnDisconnect([this] {
        // 手机断开/退出：复位会话状态（UI 回到启动页，不再黑屏）
        if (session_) session_->markDisconnected();
        LOG_INFO("控制连接断开，会话状态已复位");
    });

    // ---- 7) Bonjour 广播（失败不致命）----
    if (cfg_.enableAvahi) {
        // Bonjour TXT 的 features 用 "0x{低32位}[,0x{高32位}]"（参照协议的 features TXT 格式）
        const uint64_t f = info_.features;
        const uint32_t lo = uint32_t(f & 0xFFFFFFFFULL);
        const uint32_t hi = uint32_t(f >> 32);
        char feat[48];
        if (hi == 0) std::snprintf(feat, sizeof feat, "0x%x", lo);
        else         std::snprintf(feat, sizeof feat, "0x%x,0x%x", lo, hi);
        std::vector<std::string> txt = {
            "deviceid=" + deviceId,
            std::string("features=") + feat,
            "flags=0x4",
            "model=AllPlay1,1",
            "srcvers=" + std::string(kSourceVersion),
            std::string("protovers=") + kProtoVersion,
            "pi=" + identity_.pi,
            "pk=" + identity_.pkHex,
        };
        bonjour_ = services.makeMdnsAdvertiser();
        if (!bonjour_->start(cfg_.name, rtsp_->port(), txt, &avahiErr_)) {
            LOG_WARN("Bonjour 广播不可用：%s（手机在网络上将看不到本机）", avahiErr_.c_str());
            bonjour_.reset();
        }
    }

    LOG_INFO("接收端就绪：ifname=%s ip=%s deviceid=%s port=%u",
             ifname_.c_str(), ip_.c_str(), deviceId.c_str(), rtsp_->port());
    return true;
}

void Receiver::stop()
{
    if (bonjour_) { bonjour_->stop(); bonjour_.reset(); }
    if (rtsp_)    { rtsp_->stop();    rtsp_.reset(); }
}

}  // namespace core::airplay
