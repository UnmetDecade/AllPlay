/**
 * @file   info.h
 * @brief  Binary plist construction for the /info response
 * @author blt
 */

// ============================================================================
// core/airplay/info.h - Binary plist construction for the /info response
// ============================================================================
// /info is the first thing the phone asks for after connecting to RTSP: it reads our capabilities (features/
// displays/HID/audio formats...). Most field values need **real-device calibration** (see PROTOCOL-NOTES §7/§8);
// these are structurally correct initial values aligned with protocol behavior, to be tuned item by item during M3 integration.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core::airplay {

struct DeviceInfo {
    std::string name       = "AllPlay";
    std::string deviceId;            ///< AA:BB:CC:DD:EE:FF (NIC MAC)
    std::string btMac;               ///< local Bluetooth MAC (bluetoothIDs, used for audio routing)
    std::string pi;                  ///< pairing uuid
    std::string pkHex;               ///< ed25519 pubkey hex
    int         width  = 1280;       ///< main screen width (target resolution negotiated with the phone)
    int         height = 720;
    int         fps    = 60;
    uint64_t    features = 0x615653AEE2ULL;   ///< CarPlay feature bits (including audio; per the CarPlay feature bit definitions)
};

/// Build the /info binary plist
std::vector<uint8_t> buildInfoPlist(const DeviceInfo& info);

/// Main screen UUID (fixed protocol value; secondary screen ...0002 not implemented yet)
extern const char* const kMainDisplayUuid;

/// Version string (used for sourceVersion / srcvers / protovers)
extern const char* const kSourceVersion;   ///< "950.7.1"
extern const char* const kProtoVersion;    ///< "1.1"

}  // namespace core::airplay
