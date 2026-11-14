/**
 * @file   control_cipher.h
 * @brief  Control channel encryption (enabled after pair-verify)
 * @author blt
 */

// ============================================================================
// core/airplay/control_cipher.h - Control channel encryption (enabled after pair-verify)
// ============================================================================
// Frame = [2-byte LE ciphertext length][ciphertext][16-byte tag]; AAD = those 2 length bytes.
// nonce = 4 zero bytes + 8-byte LE counter (read and write independent, each with its own key).
// Single-frame payload limit 0x4000 (split into multiple frames beyond that).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace core::airplay {

struct ControlKeys {
    std::array<uint8_t, 32> readKey;    ///< decrypt inbound (= phone write key)
    std::array<uint8_t, 32> writeKey;   ///< encrypt outbound (= phone read key)
};

class ControlCipher {
public:
    ControlCipher(const ControlKeys& keys);
    ControlCipher(const uint8_t readKey[32], const uint8_t writeKey[32]);

    /// Encrypt a plaintext chunk -> complete frame(s) (may be multiple frames).
    std::vector<uint8_t> encrypt(const uint8_t* data, size_t len);

    /// Feed ciphertext bytes (buffered internally).
    void feed(const uint8_t* data, size_t len);

    /// Take the next decrypted plaintext block; returns false if less than a full frame is buffered (out untouched).
    bool next(std::vector<uint8_t>* out);

    /// Whether an authentication failure has occurred (the connection should be closed afterwards).
    bool failed() const { return failed_; }

private:
    std::array<uint8_t, 32> readKey_;
    std::array<uint8_t, 32> writeKey_;
    uint64_t readCounter_  = 0;
    uint64_t writeCounter_ = 0;
    std::vector<uint8_t> buf_;
    bool failed_ = false;
};

}  // namespace core::airplay
