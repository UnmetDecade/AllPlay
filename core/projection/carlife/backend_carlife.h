/**
 * @file   backend_carlife.h
 * @brief  CarLife backend (placeholder: interface in place, protocol not implemented)
 * @author blt
 */

// ============================================================================
// core/projection/carlife/backend_carlife.h - CarLife placeholder backend
// ============================================================================
// Same as HiCar: occupy the IProjectionBackend slot first; probe() returns empty and connect() reports unimplemented.
// A real implementation needs Baidu CarLife's USB/Wi-Fi discovery, authentication and A/V encapsulation; do it once the docs are available.
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
