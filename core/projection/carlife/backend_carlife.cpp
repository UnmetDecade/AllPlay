/**
 * @file   backend_carlife.cpp
 * @brief  CarLife placeholder backend
 */

#include "projection/carlife/backend_carlife.h"

namespace core::projection::carlife {

std::vector<ProjectionTarget> CarLifeBackend::probe(const core::PlatformServices&,
                                                    std::string* err)
{
    if (err) *err = "CarLife backend is not implemented yet";
    return {};
}

std::unique_ptr<IProjectionSession> CarLifeBackend::connect(
    const ProjectionTarget&, const core::PlatformServices&,
    IProjectionSink&, std::string* err)
{
    if (err) *err = "CarLife backend is not implemented yet";
    return nullptr;
}

}  // namespace core::projection::carlife
