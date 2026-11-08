/**
 * @file   control_cipher.h
 * @brief  控制通道加密（pair-verify 之后启用）
 * @author blt
 */

// ============================================================================
// core/airplay/control_cipher.h —— 控制通道加密（pair-verify 之后启用）
// ============================================================================
// 帧 = [2 字节 LE 密文长][密文][16 字节 tag]；AAD = 那 2 字节长度头。
// nonce = 4 零字节 + 8 字节 LE 计数器（读写各自独立、各自密钥）。
// 单帧负载上限 0x4000（超了分帧）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace core::airplay {

struct ControlKeys {
    std::array<uint8_t, 32> readKey;    ///< 解密入站（= 手机 write key）
    std::array<uint8_t, 32> writeKey;   ///< 加密出站（= 手机 read key）
};

class ControlCipher {
public:
    ControlCipher(const ControlKeys& keys);
    ControlCipher(const uint8_t readKey[32], const uint8_t writeKey[32]);

    /// 加密一段明文 → 完整帧（可能多帧）。
    std::vector<uint8_t> encrypt(const uint8_t* data, size_t len);

    /// 喂入密文字节（内部缓冲）。
    void feed(const uint8_t* data, size_t len);

    /// 取下一个解出的明文块；缓冲不足一整帧返回 false（out 不动）。
    bool next(std::vector<uint8_t>* out);

    /// 是否发生过鉴权失败（此后连接应关闭）。
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
