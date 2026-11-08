#include "airplay/control_cipher.h"

#include "crypto/chacha20poly1305.h"

#include <algorithm>

namespace core::airplay {

namespace {
constexpr size_t kMaxPayload = 0x4000;   // 16384，参照协议的帧体上限
}

ControlCipher::ControlCipher(const ControlKeys& keys)
    : readKey_(keys.readKey), writeKey_(keys.writeKey) {}

ControlCipher::ControlCipher(const uint8_t readKey[32], const uint8_t writeKey[32])
{
    std::copy(readKey, readKey + 32, readKey_.begin());
    std::copy(writeKey, writeKey + 32, writeKey_.begin());
}

std::vector<uint8_t> ControlCipher::encrypt(const uint8_t* data, size_t len)
{
    std::vector<uint8_t> out;
    size_t off = 0;
    do {
        const size_t n = std::min(kMaxPayload, len - off);
        std::array<uint8_t, 2> hdr{uint8_t(n & 0xFF), uint8_t((n >> 8) & 0xFF)};
        auto sealed = crypto::chachaSeal(writeKey_.data(), crypto::nonce64(writeCounter_),
                                         hdr.data(), hdr.size(), data + off, n);
        ++writeCounter_;
        out.insert(out.end(), hdr.begin(), hdr.end());
        out.insert(out.end(), sealed.begin(), sealed.end());
        off += n;
    } while (off < len);
    return out;
}

void ControlCipher::feed(const uint8_t* data, size_t len)
{
    buf_.insert(buf_.end(), data, data + len);
}

bool ControlCipher::next(std::vector<uint8_t>* out)
{
    while (buf_.size() >= 2) {
        const size_t ctLen = size_t(buf_[0]) | (size_t(buf_[1]) << 8);
        const size_t frameLen = 2 + ctLen + 16;
        if (buf_.size() < frameLen) return false;   // 未凑齐一整帧

        std::array<uint8_t, 2> hdr{buf_[0], buf_[1]};
        std::vector<uint8_t> pt;
        const bool ok = crypto::chachaOpen(readKey_.data(), crypto::nonce64(readCounter_),
                                           hdr.data(), hdr.size(),
                                           buf_.data() + 2, ctLen + 16, &pt);
        ++readCounter_;
        buf_.erase(buf_.begin(), buf_.begin() + frameLen);
        if (!ok) { failed_ = true; return false; }   // 鉴权失败
        *out = std::move(pt);
        return true;
    }
    return false;
}

}  // namespace core::airplay
