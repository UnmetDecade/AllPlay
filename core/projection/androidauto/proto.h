/**
 * @file   proto.h
 * @brief  极简 protobuf wire 编解码（只覆盖 AAP 用到的子集，不引 protobuf 库）
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/proto.h —— 极简 protobuf wire 编解码
// ============================================================================
// Android Auto 的 AAP 消息体是 protobuf。本项目原则是"只依赖系统库"，所以这里
// 只实现 wire format 的最小子集：
//   · varint（type 0）—— int32 / int64 / uint32 / uint64 / bool / enum
//   · length-delimited（type 2）—— string / bytes / 嵌套消息
// 不生成 .proto、不引 libprotobuf。字段号与类型见 docs/projection-backend.md
// 引用的 AASDK 定义。
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
    // 负数按补码的 64 位 varint 编（protobuf 的规定）
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

/// 顺序读取器：只处理本项目用得到的字段。
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    /// 读下一个字段。返回 false = 读完 / 数据损坏。
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
        return false;   // 不支持的类型（group 等）—— 当作数据损坏
    }

    /// 取一个 varint 字段（找不到返回 fallback）。
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
