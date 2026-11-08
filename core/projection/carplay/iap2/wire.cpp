#include "iap2/wire.h"

#include <algorithm>
#include <cstring>

namespace core::iap2 {

namespace {

void putU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

uint16_t readU16(const uint8_t* p)
{
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

uint32_t readU32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::vector<uint8_t> encodeParam(uint16_t id, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    out.reserve(4 + payload.size());
    putU16(out, uint16_t(4 + payload.size()));
    putU16(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

Params& Params::u8(uint16_t id, uint8_t v)      { items_.emplace_back(id, std::vector<uint8_t>{v}); return *this; }
Params& Params::voidv(uint16_t id)               { items_.emplace_back(id, std::vector<uint8_t>{});    return *this; }
Params& Params::bytes(uint16_t id, const std::vector<uint8_t>& v) { items_.emplace_back(id, v);       return *this; }
Params& Params::group(uint16_t id, const Params& c)               { items_.emplace_back(id, c.encode()); return *this; }

Params& Params::u16(uint16_t id, uint16_t v)
{
    std::vector<uint8_t> b;
    putU16(b, v);
    items_.emplace_back(id, std::move(b));
    return *this;
}

Params& Params::u32(uint16_t id, uint32_t v)
{
    std::vector<uint8_t> b;
    putU32(b, v);
    items_.emplace_back(id, std::move(b));
    return *this;
}

Params& Params::str(uint16_t id, const std::string& v)
{
    std::vector<uint8_t> b(v.begin(), v.end());
    b.push_back(0);   // NUL 结尾（长度含它）
    items_.emplace_back(id, std::move(b));
    return *this;
}

Params& Params::strList(uint16_t id, const std::vector<std::string>& v)
{
    std::vector<uint8_t> b;
    for (const auto& s : v) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    items_.emplace_back(id, std::move(b));
    return *this;
}

Params& Params::u16List(uint16_t id, const std::vector<uint16_t>& v)
{
    std::vector<uint8_t> b;
    b.reserve(v.size() * 2);
    for (uint16_t x : v) putU16(b, x);
    items_.emplace_back(id, std::move(b));
    return *this;
}

std::vector<uint8_t> Params::encode() const
{
    std::vector<uint8_t> out;
    for (const auto& it : items_) {
        auto enc = encodeParam(it.first, it.second);
        out.insert(out.end(), enc.begin(), enc.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

bool parseParams(const uint8_t* p, size_t n, std::vector<Param>* out)
{
    out->clear();
    size_t off = 0;
    while (off + 4 <= n) {
        const uint16_t len = readU16(p + off);
        if (len < 4 || off + len > n) return false;
        Param param;
        param.id = readU16(p + off + 2);
        param.payload.assign(p + off + 4, p + off + len);
        out->push_back(std::move(param));
        off += len;
    }
    return off == n;
}

const Param* findParam(const std::vector<Param>& params, uint16_t id)
{
    for (const auto& p : params)
        if (p.id == id) return &p;
    return nullptr;
}

std::string paramString(const Param& p, bool* ok)
{
    if (ok) *ok = false;
    if (p.payload.empty() || p.payload.back() != 0) return {};
    if (ok) *ok = true;
    return std::string(p.payload.begin(), p.payload.end() - 1);
}

uint8_t paramU8(const Param& p, bool* ok)
{
    if (ok) *ok = (p.payload.size() == 1);
    return p.payload.size() == 1 ? p.payload[0] : 0;
}

uint16_t paramU16(const Param& p, bool* ok)
{
    if (ok) *ok = (p.payload.size() == 2);
    return p.payload.size() == 2 ? readU16(p.payload.data()) : 0;
}

uint32_t paramU32(const Param& p, bool* ok)
{
    if (ok) *ok = (p.payload.size() == 4);
    return p.payload.size() == 4 ? readU32(p.payload.data()) : 0;
}

// ---------------------------------------------------------------------------
// CSM
// ---------------------------------------------------------------------------

std::vector<uint8_t> csmFrame(uint16_t msgId, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> out;
    out.reserve(6 + body.size());
    putU16(out, 0x4040);
    putU16(out, uint16_t(6 + body.size()));
    putU16(out, msgId);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void CsmFramer::feed(const uint8_t* p, size_t n)
{
    buf_.insert(buf_.end(), p, p + n);
}

bool CsmFramer::next(uint16_t* msgId, std::vector<uint8_t>* body)
{
    // 找 0x4040（丢弃前面的垃圾）
    size_t off = 0;
    while (buf_.size() - off >= 2 && !(buf_[off] == 0x40 && buf_[off + 1] == 0x40)) ++off;
    if (off > 0) buf_.erase(buf_.begin(), buf_.begin() + off);
    if (buf_.size() < 6) return false;

    const uint16_t len = readU16(buf_.data() + 2);
    if (len < 6) {                     // 非法长度：丢 1 字节重新对齐
        buf_.erase(buf_.begin());
        return next(msgId, body);
    }
    if (buf_.size() < len) return false;

    *msgId = readU16(buf_.data() + 4);
    body->assign(buf_.begin() + 6, buf_.begin() + len);
    buf_.erase(buf_.begin(), buf_.begin() + len);
    return true;
}

}  // namespace core::iap2
