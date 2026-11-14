/**
 * @file   mux.h
 * @brief  Control-plane "multiplexer" abstraction: open a byte stream to a port on the phone
 * @author blt
 */

// ============================================================================
// core/usb/mux.h — mux abstraction under the control plane
// ============================================================================
// The wired control plane (lockdown → carkit) opens three connections to phone ports in total, but there are two ways "how":
//
//   Linux: grab the USBMUX bulk pipe directly and run usbmux v2 + mini TCP on it
//          → UsbMux (see usbmux.h)
//   macOS: never touches USB. The system usbmuxd is already running; just ask it for a tunnel
//          → SystemUsbMuxd (see platform/posix/usbmuxd_mac.cpp)
//
// Above that (lockdown / TLS / iAP2) everything is identical, so only "opening a connection" is abstracted out.
#pragma once

#include "port/stream.h"

#include <cstdint>
#include <memory>
#include <string>

namespace core::usb {

class IMux {
public:
    virtual ~IMux() = default;

    /// Establish the mux session (Linux: version negotiation; macOS: confirm the phone is present, fix up the device number).
    /// Must be called once before any connectTcp; repeated calls should be harmless.
    virtual bool begin(std::string* err) = 0;

    /// Open a byte stream to phone port (typical: 62078 = lockdown).
    /// Returns the connection on success (owned by the caller); nullptr on failure.
    virtual std::unique_ptr<IByteStream> connectTcp(uint16_t port, int timeoutMs,
                                                    std::string* err) = 0;
};

}  // namespace core::usb
