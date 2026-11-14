/**
 * @file   hkdf.h
 * @brief  HKDF-SHA512 + SHA1/256/512 (OpenSSL implementation)
 * @author blt
 */

// ============================================================================
// core/crypto/hkdf.h — HKDF-SHA512 + SHA1/256/512 (OpenSSL implementation)
// ============================================================================
// Key derivation throughout AirPlay uses HKDF-SHA512 (standard RFC 5869: Extract + Expand).
// salt is passed as the HKDF salt parameter (not concatenated onto the data) — compare standard HKDF (RFC 5869)
// Extract + Expand parameter usage (ikm / salt / info).
// SHA1 is only used for MFi-SAP's AES-KEY/AES-IV derivation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core::crypto {

/// HKDF-SHA512 (RFC 5869). salt may be empty (equivalent to 64 zero bytes).
std::vector<uint8_t> hkdfSha512(const uint8_t* ikm, size_t ikmLen,
                                const uint8_t* salt, size_t saltLen,
                                const uint8_t* info, size_t infoLen,
                                size_t outLen = 32);

/// Convenience: ikm as a vector, salt/info as raw bytes.
std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::vector<uint8_t>& salt,
                                const std::vector<uint8_t>& info,
                                size_t outLen = 32);

/// Convenience: salt/info as ASCII strings.
std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::string& salt,
                                const std::string& info,
                                size_t outLen = 32);

std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha512(const std::vector<uint8_t>& data);

}  // namespace core::crypto
