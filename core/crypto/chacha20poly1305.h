/**
 * @file   chacha20poly1305.h
 * @brief  ChaCha20-Poly1305 AEAD (RFC 8439)
 * @author blt
 */

// ============================================================================
// core/crypto/chacha20poly1305.h — ChaCha20-Poly1305 AEAD (RFC 8439)
// ============================================================================
// All AirPlay encrypted frames use it: 128-bit tag, 12-byte nonce. Nonces come from two sources:
//   nonce64(counter)  4 zero bytes + 8-byte little-endian counter (control/data streams, independent per direction)
//   nonceLabel(label) 4 zero bytes + 8 bytes containing the label ASCII (pair-verify inner layer)
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

namespace core::crypto {

/// nonce = 4 zero bytes + 8-byte little-endian counter (starts at 0, ++ per frame).
std::array<uint8_t, 12> nonce64(uint64_t counter);

/// nonce = 4 zero bytes + 8 bytes holding the label ASCII (zero-padded if shorter than 8 bytes).
std::array<uint8_t, 12> nonceLabel(const std::string& label);

/// Encrypt: output = ciphertext ‖ 16-byte tag.
std::vector<uint8_t> chachaSeal(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                                const uint8_t* aad, size_t aadLen,
                                const uint8_t* pt, size_t ptLen);

/// Decrypt: the last 16 bytes of ct are the tag. Writes out and returns true on success (false if tag check fails).
bool chachaOpen(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                const uint8_t* aad, size_t aadLen,
                const uint8_t* ct, size_t ctLen,
                std::vector<uint8_t>* out);

}  // namespace core::crypto
