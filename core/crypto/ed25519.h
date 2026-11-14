/**
 * @file   ed25519.h
 * @brief  Ed25519 sign/verify (OpenSSL)
 * @author blt
 */

// ============================================================================
// core/crypto/ed25519.h — Ed25519 sign/verify (OpenSSL)
// ============================================================================
// In pair-setup/pair-verify: sign with the local long-term Ed25519 private key, verify with the peer's public key (32 bytes).
#pragma once

#include <openssl/evp.h>

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

/// Sign with an EVP_PKEY(Ed25519) private key, returning 64 bytes; empty on failure.
std::vector<uint8_t> ed25519Sign(EVP_PKEY* key, const uint8_t* msg, size_t len);

/// Verify with a raw 32-byte public key.
bool ed25519Verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);

}  // namespace core::crypto
