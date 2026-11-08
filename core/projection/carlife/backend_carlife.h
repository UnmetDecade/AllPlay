/**
 * @file   backend_carlife.h
 * @brief  CarLife 后端（占位：接口已就位，协议未实现）
 * @author blt
 */

// ============================================================================
// core/projection/carlife/backend_carlife.h —— CarLife 占位后端
// ============================================================================
// 与 HiCar 同理：先占住 IProjectionBackend，probe() 返回空、connect() 报未实现。
// 真实现需要百度 CarLife 的 USB/Wi-Fi 发现、认证与音视频封装，等资料齐了再做。
#pragma once

#include "projection/backend.h"

namespace core::projection::carlife {

class CarLifeBackend final : public IProjectionBackend {
public:
    CarLifeBackend() = default;
    ~CarLifeBackend() override = default;

    Protocol    protocol() const override { return Protocol::CarLife; }
    const char* name() const override { return "CarLife"; }

    std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                        std::string* err) override;

    std::unique_ptr<IProjectionSession> connect(const ProjectionTarget& target,
                                                const core::PlatformServices& services,
                                                IProjectionSink& sink,
                                                std::string* err) override;
};

}  // namespace core::projection::carlife
