/**
 * @file   harmony_platform.cpp
 * @brief  鸿蒙侧的组合根：把 POSIX 网络层 + 空 mDNS 组装成 core::PlatformServices
 * @author blt
 */

// ============================================================================
// harmony_platform.cpp —— core::PlatformServices 的鸿蒙装配
// ============================================================================
// core 只要三样东西：网卡信息、TCP 监听、mDNS 广播。
//   · 网卡 / TCP / UDP —— 直接复用 platform/posix/net_posix.cpp（OHOS 是 POSIX 系）；
//   · mDNS —— 不用 C++ 实现：鸿蒙的 @ohos.net.mdns 在 ArkTS 侧已经广播了
//     _airplay._tcp / _carplay._tcp，这里给一个空实现让 Receiver 能启动。
// 视频/音频输出先不装（nullptr）—— 先把 pair-setup / pair-verify / SETUP 跑通。
// ============================================================================

#include "posix/posix.h"

#include "common/log.h"
#include "port/mdns.h"

#include <memory>
#include <string>
#include <vector>

namespace {

/// 空 mDNS 广播器：发现交给 ArkTS（@ohos.net.mdns），核心层只要"不失败"。
class NoopMdnsAdvertiser : public core::IMdnsAdvertiser {
public:
    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& /*txt*/, std::string* /*err*/) override
    {
        LOG_INFO("[mdns] 广播交给 ArkTS：%s:%u", name.c_str(), port);
        return true;
    }
    void stop() override {}
};

}  // namespace

namespace allplay::posix {

/// net_posix.cpp::makePlatformServices() 引用了它；鸿蒙没有 avahi/dns_sd 后端，
/// 这里给出空实现，链接才能过（Receiver 走的是 makeHarmonyPlatformServices）。
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::make_unique<NoopMdnsAdvertiser>();
}

}  // namespace allplay::posix

namespace allplay::harmony {

core::PlatformServices makeHarmonyPlatformServices()
{
    core::PlatformServices s;
    s.network            = &allplay::posix::networkInfo();
    s.makeTcpListener    = &allplay::posix::makeTcpListener;
    s.makeUdpSocket      = &allplay::posix::makeUdpSocket;
    s.makeMdnsAdvertiser = &allplay::posix::makeMdnsAdvertiser;
    s.bluetooth          = nullptr;   // 蓝牙在 ArkTS 侧，走 iap2 那条独立通道
    return s;
}

}  // namespace allplay::harmony
