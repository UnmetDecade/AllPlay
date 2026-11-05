/**
 * @file   harmony_platform.h
 * @brief  鸿蒙侧组合根（POSIX 网络层 + 空 mDNS）的装配入口
 * @author blt
 */
#pragma once

#include "port/platform.h"

namespace allplay::harmony {

/// 组装一份鸿蒙用的 PlatformServices：network/TCP/UDP 走 platform/posix，
/// mDNS 是空实现（广播由 ArkTS 的 @ohos.net.mdns 负责）。
core::PlatformServices makeHarmonyPlatformServices();

}  // namespace allplay::harmony
