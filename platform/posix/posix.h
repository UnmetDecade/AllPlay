/**
 * @file   posix.h
 * @brief  Assembly entry point for the POSIX platform adapters (Linux + macOS)
 * @author blt
 */

// ============================================================================
// platform/posix/posix.h -- assembly entry point for the POSIX platform adapters (Linux + macOS)
// ============================================================================
// core depends only on core::PlatformServices; this provides the POSIX implementation and assembles it.
// Implementation files: net_posix.cpp (TCP/network interfaces), mdns_{dns_sd,avahi}.cpp (mDNS),
// bt_{iobluetooth,bluez}.{mm,cpp} (Bluetooth). The desktop composition root (app/main.cpp, tools/*)
// calls allplay::posix::makePlatformServices() to inject them into core.
#pragma once

#include "port/platform.h"

#include <memory>
#include <string>
#include <vector>

namespace allplay::posix {

/// Singleton for network interface info (POSIX getifaddrs)
core::INetworkInfo& networkInfo();

/// Factories: each call builds a new instance
std::unique_ptr<core::ITcpListener>   makeTcpListener();
std::unique_ptr<core::IUdpSocket>     makeUdpSocket();
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser();
std::unique_ptr<core::IBluetooth>     makeBluetooth();

/// Composition-root helpers: Bluetooth controller address / current Wi-Fi SSID (empty string if unavailable).
/// Since macOS 27 the system often masks both; in same-network mode pass --ssid explicitly.
std::string controllerAddress();
std::string currentSSID();

/// Advertised Bluetooth adapter name -- this is **exactly** what appears in the phone's Bluetooth list (and what the splash page prints).
/// Linux uses BlueZ Adapter1.Alias; macOS uses IOBluetoothHostController. Empty string if unavailable.
std::string bluetoothAdapterName();

/// Assemble a complete POSIX PlatformServices (network + the three factories).
core::PlatformServices makePlatformServices();

// ---------------------------------------------------------------------------
// Wired CarPlay media plane: the network interface created by the kernel's cdc_ncm
// ---------------------------------------------------------------------------

/// State of the wired media plane once it is ready
struct WiredNcmLink {
    std::string              ifname;
    std::string              mac;      ///< AA:BB:CC:DD:EE:FF (the enx... name is derived from it)
    std::vector<std::string> ipv6;     ///< Link-local addresses; **the pinned one comes first**
    bool                     unmanaged = false;  ///< NetworkManager was detached this time
    bool                     broughtUp = false;
    bool                     pinned    = false;  ///< Address was newly pinned this time (false if one already existed)
};

/// Bring up the wired media plane: locate the interface exposed by the phone-side NCM NIC
/// -> detach NetworkManager before handing it to the kernel -> bring it up -> ensure a usable IPv6 link-local address on the link.
///
/// **Must be called after USB has switched into the CarPlay configuration** -- the kernel creates the interface only then.
/// The USB-side data (bus/addr) comes from the WiredUsbInfo filled in by openWiredCarPlay.
///
/// Two pitfalls hit on real hardware, recorded here so they do not have to be rediscovered:
///   - This interface has `addr_gen_mode=1` (NONE) -- **the kernel generates no address at all**,
///     `ip -6 addr` is empty, so there is no address in 0x4301 to report to the phone.
///   - NetworkManager treats it as a "wired connection" and keeps requesting DHCP, stuck in connecting,
///     and can flush our pinned address at any moment. Hence `managed no` first.
///
/// @param wantLinkLocal address to pin; empty string uses the default (fe80::2, per DiPlay's config.linkLocal)
/// @return on success out->ipv6 is non-empty; on failure returns false and writes err
bool prepareWiredNcm(int usbBus, int usbAddress, const std::string& wantLinkLocal,
                     WiredNcmLink* out, std::string* err);

}  // namespace allplay::posix
