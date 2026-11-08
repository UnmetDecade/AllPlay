/**
 * @file   backend_carplay.h
 * @brief  CarPlay 协议后端：把无线（Receiver + bt_trigger）与有线（usbmux/iAP2）
 *         包装成 core::projection::IProjectionBackend
 * @author blt
 */

// ============================================================================
// core/projection/carplay/backend_carplay.h —— CarPlay 后端
// ============================================================================
// 这是 core/projection/backend.h 的第一个实现：
//   · probe()   —— 有线看插着的 iPhone（平台注入的 probePhones），无线看已配对手机；
//   · connect() —— 按 target.transport 选有线 / 无线会话；
//   · 会话内部沿用现有协议栈，行为与改造前一致（握手、0x4300/0x4301、iAP2、MFi）。
//
// 平台相关的东西一律通过构造参数注入（WiredPorts + controllerAddress/currentSSID），
// core 里不出现任何平台头。
#pragma once

#include "projection/backend.h"
#include "wired_port.h"

#include <functional>
#include <string>

namespace core::mfi { class IMfiIdentity; }

namespace core::projection::carplay {

struct CarPlayBackendOptions {
    // —— 媒体 / 身份（与 airplay::ReceiverConfig 同义）——
    std::string name      = "AllPlay";
    std::string ifname;                       ///< 无线绑定网卡；空 = 自动
    uint16_t    port      = 7000;
    int         width     = 1280;
    int         height    = 720;
    int         fps       = 60;
    std::string stateDir;                     ///< 空 = 默认状态目录
    std::string mfiDir    = "identity";
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;   ///< 优先于 mfiDir（不拥有）

    // —— 无线触发（与 bt::BtTriggerParams 同义）——
    std::string deviceName    = "iPhone";     ///< 名字包含匹配
    std::string deviceAddress;                ///< 非空则按地址直连
    std::string ssid;
    std::string passphrase;
    int         retry         = 10;
    int         retryDelayMs  = 3000;
    bool        enableAvahi   = true;

    // —— 有线 ——
    unsigned    productId     = 0;            ///< 0 = 不限
    int         switchWaitMs  = 10000;
    std::string ncmLinkLocal;
    WiredPorts  wired;

    // —— 平台小能力（可空）——
    std::function<std::string()> controllerAddress;   ///< 本机蓝牙 MAC（0x5703 / info）
    std::function<std::string()> currentSSID;         ///< 同网 SSID
};

class CarPlayBackend final : public IProjectionBackend {
public:
    explicit CarPlayBackend(CarPlayBackendOptions opt);
    ~CarPlayBackend() override = default;

    Protocol    protocol() const override { return Protocol::CarPlay; }
    const char* name() const override { return "CarPlay"; }

    std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                        std::string* err) override;

    std::unique_ptr<IProjectionSession> connect(const ProjectionTarget& target,
                                                const core::PlatformServices& services,
                                                IProjectionSink& sink,
                                                std::string* err) override;

private:
    CarPlayBackendOptions opt_;
};

}  // namespace core::projection::carplay
