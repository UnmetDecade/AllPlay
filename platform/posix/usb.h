/**
 * @file   usb.h
 * @brief  有线（USB）CarPlay 传输层：把 iPhone 切进 CarPlay 配置并交出 USBMUX 管道
 * @author blt
 */

// ============================================================================
// platform/posix/usb.h —— 有线（USB）CarPlay 传输层
// ============================================================================
// 有线链路里只有这一层是"平台"的：怎么把 iPhone 从普通配置切进 CarPlay 配置、
// 怎么把 USBMUX 口的 bulk 端点包成 core::IByteStream。再往上（usbmux v2、lockdown、
// iAP2）全在 core 里，与平台无关。
//
// **媒体面不在这里。** 切进 CarPlay 配置后，内核的 cdc_ncm 会自己绑上 CDC-NCM 功能、
// 建出一块真网卡（实测：`cdc_ncm 1-3:6.3 usb0: register 'cdc_ncm' ...`），AirPlay
// 直接 bind 到那块网卡的 IPv6 链路本地地址即可 —— 不必自己写 NTB16/以太网/IPv6。
// macOS 同理，只是建网卡的是系统的 AppleUSBNCMData（见本文件末尾的 Darwin 一段）。
// 本文件只碰 USBMUX 那一个接口，其余接口保持原样让内核去认领。
#pragma once

#include "port/stream.h"
#include "usb/mux.h"

#if defined(__APPLE__)
#include "posix/posix.h"   // WiredNcmLink
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace allplay::posix {

/// 打开参数
struct WiredUsbOptions {
    uint16_t vendorId  = 0x05ac;   ///< Apple
    int      productId = -1;       ///< -1 = 不限（苹果键盘/集线器会自动排除）
    int      switchWaitMs = 10000; ///< 切配置后等设备回来的上限
};

/// 打开成功后回填的 USB 侧事实（iAP2 识别与媒体面都要用）
struct WiredUsbInfo {
    uint16_t    vendorId = 0;
    uint16_t    productId = 0;
    std::string serial;                ///< iPhone 序列号（稳定，用来认设备）
    uint8_t     bus = 0;
    uint8_t     address = 0;
    int         configValue = -1;      ///< 选中的 CarPlay 配置值（型号之间不同）
    uint8_t     usbMuxInterface = 0;   ///< ff/fe/02：iAP2 的载体
    uint8_t     bulkIn = 0;            ///< USBMUX 手机→主机
    uint8_t     bulkOut = 0;           ///< USBMUX 主机→手机
    uint8_t     ncmControlInterface = 0;  ///< 02/0d：cdc_ncm 绑这个，网卡就挂在它名下
    uint8_t     ncmDataInterface = 0;     ///< 0a/00/01：NCM 数据口
};

/// 把 iPhone 切进 CarPlay 配置并打开 USBMUX 管道。
/// 失败返回 nullptr，err 里是**给人看的**原因（含"谁占着设备"的处置办法）。
/// 成功返回的流归调用方所有；对象析构时会停掉读线程并释放 USB 句柄。
std::unique_ptr<core::IByteStream> openWiredCarPlay(const WiredUsbOptions& opt,
                                                    WiredUsbInfo* info,
                                                    std::string* err);

#if defined(__APPLE__)
// ---------------------------------------------------------------------------
// macOS：不碰 USB，借系统 usbmuxd + AppleUSBNCMData
// ---------------------------------------------------------------------------
// macOS 上这条路**没有** openWiredCarPlay：系统本来就有 usbmuxd 在管 USB，而且
// 不允许用户态抢接口，所以控制面改为跟 daemon 要隧道（见 usbmuxd_mac.cpp 文件头）。

/// 用系统 usbmuxd（/var/run/usbmuxd）当控制面的 mux。成功时回填 info（serial 是
/// usbmuxd 的带横杠 UDID；usbMuxInterface 从 IORegistry 读，读不到按 1）。
/// 前提：这台 Mac 已经在 Finder 里被 iPhone 信任过。
std::unique_ptr<core::usb::IMux> openSystemUsbMuxd(WiredUsbInfo* info, std::string* err);

/// macOS 的媒体面：按序列号在 IORegistry 里找 AppleUSBNCMData 挂出来的 enX，
/// 取它的 IPv6 链路本地地址。不切配置、不钉地址 —— 都是 Apple 自己做的。
bool prepareWiredNcmDarwin(const std::string& phoneSerial, WiredNcmLink* out, std::string* err);
#endif

}  // namespace allplay::posix
