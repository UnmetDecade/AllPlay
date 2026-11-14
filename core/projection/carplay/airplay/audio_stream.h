/**
 * @file   audio_stream.h
 * @brief  CarPlay audio stream (RTP/RTCP over UDP)
 * @author blt
 */

// ============================================================================
// core/airplay/audio_stream.h - CarPlay audio stream (RTP/RTCP over UDP)
// ============================================================================
// Wire layout (per the AirPlay protocol): one RTP packet = [12B RTP header][ciphertext][16B tag][8B nonce].
//   AAD   = RTP header bytes 4..12 (timestamp + SSRC, 8 bytes)
//   nonce = 4 zero bytes + last 8 bytes little-endian
//   key   = HKDF-SHA512(shared, "DataStream-Salt"+streamConnectionID,
//                       "DataStream-Output-Encryption-Key", 32) (same derivation as the screen stream)
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

    /// RTP reordering: buffer a few packets by sequence number and emit them in order (AAC/Opus decoding is sensitive to reordering).
    void deliver(const uint8_t* payload, size_t len, uint32_t sample, uint16_t seq);

    std::unique_ptr<core::IUdpSocket> dataSock_, controlSock_;
    std::array<uint8_t, 32> key_{};
    AudioCodec codec_;
    int sampleRate_ = 48000, channels_ = 1;
    core::IAudioOutput* output_ = nullptr;
    uint16_t dataPort_ = 0, controlPort_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    // Reorder state (accessed only by the run thread)
    struct Pkt { std::vector<uint8_t> payload; uint32_t sample = 0; };
    std::map<uint64_t, Pkt> pending_;   // extended sequence number -> packet
    uint64_t nextSeq_ = 0;              // next extended sequence number to emit
    uint16_t lastRaw_ = 0;              // previous raw sequence number (for wraparound/reorder delta)
    uint64_t lastExt_ = 0;
    bool     haveSeq_ = false;
};

}  // namespace core::airplay
