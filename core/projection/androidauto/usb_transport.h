/**
 * @file   usb_transport.h
 * @brief  Android Auto USB transport port (AOAP control requests + bulk endpoints)
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/usb_transport.h - AA USB transport port
// ============================================================================
// The AOAP handshake (GET_PROTOCOL / SEND_STRING / START) uses control transfers; after re-enumeration there are two
// bulk endpoints. The only platform difference is "how to obtain a libusb handle"; the core session drives protocol ordering,
// so core depends only on this interface; platform/posix/aa_usb_libusb.cpp implements it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::projection::androidauto {

class IAaUsbDevice {
public:
    virtual ~IAaUsbDevice() = default;

    /// Stable identifier (serial; the platform makes one up if it cannot be read).
    virtual std::string id() const = 0;

    /// Vendor control request (AOAP 51/52/53). requestType uses libusb's bit definitions:
    /// 0xC0 = IN|VENDOR, 0x40 = OUT|VENDOR.
    virtual bool controlTransfer(uint8_t requestType, uint8_t request, uint16_t value,
                                 uint16_t index, uint8_t* data, uint16_t len,
                                 int timeoutMs, std::string* err) = 0;

    /// After START the phone re-enumerates as 0x18D1:0x2D00/0x2D01. Wait for it to appear and open it.
    virtual std::unique_ptr<IAaUsbDevice> waitForAccessory(int timeoutMs, std::string* err) = 0;

    /// Claim interface 0 and pick bulk IN/OUT from the descriptors.
    virtual bool openBulk(std::string* err) = 0;

    /// Returns bytes written on success; -1 on failure.
    virtual int writeBulk(const uint8_t* data, size_t len, int timeoutMs, std::string* err) = 0;

    /// Returns bytes read; 0 = timeout (not an error); -1 = error.
    virtual int readBulk(uint8_t* data, size_t cap, int timeoutMs, std::string* err) = 0;
};

/// USB capabilities injected by the composition root.
struct AaUsbPorts {
    /// Enumerate currently available phones (id list for display; empty err means none found).
    std::function<std::vector<std::string>(std::string* err)> probeDevices;

    /// Open a device by id (ready to send AOAP control requests).
    std::function<std::unique_ptr<IAaUsbDevice>(const std::string& id, std::string* err)> open;
};

}  // namespace core::projection::androidauto
