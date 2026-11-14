/**
 * @file   wire.h
 * @brief  iAP2 wire format: CSM frames + parameter encoding (big-endian)
 * @author blt
 */

// ============================================================================
// core/iap2/wire.h - iAP2 wire format: CSM frames + parameter encoding (big-endian)
// ============================================================================
// Protocol facts (compiled from iAP2 frame/wire format):
//   CSM frame: `0x4040 | u16 frame length | u16 msgId | body` (length includes the 6-byte header; no checksum)
//   parameter: `u16 param length | u16 param id | payload` (length includes the 4-byte header)
//   string: UTF-8 + trailing NUL (length includes the NUL); string list = several NUL-terminated strings concatenated
//   u16 list: consecutive big-endian u16; void: empty payload; group: nested parameter table (child params encoded in order)
// This file only handles "how bytes are laid out", not business semantics (those live in wireless_client.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

namespace core::iap2 {

// ---------------------------------------------------------------------------
// Parameter building (ordered, duplicate ids allowed — consistent with the protocol)
// ---------------------------------------------------------------------------
class Params {
public:
    Params& u8(uint16_t id, uint8_t v);
    Params& u16(uint16_t id, uint16_t v);
    Params& u32(uint16_t id, uint32_t v);
    Params& voidv(uint16_t id);                          // empty-payload parameter
    Params& bytes(uint16_t id, const std::vector<uint8_t>& v);
    Params& str(uint16_t id, const std::string& v);      // UTF-8 + NUL
    Params& strList(uint16_t id, const std::vector<std::string>& v);
    Params& u16List(uint16_t id, const std::vector<uint16_t>& v);
    Params& group(uint16_t id, const Params& child);     // nested parameter table

    std::vector<uint8_t> encode() const;

private:
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> items_;
};

// ---------------------------------------------------------------------------
// Parameter parsing (for received peer messages)
// ---------------------------------------------------------------------------
struct Param {
    uint16_t             id = 0;
    std::vector<uint8_t> payload;
};

/// Parse a parameter table; returns false on malformed input (length out of bounds)
bool parseParams(const uint8_t* p, size_t n, std::vector<Param>* out);

/// Find the first parameter with a matching id in the table; returns nullptr if absent
const Param* findParam(const std::vector<Param>& params, uint16_t id);

/// Read a string parameter (strip trailing NUL; returns empty and sets ok=false if there is no NUL)
std::string paramString(const Param& p, bool* ok = nullptr);
/// Read u8 / u16 / u32 (returns 0 and sets ok=false on length mismatch)
uint8_t  paramU8(const Param& p, bool* ok = nullptr);
uint16_t paramU16(const Param& p, bool* ok = nullptr);
uint32_t paramU32(const Param& p, bool* ok = nullptr);

// ---------------------------------------------------------------------------
// CSM frames
// ---------------------------------------------------------------------------
std::vector<uint8_t> csmFrame(uint16_t msgId, const std::vector<uint8_t>& body);

/// Streaming frame splitter: feed arbitrary fragments, produce complete frames (tolerates junk before 0x4040 and concatenation)
class CsmFramer {
public:
    void feed(const uint8_t* p, size_t n);
    /// Take the next complete frame; returns false if none
    bool next(uint16_t* msgId, std::vector<uint8_t>* body);
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

}  // namespace core::iap2
