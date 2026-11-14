/**
 * @file   pairing_store.h
 * @brief  Persistence of phone (controller) pairing records
 * @author blt
 */

// ============================================================================
// core/platform/pairing_store.h — persistence of phone (controller) pairing records
// ============================================================================
// pair-setup M5 stores the phone's Ed25519 long-term public key (LTPK) + its identifier (UUID string);
// pair-verify M3 fetches it by identifier to verify signatures. v1 stores only the "most recent" phone (single-phone case),
// multi-phone mapping is an M4+ refinement. Written with 0600, in the same directory as device_identity.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

class PairingStore {
public:
    /// Read from stateDir (missing/unreadable returns false without error).
    bool load(const std::string& stateDir);

    bool hasPairing() const { return !controllerId_.empty(); }
    const std::string&        controllerId() const { return controllerId_; }
    const std::vector<uint8_t>& controllerLtpk() const { return controllerLtpk_; }

    /// Save (0600). Returns false on failure.
    bool save(const std::string& stateDir, const std::string& id,
              const std::vector<uint8_t>& ltpk32);

private:
    std::string         controllerId_;
    std::vector<uint8_t> controllerLtpk_;
};

}  // namespace core
