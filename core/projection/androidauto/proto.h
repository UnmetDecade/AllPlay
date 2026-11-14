/**
 * @file   proto.h
 * @brief  Minimal protobuf wire codec (covers only the subset AAP uses; no protobuf library)
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/proto.h - Minimal protobuf wire codec
// ============================================================================
// Android Auto's AAP message bodies are protobuf. This project only depends on system libraries, so here
// we implement only the minimal subset of the wire format:
//   · varint (type 0) -- int32 / int64 / uint32 / uint64 / bool / enum
//   · length-delimited (type 2) -- string / bytes / nested messages
// No .proto generation and no libprotobuf. Field numbers and types follow the AASDK definitions
// referenced by docs/projection-backend.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core::projection::androidauto::pb {

enum WireType : uint32_t { kVarint = 0, kFixed64 = 1, kLengthDelimited = 2, kFixed32 = 5 };

inline void putVarint(std::vector<uint8_t>& out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

inline void putTag(std::vector<uint8_t>& out, uint32_t field, WireType wt)
{
    putVarint(out, (uint64_t(field) << 3) | uint32_t(wt));
}

inline void putUInt(std::vector<uint8_t>& out, uint32_t field, uint64_t v)
{
    putTag(out, field, kVarint);
    putVarint(out, v);
}

inline void putInt(std::vector<uint8_t>& out, uint32_t field, int64_t v)
{
    // Negative numbers are encoded as 64-bit two's complement varints (per the protobuf spec)
    putUInt(out, field, uint64_t(v));
}

inline void putBool(std::vector<uint8_t>& out, uint32_t field, bool v)
{
    putUInt(out, field, v ? 1 : 0);
}

inline void putBytes(std::vector<uint8_t>& out, uint32_t field,
                     const uint8_t* data, size_t len)
{
    putTag(out, field, kLengthDelimited);
    putVarint(out, len);
    out.insert(out.end(), data, data + len);
}

inline void putString(std::vector<uint8_t>& out, uint32_t field, const std::string& s)
{
    putBytes(out, field, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline void putMessage(std::vector<uint8_t>& out, uint32_t field,
                       const std::vector<uint8_t>& body)
{
    putBytes(out, field, body.data(), body.size());
}

/// Sequential reader: handles only the fields this project needs.
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    /// Read the next field. false = end of data / corrupt data.
    bool next(uint32_t* field, WireType* wt, uint64_t* varint,
              const uint8_t** bytes, size_t* bytesLen)
    {
        uint64_t key = 0;
        if (!readVarint(&key)) return false;
        *field = uint32_t(key >> 3);
        *wt    = WireType(key & 7);
        *varint = 0;
        *bytes = nullptr;
        *bytesLen = 0;

        if (*wt == kVarint) return readVarint(varint);
        if (*wt == kLengthDelimited) {
            uint64_t n = 0;
            if (!readVarint(&n) || n > size_t(end_ - p_)) return false;
            *bytes = p_;
            *bytesLen = size_t(n);
            p_ += n;
            return true;
        }
        if (*wt == kFixed64) {
            if (size_t(end_ - p_) < 8) return false;
            *varint = 0;
            for (int i = 0; i < 8; ++i) *varint |= uint64_t(p_[i]) << (8 * i);
            p_ += 8;
            return true;
        }
        if (*wt == kFixed32) {
            if (size_t(end_ - p_) < 4) return false;
            *varint = 0;
            for (int i = 0; i < 4; ++i) *varint |= uint64_t(p_[i]) << (8 * i);
            p_ += 4;
            return true;
        }
        return false;   // Unsupported type (group etc.) -- treat as corrupt data
    }

    /// Get a varint field (returns fallback if not found).
    static uint64_t getVarint(const uint8_t* data, size_t len, uint32_t wanted,
                              uint64_t fallback = 0)
    {
        Reader r(data, len);
        uint32_t f; WireType wt; uint64_t v; const uint8_t* b; size_t n;
        while (r.next(&f, &wt, &v, &b, &n)) {
            if (f == wanted && wt == kVarint) return v;
        }
        return fallback;
    }

private:
    bool readVarint(uint64_t* out)
    {
        uint64_t v = 0;
        int shift = 0;
        while (p_ < end_ && shift <= 63) {
            const uint8_t b = *p_++;
            v |= uint64_t(b & 0x7F) << shift;
            if ((b & 0x80) == 0) { *out = v; return true; }
            shift += 7;
        }
        return false;
    }

    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace core::projection::androidauto::pb
