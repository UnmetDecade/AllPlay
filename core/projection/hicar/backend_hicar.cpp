/**
 * @file   backend_hicar.cpp
 * @brief  HiCar placeholder backend
 */

#include "projection/hicar/backend_hicar.h"

namespace core::projection::hicar {

std::vector<ProjectionTarget> HiCarBackend::probe(const core::PlatformServices&,
                                                  std::string* err)
{
    if (err) *err = "HiCar backend is not implemented yet";
    return {};
}

std::unique_ptr<IProjectionSession> HiCarBackend::connect(
    const ProjectionTarget&, const core::PlatformServices&,
    IProjectionSink&, std::string* err)
{
    if (err) *err = "HiCar backend is not implemented yet";
    return nullptr;
}

}  // namespace core::projection::hicar
