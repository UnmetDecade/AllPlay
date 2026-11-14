/**
 * @file   mfi_identity.h
 * @brief  MFi accessory identity (interface + certificate implementation)
 * @author blt
 */

// ============================================================================
// core/mfi/mfi_identity.h - MFi accessory identity (interface + certificate implementation)
// ============================================================================
// The protocol layer (Session / iAP2) depends only on the IMfiIdentity interface: it needs two capabilities, "certificate" and "signing".
// Who actually signs does not matter, so either implementation works:
//   1. Certificate implementation (CertificateMfiIdentity): identity.pk8 + certificate.p7b, signing with a software private key.
//   2. Hardware implementation: an MFi authentication chip (EAP / NXP etc.); pass a 32-byte digest to the chip, which returns 64 bytes raw r‖s.
//
// The identity files are separately imported MFi credentials (extracted by scripts/extract-identity.sh):
//   identity.pk8     PKCS#8 DER, EC P-256 private key
//   certificate.p7b  PKCS#7 DER, a single accessory certificate (= what 0xAA01 sends)
//
// Signing semantics (matching the behavior of the MFi-SAP authentication client, LocalMfiAuthenticationClient):
//   Receive the 0xAA02 challenge (32-byte **digest**) -> sign directly with ECDSA (**no re-hashing**) ->
//   output **64 bytes raw r‖s** (not DER).
#pragma once

// ALLPLAY_MFI_STUB: builds without OpenSSL (HarmonyOS phase one only verifies the link layer/identification;
// OpenSSL is not cross-compiled yet). This class keeps only the interface shape; load() always fails,
// and wireless trigger does not load an identity when mfiDir is empty and sends an empty certificate -- enough to verify through 0x1D02.
#ifdef ALLPLAY_MFI_STUB
using EVP_PKEY = void;
#else
#include <openssl/evp.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

namespace core::mfi {

/// MFi accessory identity interface: provides the certificate + signs the challenge digest.
/// A hardware MFi chip or a simple certificate only needs to implement these two methods to plug in.
class IMfiIdentity {
public:
    virtual ~IMfiIdentity() = default;

    /// Accessory certificate (= what iAP2 0xAA01 sends)
    virtual const std::vector<uint8_t>& certificate() const = 0;

    /// Sign a 32-byte digest -> 64 bytes raw r‖s; on failure ok=false
    virtual std::vector<uint8_t> signDigest(const uint8_t* digest, size_t len,
                                            bool* ok = nullptr) const = 0;
};

/// Certificate implementation (software private key signing, the default path)
class CertificateMfiIdentity : public IMfiIdentity {
public:
    ~CertificateMfiIdentity() override;
    CertificateMfiIdentity() = default;
    CertificateMfiIdentity(const CertificateMfiIdentity&) = delete;
    CertificateMfiIdentity& operator=(const CertificateMfiIdentity&) = delete;

    /// Load from a directory (identity.pk8 / certificate.p7b); on failure writes err and returns nullptr
    static std::unique_ptr<CertificateMfiIdentity> load(const std::string& dir, std::string* err);

    /// Load from memory (bytes from places without a readable path, e.g. HAP rawfile); on failure writes err and returns nullptr
    static std::unique_ptr<CertificateMfiIdentity> loadFromMemory(const uint8_t* keyDer, size_t keyLen,
                                                                  const uint8_t* certDer, size_t certLen,
                                                                  std::string* err);

    const std::vector<uint8_t>& certificate() const override { return cert_; }
    std::vector<uint8_t> signDigest(const uint8_t* digest, size_t len,
                                    bool* ok = nullptr) const override;

private:
    EVP_PKEY*            key_ = nullptr;
    std::vector<uint8_t> cert_;
};

}  // namespace core::mfi
