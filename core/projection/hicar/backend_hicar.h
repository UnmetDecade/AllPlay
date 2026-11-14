/**
 * @file   backend_hicar.h
 * @brief  HiCar backend (placeholder: interface in place, protocol not implemented)
 * @author blt
 */

// ============================================================================
// core/projection/hicar/backend_hicar.h - HiCar placeholder backend
// ============================================================================
// How HiCar integrates is still undecided (the phone side only accepts Huawei's own discovery/auth flow, so it cannot ride on CarPlay's
// channel). This class satisfies IProjectionBackend for now so the host menu/registry need no changes; probe()
// returns empty and connect() reports "not implemented" until the protocol docs are available.
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
