/**
 * @file   backend_carlife.cpp
 * @brief  CarLife 占位后端
 */

#include "projection/carlife/backend_carlife.h"

namespace core::projection::carlife {

std::vector<ProjectionTarget> CarLifeBackend::probe(const core::PlatformServices&,
                                                    std::string* err)
{
    if (err) *err = "CarLife 后端还没实现";
    return {};
}

std::unique_ptr<IProjectionSession> CarLifeBackend::connect(
    const ProjectionTarget&, const core::PlatformServices&,
    IProjectionSink&, std::string* err)
{
    if (err) *err = "CarLife 后端还没实现";
    return nullptr;
}

}  // namespace core::projection::carlife
