/**
 * @file   aes_ctr.h
 * @brief  AES-128-CTR (used for MFi-SAP signing/ciphering)
 * @author blt
 */

// ============================================================================
// core/crypto/aes_ctr.h — AES-128-CTR (used for MFi-SAP signing/ciphering)
// ============================================================================
// In /auth-setup our signature is encrypted with AES-128-CTR (key/iv derived by SHA1("AES-KEY"/"AES-IV"||shared)
// derived). In CTR mode encryption/decryption are the same operation.
#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

std::vector<uint8_t> aes128Ctr(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t* data, size_t len);

}  // namespace core::crypto
