/**
 * @file   wired_port.h
 * @brief  CarPlay wired transport port: the sole seam between core and the platform
 * @author blt
 */

// ============================================================================
// core/projection/carplay/wired_port.h - CarPlay wired transport port
// ============================================================================
// In the wired path only this layer is platform-specific: how to switch the iPhone into CarPlay configuration, how to request a
// tunnel from the system usbmuxd, and how to find the NIC created by AppleUSBNCMData. Everything above (usbmux v2,
// lockdown, iAP2, AirPlay) lives in core.
//
// The composition root (host/main.cpp / tools) wraps the platform implementation (openWiredCarPlay / openSystemUsbMuxd /
// prepareWiredNcm* in platform/posix/usb.h) into the std::function slots here
// and injects it into CarPlayBackend -- the same idea as PlatformServices factory injection.
#pragma once

#include "port/stream.h"
#include "usb/mux.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::projection::carplay {

/// USB-side facts (needed for iAP2 identification and the media path).
struct WiredPhoneInfo {
    uint16_t    vendorId  = 0;
    uint16_t    productId = 0;
    std::string serial;                ///< iPhone serial (stable; on macOS it is the usbmuxd UDID)
    uint8_t     bus = 0;
    uint8_t     address = 0;
    int         configValue = -1;
    uint8_t     usbMuxInterface = 0;   ///< ff/fe/02: the iAP2 carrier
};

/// Media-path NIC (Linux = cdc_ncm, macOS = AppleUSBNCMData).
struct WiredNcm {
    std::string              ifname;
    std::string              mac;
    std::vector<std::string> ipv6;     ///< The first entry is the address to bind this time
    bool                     unmanaged = false;
    bool                     pinned = false;
};

/// Wired capabilities injected by the platform. Linux and macOS each fill half:
///   Linux: openPipe + prepareNcm (probePhones optional, libusb enumeration)
///   macOS: openMux  + prepareNcm (finds the NCM NIC by serial)
struct WiredPorts {
    /// Switch into CarPlay configuration and open the USBMUX byte stream (Linux). Returns nullptr and writes err on failure.
    std::function<std::unique_ptr<core::IByteStream>(
        unsigned productId, int switchWaitMs, WiredPhoneInfo* out, std::string* err)> openPipe;

    /// Borrow a tunnel from the system usbmuxd (macOS). Returns nullptr and writes err on failure.
    std::function<std::unique_ptr<core::usb::IMux>(
        WiredPhoneInfo* out, std::string* err)> openMux;

    /// Prepare the media-path NIC. Empty pinnedLinkLocal = pick automatically.
    std::function<bool(const WiredPhoneInfo& phone, const std::string& pinnedLinkLocal,
                       WiredNcm* out, std::string* err)> prepareNcm;

    /// Discover attached phones (optional). If empty, the backend can only open links via openPipe/openMux.
    std::function<std::vector<WiredPhoneInfo>(std::string* err)> probePhones;
};

}  // namespace core::projection::carplay
