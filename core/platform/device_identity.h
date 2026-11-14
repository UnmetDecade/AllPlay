/**
 * @file   device_identity.h
 * @brief  Local device identity (Ed25519 long-term key + pairing UUID)
 * @author blt
 */

// ============================================================================
// core/platform/device_identity.h — local device identity (Ed25519 long-term key + pairing UUID)
// ============================================================================
// Why it must persist: the phone remembers "this receiver" as a paired device, identified by this Ed25519
// public key (the pk pushed to the phone in 0x4301). After process/machine restarts it must be **the same key**,
// otherwise the phone's pairing record becomes invalid and pairing must be redone every time.
//
// This is distinct from identity/ (MFi accessory identity pk8/p7b):
//   · here is the **AirPlay pairing layer** identity (Ed25519) — generated per machine;
//   · identity/ is the **Apple MFi certification** identity — shared by all machines (a separately supplied credential file, not code).
#pragma once

#include <openssl/evp.h>

#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct DeviceIdentity {
    std::string          pi;      ///< pairing id (UUID string, used by the pi TXT and /info)
    std::string          pkHex;   ///< Ed25519 public key (lowercase hex), used by the pk TXT and /info
    std::vector<uint8_t> pub;     ///< 32-byte raw public key
    EVP_PKEY*            priv = nullptr;   ///< Ed25519 private key (used by pair-verify in M3)

    ~DeviceIdentity();
    DeviceIdentity() = default;
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    /// Read from stateDir; if absent, generate and persist (0600). Returns false and sets err on failure.
    bool loadOrCreate(const std::string& stateDir, std::string* err);
};

/// Default stateDir: $ALLPLAY_STATE_DIR, else $HOME/.local/state/allplay
std::string defaultStateDir();

/// Persist a synthesized deviceid (MAC form, "02:xx:xx:xx:xx:xx", locally administered bit).
/// When to use: when the real NIC MAC is unavailable — notably **since macOS 27, getifaddrs returns
/// link-layer addresses as 02:00:00:00:00:00 for unauthorized processes** (privacy hardening), and the real MAC cannot
/// rely on private ioctls. The synthesized id is generated once and persisted, **stable across restarts and networks** (better than
/// the private Wi-Fi address, which changes with the network, as a pairing identifier).
/// @param created if non-null, filled with "whether it was newly generated"
std::string loadOrCreateSyntheticDeviceId(const std::string& stateDir, bool* created);

}  // namespace core
