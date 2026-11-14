/**
 * @file   tlv8.h
 * @brief  TLV8 encode/decode (message bodies of pair-setup / pair-verify)
 * @author blt
 */

// ============================================================================
// core/crypto/tlv8.h — TLV8 encode/decode (message bodies of pair-setup / pair-verify)
// ============================================================================
// Each item = [type:1][len:1][value:len]. value>255 is split into 255-byte chunks (each with its own type/len header).
// When two adjacent **items** share a type, insert a separator (0xff, 0x00) between them. On decode 0xff has no special meaning;
// continuation is detected by "same type and previous chunk len==255", otherwise it overwrites.
#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

struct Tlv {
    uint8_t             type;
    std::vector<uint8_t> value;
};

/// Encode (in items order; insert separator between adjacent same-type items; split >255).
std::vector<uint8_t> tlv8Encode(const std::vector<Tlv>& items);

/// Decode (concatenate chunks + same-type overwrite semantics). Truncation is not an error; decode as much as possible.
std::vector<Tlv> tlv8Decode(const uint8_t* data, size_t len);

/// Get the value of a given type from the decode result; returns nullptr if absent.
const std::vector<uint8_t>* tlv8Get(const std::vector<Tlv>& items, uint8_t type);

}  // namespace core::crypto
