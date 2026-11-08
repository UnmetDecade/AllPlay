/**
 * @file   aap.cpp
 * @brief  AAP 分帧 / 拼帧
 */

#include "projection/androidauto/aap.h"

#include <algorithm>
#include <cstring>

namespace core::projection::androidauto {

namespace {
constexpr uint8_t kFlagFirst     = 0x01;
constexpr uint8_t kFlagLast      = 0x02;
constexpr uint8_t kFlagControl   = 0x04;
constexpr uint8_t kFlagEncrypted = 0x08;
}  // namespace

void FrameParser::feed(const uint8_t* data, size_t len)
{
    if (off_ > 0 && off_ >= buf_.size() / 2) compact_();
    buf_.insert(buf_.end(), data, data + len);
}

void FrameParser::reset()
{
    buf_.clear();
    off_ = 0;
}

void FrameParser::compact_()
{
    if (off_ == 0) return;
    buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(off_));
    off_ = 0;
}

bool FrameParser::next(Frame* out)
{
    const size_t avail = buf_.size() - off_;
    if (avail < 2) return false;

    const uint8_t* p = buf_.data() + off_;
    const uint8_t  chFlags = p[1];
    const bool first     = (chFlags & kFlagFirst) != 0;
    const bool last      = (chFlags & kFlagLast) != 0;
    const bool control   = (chFlags & kFlagControl) != 0;
    const bool encrypted = (chFlags & kFlagEncrypted) != 0;

    // FIRST（且不是单帧 BULK）带 6 字节长度：u16 本帧长度 + u32 明文总长
    const bool extended = first && !last;
    const size_t sizeLen = extended ? 6 : 2;
    if (avail < 2 + sizeLen) return false;

    const uint16_t payloadLen = uint16_t((uint16_t(p[2]) << 8) | p[3]);
    uint32_t total = 0;
    if (extended) {
        total = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) |
                (uint32_t(p[6]) << 8)  |  uint32_t(p[7]);
    }

    const size_t need = 2 + sizeLen + payloadLen;
    if (avail < need) return false;

    out->channel   = Channel(p[0]);
    out->first     = first;
    out->last      = last;
    out->control   = control;
    out->encrypted = encrypted;
    out->totalSize = total;
    out->payload.assign(p + 2 + sizeLen, p + need);

    off_ += need;
    if (off_ == buf_.size()) {
        buf_.clear();
        off_ = 0;
    }
    return true;
}

std::vector<uint8_t> encodeFrame(Channel ch, bool encrypted, bool control,
                                 const uint8_t* payload, size_t len,
                                 bool first, bool last, uint32_t totalPlain)
{
    std::vector<uint8_t> out;
    out.reserve(2 + (first && !last ? 6 : 2) + len);

    uint8_t flags = 0;
    if (first) flags |= kFlagFirst;
    if (last)  flags |= kFlagLast;
    if (control) flags |= kFlagControl;
    if (encrypted) flags |= kFlagEncrypted;

    out.push_back(uint8_t(ch));
    out.push_back(flags);

    if (first && !last) {
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len));
        out.push_back(uint8_t(totalPlain >> 24));
        out.push_back(uint8_t(totalPlain >> 16));
        out.push_back(uint8_t(totalPlain >> 8));
        out.push_back(uint8_t(totalPlain));
    } else {
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len));
    }
    out.insert(out.end(), payload, payload + len);
    return out;
}

}  // namespace core::projection::androidauto
