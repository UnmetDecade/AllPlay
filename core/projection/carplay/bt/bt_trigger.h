/**
 * @file   bt_trigger.h
 * @brief  Bluetooth trigger orchestration: select device -> connect RFCOMM -> run iAP2
 * @author blt
 */

// ============================================================================
// core/bt/bt_trigger.h - Bluetooth trigger orchestration: select device -> connect RFCOMM -> run iAP2
// ============================================================================
#pragma once

#include "port/bluetooth.h"
#include "iap2/wireless_client.h"

#include <functional>
#include <string>

namespace core::bt {

struct BtTriggerParams {
    std::string deviceName;      ///< Name filter (substring, e.g. "iPhone")
    std::string deviceAddress;   ///< If non-empty, connect directly by address (skips name matching)
    bool        serverMode = true;   ///< true = register SDP and wait for the phone (common approach);
                                     ///< false = actively connect to the phone's iAP2 service (alternative)
    int         serverWaitMs = 180000;  ///< How long the server waits for the phone to connect
    iap2::WirelessTriggerConfig iap2;   ///< Identification/wireless/endpoint/MFi config (filled in by the caller)
};

struct BtTriggerResult {
    bool        ok = false;
    std::string error;
    std::string deviceName;
    std::string deviceAddress;
};

/// Select device -> SDP/RFCOMM -> WirelessTrigger::run() (success once 0x4301 is sent).
/// RFCOMM stays open afterwards (the transport owns the stream until process exit while waiting for the phone to connect AirPlay).
BtTriggerResult runBtTrigger(core::IBluetooth* transport, const BtTriggerParams& params,
                             const std::function<void(const std::string&)>& progress);

}  // namespace core::bt
