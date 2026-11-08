/**
 * @file   backend_hicar.h
 * @brief  HiCar 后端（占位：接口已就位，协议未实现）
 * @author blt
 */

// ============================================================================
// core/projection/hicar/backend_hicar.h —— HiCar 占位后端
// ============================================================================
// HiCar 的接入方式还没定（手机侧只认华为自己的发现/认证流程，不能借 CarPlay 的
// 通道跑）。这个类先满足 IProjectionBackend，让宿主的菜单/注册表不用改；probe()
// 返回空、connect() 报"未实现"，等协议资料齐了再填。
#pragma once

#include "projection/backend.h"

namespace core::projection::hicar {

class HiCarBackend final : public IProjectionBackend {
public:
    HiCarBackend() = default;
    ~HiCarBackend() override = default;

    Protocol    protocol() const override { return Protocol::HiCar; }
    const char* name() const override { return "HiCar"; }

    std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                        std::string* err) override;

    std::unique_ptr<IProjectionSession> connect(const ProjectionTarget& target,
                                                const core::PlatformServices& services,
                                                IProjectionSink& sink,
                                                std::string* err) override;
};

}  // namespace core::projection::hicar
