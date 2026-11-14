/**
 * @file   bluetooth.h
 * @brief  Bluetooth transport abstraction (for iAP2 triggering)
 * @author blt
 */

// ============================================================================
// core/port/bluetooth.h — Bluetooth transport abstraction (for iAP2 triggering)
// ============================================================================
// Platform implementations: macOS=IOBluetooth, Linux=BlueZ (TODO), Android=BluetoothSocket.
// The iAP2 stack (core/iap2) is platform-independent and consumes core::IByteStream directly.
//
// ⚠️ macOS 27: **Bluetooth permission is required** to enumerate/query; an ssh process is silently denied by TCC.
#pragma once

#include "port/stream.h"

#include <memory>
#include <string>
#include <vector>

namespace core {

/// iAP2 SPP service UUID exposed on the iPhone (client direction: we connect to the phone)
inline const char* const kIap2SppUuid = "00000000-deca-fade-deca-deafdecacafe";

/// Accessory-side "Wireless iAP" service UUID (server direction: register SDP and wait for the phone)
inline const char* const kWirelessIapUuid = "00000000-deca-fade-deca-deafdecacaff";

struct BtDevice {
    std::string name;
    std::string address;    ///< Platform form (on macOS "AA-BB-CC-DD-EE-FF", lowercase with dashes)
    bool        connected = false;
};

class IBluetooth {
public:
    virtual ~IBluetooth() = default;

    /// Paired devices (macOS 27 requires Bluetooth permission; if the list is empty, err explains possible causes).
    virtual std::vector<BtDevice> pairedDevices(std::string* err) = 0;

    /// Look up uuid via SDP → open RFCOMM; returns nullptr and sets err on failure.
    /// The returned pointer is not owned by the caller (lifetime contract in each platform implementation).
    virtual IByteStream* connectRFCOMM(const std::string& address,
                                       const std::string& uuid,
                                       std::string* err) = 0;

    /// Server direction: register SDP and block waiting for the phone (returns nullptr on timeout).
    virtual IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) = 0;
};

}  // namespace core
