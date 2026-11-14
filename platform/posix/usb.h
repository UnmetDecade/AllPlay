/**
 * @file   usb.h
 * @brief  Wired (USB) CarPlay transport: switch the iPhone into the CarPlay configuration and hand out a USBMUX pipe
 * @author blt
 */

// ============================================================================
// platform/posix/usb.h -- wired (USB) CarPlay transport
// ============================================================================
// In the wired link only this layer is platform-specific: how to switch the iPhone from its normal
// configuration into the CarPlay one, and how to wrap the USBMUX interface's bulk endpoints as a
// core::IByteStream. Above this (usbmux v2, lockdown, iAP2) everything lives in core and is platform-independent.
//
// **The media plane is not here.** After switching into the CarPlay configuration, the kernel's cdc_ncm binds the CDC-NCM function on its own,
// creating a real network interface (observed: `cdc_ncm 1-3:6.3 usb0: register 'cdc_ncm' ...`), and AirPlay
// can simply bind to that interface's IPv6 link-local address -- no need to implement NTB16/Ethernet/IPv6 ourselves.
// Same on macOS, except the system's AppleUSBNCMData creates the interface (see the Darwin section at the end of this file).
// This file touches only the USBMUX interface; all others are left untouched for the kernel to claim.
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

/// Open options
struct WiredUsbOptions {
    uint16_t vendorId  = 0x05ac;   ///< Apple
    int      productId = -1;       ///< -1 = any (Apple keyboards/hubs are excluded automatically)
    int      switchWaitMs = 10000; ///< Max wait for the device to come back after switching configuration
};

/// USB-side facts filled in on successful open (needed by iAP2 identification and the media plane)
struct WiredUsbInfo {
    uint16_t    vendorId = 0;
    uint16_t    productId = 0;
    std::string serial;                ///< iPhone serial number (stable, used to identify the device)
    uint8_t     bus = 0;
    uint8_t     address = 0;
    int         configValue = -1;      ///< Selected CarPlay configuration value (differs between models)
    uint8_t     usbMuxInterface = 0;   ///< ff/fe/02: carrier of iAP2
    uint8_t     bulkIn = 0;            ///< USBMUX phone -> host
    uint8_t     bulkOut = 0;           ///< USBMUX host -> phone
    uint8_t     ncmControlInterface = 0;  ///< 02/0d: cdc_ncm binds this one, and the interface is created under it
    uint8_t     ncmDataInterface = 0;     ///< 0a/00/01: NCM data interface
};

/// Switch the iPhone into the CarPlay configuration and open the USBMUX pipe.
/// Returns nullptr on failure; err holds a **human-readable** reason (including what to do about whoever is holding the device).
/// The returned stream is owned by the caller; destroying it stops the reader thread and releases the USB handle.
std::unique_ptr<core::IByteStream> openWiredCarPlay(const WiredUsbOptions& opt,
                                                    WiredUsbInfo* info,
                                                    std::string* err);

#if defined(__APPLE__)
// ---------------------------------------------------------------------------
// macOS: no direct USB access; uses the system usbmuxd + AppleUSBNCMData
// ---------------------------------------------------------------------------
// On macOS this path has **no** openWiredCarPlay: the system already runs usbmuxd managing USB and
// does not let user space seize interfaces, so the control plane instead asks the daemon for a tunnel (see the header of usbmuxd_mac.cpp).

/// Use the system usbmuxd (/var/run/usbmuxd) as the control-plane mux. On success fills in info (serial is
/// usbmuxd's hyphenated UDID; usbMuxInterface is read from IORegistry, falling back to 1).
/// Prerequisite: this Mac has already been trusted by the iPhone in Finder.
std::unique_ptr<core::usb::IMux> openSystemUsbMuxd(WiredUsbInfo* info, std::string* err);

/// macOS media plane: find the enX exposed by AppleUSBNCMData in IORegistry by serial number,
/// and take its IPv6 link-local address. No configuration switching or address pinning -- Apple does all that itself.
bool prepareWiredNcmDarwin(const std::string& phoneSerial, WiredNcmLink* out, std::string* err);
#endif

}  // namespace allplay::posix
