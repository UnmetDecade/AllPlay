/**
 * @file   x25519.h
 * @brief  X25519 key exchange (OpenSSL)
 * @author blt
 */

// ============================================================================
// core/crypto/x25519.h — X25519 key exchange (OpenSSL)
// ============================================================================
// pair-verify and /auth-setup both use ephemeral X25519 for ECDH.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace core::crypto {

struct X25519Key {
    std::array<uint8_t, 32> pub;
    std::array<uint8_t, 32> priv;
};

/// Generate an ephemeral key pair randomly.
X25519Key x25519Generate();

/// DH: priv(32) × peerPub(32) → 32-byte shared secret; returns empty on failure (or all-zero).
std::vector<uint8_t> x25519Shared(const uint8_t priv[32], const uint8_t peerPub[32]);

}  // namespace core::crypto
