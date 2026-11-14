/**
 * @file   hid.h
 * @brief  Steering-wheel buttons -> iAP2 HID report
 * @author blt
 */

// ============================================================================
// core/iap2/hid.h - Steering-wheel buttons -> iAP2 HID report
// ============================================================================
// Steering-wheel controls on iAP2 work as "accessory sends HID reports": head-unit button -> HID Consumer/Telephony page usage ->
// one HID report -> 0x4E0x message sent to the phone. The phone translates the usage into media control.
//
// The report format in the message is **an agreement between us and the report descriptor** (the descriptor is not declared yet; that belongs to
// the upper/driver layer). It is defined here as:
//   [reportId?] [usagePage LE u16] [usage LE u16]
// Release = usage 0 (HID convention: report the usage on press, 0 on release).
// Passing reportId 0 means the descriptor declares no report ID, so that byte is omitted from the message.
//
// Message numbers are in the constants below: **this family has not been verified on a real device yet**. This module first establishes the encoding, queue,
// and send path; when a real device is hooked up, verify with a packet capture; there is only one place to change.
#pragma once

#include "port/control.h"

#include <cstdint>
#include <vector>

namespace core::iap2 {

struct HidUsage {
    uint16_t page = 0;   ///< HID usage page (0x0C=Consumer, 0x0B=Telephony)
    uint16_t id   = 0;   ///< usage id; 0 means "no button"
};

/// Button -> HID usage. Returns {0,0} when there is no mapping (same as release).
HidUsage mediaKeyUsage(core::MediaKey key);

/// Report bytes for one button press (see the format description at the top of the file).
std::vector<uint8_t> hidReport(core::MediaKey key, bool down, uint8_t reportId = 0);

/// Message numbers for the iAP2 HID component family (unverified, see the top of the file)
constexpr uint16_t kStartHidUpdates    = 0x4e00;
constexpr uint16_t kStopHidUpdates     = 0x4e01;
constexpr uint16_t kAccessoryHidReport = 0x4e02;

/// Wrap a report as one CSM message (not fragmented; the caller splits to the peer's capacity)
std::vector<uint8_t> hidReportMessage(const std::vector<uint8_t>& report);

/// One complete "press/release" message, convenient for upper layers or self-tests
std::vector<uint8_t> hidReportMessage(core::MediaKey key, bool down, uint8_t reportId = 0);

}  // namespace core::iap2
