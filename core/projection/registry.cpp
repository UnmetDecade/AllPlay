/**
 * @file   registry.cpp
 * @brief  协议后端注册表（宿主按枚举挑后端，不 include 具体后端头）
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
