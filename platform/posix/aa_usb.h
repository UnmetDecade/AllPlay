/**
 * @file   aa_usb.h
 * @brief  Android Auto USB transport (libusb implementation)
 * @author blt
 */

// ============================================================================
// platform/posix/aa_usb.h -- Android Auto USB transport
// ============================================================================
// The AA session in core only knows core::projection::androidauto::AaUsbPorts (discovery + open +
// control transfer + bulk). This implements it with libusb, shared between Linux and macOS:
//   - Linux needs to detach the kernel driver (the phone is often held by the MTP/USB storage driver);
//   - macOS has no kernel driver to detach, so it can be opened directly.
// The AOAP protocol sequence (51/52/53) lives in core's session; this file is not involved.
#pragma once

#include "projection/androidauto/usb_transport.h"

namespace allplay::posix {

core::projection::androidauto::AaUsbPorts makeAndroidAutoUsbPorts();

}  // namespace allplay::posix
