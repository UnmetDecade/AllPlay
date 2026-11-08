/**
 * @file   audio_stream.h
 * @brief  CarPlay 音频流（RTP/RTCP over UDP）
 * @author blt
 */

// ============================================================================
// core/airplay/audio_stream.h —— CarPlay 音频流（RTP/RTCP over UDP）
// ============================================================================
// wire 布局（参照 AirPlay 协议）：一个 RTP 包 = [12B RTP 头][密文][16B tag][8B nonce]。
//   AAD   = RTP 头第 4..12 字节（timestamp + SSRC，8 字节）
//   nonce = 4 个零字节 + 尾部 8 字节小端
//   密钥  = HKDF-SHA512(shared, "DataStream-Salt"+streamConnectionID,
//                        "DataStream-Output-Encryption-Key", 32)（与屏幕流同款派生）
#pragma once

#include "port/media.h"
#include "port/udp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>

namespace core::airplay {

class AudioStream {
public:
    AudioStream(std::function<std::unique_ptr<core::IUdpSocket>()> makeUdp,
                const std::vector<uint8_t>& sharedSecret,
                const std::string& streamConnectionIdDecimal,
                AudioCodec codec, int sampleRate, int channels,
                core::IAudioOutput* output);
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;

    uint16_t dataPort() const { return dataPort_; }
    uint16_t controlPort() const { return controlPort_; }
    void stop();

private:
    void run();

    /// RTP 乱序重排：按序号缓冲少量包再按序吐出（AAC/Opus 解码对乱序敏感）。
    void deliver(const uint8_t* payload, size_t len, uint32_t sample, uint16_t seq);

    std::unique_ptr<core::IUdpSocket> dataSock_, controlSock_;
    std::array<uint8_t, 32> key_{};
    AudioCodec codec_;
    int sampleRate_ = 48000, channels_ = 1;
    core::IAudioOutput* output_ = nullptr;
    uint16_t dataPort_ = 0, controlPort_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    // 重排状态（仅 run 线程访问）
    struct Pkt { std::vector<uint8_t> payload; uint32_t sample = 0; };
    std::map<uint64_t, Pkt> pending_;   // 扩展序号 → 包
    uint64_t nextSeq_ = 0;              // 下一个要吐出的扩展序号
    uint16_t lastRaw_ = 0;              // 上一个原始序号（算回绕/乱序增量用）
    uint64_t lastExt_ = 0;
    bool     haveSeq_ = false;
};

}  // namespace core::airplay
