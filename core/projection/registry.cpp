/**
 * @file   registry.cpp
 * @brief  Protocol backend registry (host picks a backend by enum without including concrete backend headers)
 */

#include "projection/backend.h"

namespace core::projection {

void BackendRegistry::add(std::unique_ptr<IProjectionBackend> backend)
{
    if (backend) backends_.push_back(std::move(backend));
}

IProjectionBackend* BackendRegistry::find(Protocol p) const
{
    for (const auto& b : backends_) {
        if (b && b->protocol() == p) return b.get();
    }
    return nullptr;
}

}  // namespace core::projection
