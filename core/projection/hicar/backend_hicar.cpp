/**
 * @file   backend_hicar.cpp
 * @brief  HiCar 占位后端
 */

#include "projection/hicar/backend_hicar.h"

namespace core::projection::hicar {

std::vector<ProjectionTarget> HiCarBackend::probe(const core::PlatformServices&,
                                                  std::string* err)
{
    if (err) *err = "HiCar 后端还没实现";
    return {};
}

std::unique_ptr<IProjectionSession> HiCarBackend::connect(
    const ProjectionTarget&, const core::PlatformServices&,
    IProjectionSink&, std::string* err)
{
    if (err) *err = "HiCar 后端还没实现";
    return nullptr;
}

}  // namespace core::projection::hicar
