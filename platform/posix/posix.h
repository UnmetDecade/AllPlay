/**
 * @file   posix.h
 * @brief  POSIX 平台适配（Linux + macOS）的装配入口
 * @author blt
 */

// ============================================================================
// platform/posix/posix.h —— POSIX 平台适配（Linux + macOS）的装配入口
// ============================================================================
// core 只依赖 core::PlatformServices；这里提供 POSIX 实现并组装好。
// 各实现文件：net_posix.cpp（TCP/网卡）、mdns_{dns_sd,avahi}.cpp（mDNS）、
// bt_{iobluetooth,bluez}.{mm,cpp}（蓝牙）。桌面组合根（app/main.cpp、tools/*）
// 调用 allplay::posix::makePlatformServices() 注入 core。
#pragma once

#include "port/platform.h"

#include <memory>
#include <string>
#include <vector>

namespace allplay::posix {

/// 网卡信息单例（POSIX getifaddrs）
core::INetworkInfo& networkInfo();

/// 各工厂：每用建一个新实例
std::unique_ptr<core::ITcpListener>   makeTcpListener();
std::unique_ptr<core::IUdpSocket>     makeUdpSocket();
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser();
std::unique_ptr<core::IBluetooth>     makeBluetooth();

/// 组合根辅助：蓝牙控制器地址 / 当前 Wi-Fi SSID（拿不到返回空串）。
/// macOS 27 起两者常被系统打码，同网模式请用 --ssid 显式传。
std::string controllerAddress();
std::string currentSSID();

/// 蓝牙适配器对外显示的名字 —— 手机蓝牙列表里看到的**就是它**（启动页要印的那个）。
/// Linux 走 BlueZ Adapter1.Alias；macOS 走 IOBluetoothHostController。拿不到返回空串。
std::string bluetoothAdapterName();

/// 组装一份完整的 POSIX PlatformServices（network + 三个工厂）。
core::PlatformServices makePlatformServices();

// ---------------------------------------------------------------------------
// 有线 CarPlay 的媒体面：内核 cdc_ncm 那张网卡
// ---------------------------------------------------------------------------

/// 有线媒体面就绪后的样子
struct WiredNcmLink {
    std::string              ifname;
    std::string              mac;      ///< AA:BB:CC:DD:EE:FF（enx… 名字就是从它来的）
    std::vector<std::string> ipv6;     ///< 链路本地地址；**钉的那个排第一**
    bool                     unmanaged = false;  ///< 这次把 NetworkManager 摘开了
    bool                     broughtUp = false;
    bool                     pinned    = false;  ///< 这次是新钉的地址（之前就有则 false）
};

/// 把有线媒体面拉起来：找到手机那侧 NCM 网卡挂出来的网卡 → 交给内核前先把
/// NetworkManager 摘开 → 拉 up → 保证链路上有一个可用的 IPv6 链路本地地址。
///
/// **必须在 USB 切进 CarPlay 配置之后调** —— 网卡是那时候才被内核建出来的。
/// USB 侧要的东西（bus/addr）来自 openWiredCarPlay 回填的 WiredUsbInfo。
///
/// 两个真机上踩过的点，写在这儿免得下次再查：
///   · 这张网卡的 `addr_gen_mode=1`（NONE）—— **内核一个地址都不生成**，
///     `ip -6 addr` 是空的，0x4301 里就没地址可报给手机。
///   · NetworkManager 拿它当"有线连接"去要 DHCP，一直卡在 connecting，
///     随时可能把我们钉的地址冲掉。所以先 `managed no`。
///
/// @param wantLinkLocal 想钉的地址，空串用默认（fe80::2，照 DiPlay 的 config.linkLocal）
/// @return 成功时 out->ipv6 非空；失败返回 false 并写 err
bool prepareWiredNcm(int usbBus, int usbAddress, const std::string& wantLinkLocal,
                     WiredNcmLink* out, std::string* err);

}  // namespace allplay::posix
