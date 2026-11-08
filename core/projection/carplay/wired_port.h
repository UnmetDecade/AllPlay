/**
 * @file   wired_port.h
 * @brief  CarPlay 有线传输端口：core 与平台之间的唯一接缝
 * @author blt
 */

// ============================================================================
// core/projection/carplay/wired_port.h —— CarPlay 有线传输端口
// ============================================================================
// 有线链路里只有这一层是平台的：怎么把 iPhone 切进 CarPlay 配置、怎么跟系统
// usbmuxd 要隧道、怎么找到 AppleUSBNCMData 建出来的网卡。再往上（usbmux v2、
// lockdown、iAP2、AirPlay）全在 core 里。
//
// 组合根（host/main.cpp / tools）把平台实现（platform/posix/usb.h 里的
// openWiredCarPlay / openSystemUsbMuxd / prepareWiredNcm*）包成这里的 std::function
// 注入给 CarPlayBackend —— 与 PlatformServices 的工厂注入同一个思路。
#pragma once

#include "port/stream.h"
#include "usb/mux.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::projection::carplay {

/// USB 侧事实（iAP2 识别与媒体面都要用）。
struct WiredPhoneInfo {
    uint16_t    vendorId  = 0;
    uint16_t    productId = 0;
    std::string serial;                ///< iPhone 序列号（稳定；macOS 上是 usbmuxd UDID）
    uint8_t     bus = 0;
    uint8_t     address = 0;
    int         configValue = -1;
    uint8_t     usbMuxInterface = 0;   ///< ff/fe/02：iAP2 的载体
};

/// 媒体面网卡（Linux = cdc_ncm，macOS = AppleUSBNCMData）。
struct WiredNcm {
    std::string              ifname;
    std::string              mac;
    std::vector<std::string> ipv6;     ///< 第一个是本次要绑定的地址
    bool                     unmanaged = false;
    bool                     pinned = false;
};

/// 平台注入的有线能力。Linux 与 macOS 各填一半：
///   Linux：openPipe + prepareNcm（probePhones 可选，libusb 枚举）
///   macOS：openMux  + prepareNcm（按序列号找 NCM 网卡）
struct WiredPorts {
    /// 切进 CarPlay 配置并打开 USBMUX 字节流（Linux）。失败返回 nullptr 并写 err。
    std::function<std::unique_ptr<core::IByteStream>(
        unsigned productId, int switchWaitMs, WiredPhoneInfo* out, std::string* err)> openPipe;

    /// 借系统 usbmuxd 的隧道（macOS）。失败返回 nullptr 并写 err。
    std::function<std::unique_ptr<core::usb::IMux>(
        WiredPhoneInfo* out, std::string* err)> openMux;

    /// 准备媒体面网卡。pinnedLinkLocal 空 = 自动挑。
    std::function<bool(const WiredPhoneInfo& phone, const std::string& pinnedLinkLocal,
                       WiredNcm* out, std::string* err)> prepareNcm;

    /// 发现已插上的手机（可选）。为空则后端只能靠 openPipe/openMux 开链。
    std::function<std::vector<WiredPhoneInfo>(std::string* err)> probePhones;
};

}  // namespace core::projection::carplay
