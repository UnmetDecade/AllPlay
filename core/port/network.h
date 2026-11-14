/**
 * @file   network.h
 * @brief  NIC information abstraction (IP/MAC enumeration)
 * @author blt
 */

// ============================================================================
// core/port/network.h — NIC information abstraction (IP/MAC enumeration)
// ============================================================================
// Platform implementations: POSIX getifaddrs (+ioctl fallback) / Android NetworkInterface / Windows.
// The key to same-network mode is "picking the right interface": the IP used for the Bonjour advertisement and in 0x4301 must be reachable by the phone.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace core {

class INetworkInfo {
public:
    virtual ~INetworkInfo() = default;

    /// Auto-pick an interface "reachable by the phone": the first one that is up, non-loopback and has IPv4.
    /// @param ifnameOut the selected interface name (may be empty string = not found)
    /// @return the interface IPv4 (dotted decimal); empty string if not found
    virtual std::string pickInterfaceIPv4(std::string* ifnameOut) = 0;

    /// IPv4 of the given interface (empty string if absent/no address)
    virtual std::string interfaceIPv4(const std::string& ifname) = 0;

    /// MAC of the given interface (6 bytes). Returns true on success.
    virtual bool interfaceMAC(const std::string& ifname, uint8_t mac[6]) = 0;

    // ---- Wired CarPlay media path (kernel cdc_ncm) -----------------------------
    // On the wired path the phone does not join our Wi-Fi but uses NCM Ethernet over USB. That NIC
    // is **created by a kernel driver** and its name (enx…) is not fixed, so look it up by USB device.

    /// Find the NIC name created for a USB device by its bus/addr. Empty string if not found.
    /// Default returns empty — the platform has no such path (Android uses VpnService, not kernel NCM).
    virtual std::string interfaceForUsbDevice(int bus, int address)
    {
        (void)bus;
        (void)address;
        return {};
    }

    /// IPv6 link-local address of the given interface (without %zone suffix). Empty list if none.
    /// This is what wired 0x4301 reports to the phone — equivalent to config.linkLocal on the DiPlay path.
    virtual std::vector<std::string> interfaceIPv6LinkLocal(const std::string& ifname)
    {
        (void)ifname;
        return {};
    }
};

/// MAC 6 bytes → "AA:BB:CC:DD:EE:FF" (pure formatting, platform-independent)
inline std::string deviceIdFromMAC(const uint8_t mac[6])
{
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

}  // namespace core
