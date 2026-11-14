/**
 * @file   video_stream.h
 * @brief  Screen stream (separate TCP data port)
 * @author blt
 */

// ============================================================================
// core/airplay/video_stream.h - Screen stream (separate TCP data port)
// ============================================================================
// wire = [128B header][body]; header offset0 = u32LE bodyLen, offset4 = opcode
// (0 = encrypted video frame, 1 = plaintext config frame); the other 120B is not parsed and is used whole as AAD.
// key = HKDF-SHA512(sharedSecret, "DataStream-Salt"+streamConnectionID,
//                    "DataStream-Output-Encryption-Key", 32).
// Decrypted plaintext = NALU(AVCC) with 4B big-endian length prefix -> convert to AnnexB; keyframe gate.
// Per the AirPlay screen stream protocol (ScreenCodec / MediaCodecSupport behavior).
#pragma once

#include "port/media.h"
#include "port/tcp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>

namespace core::airplay {

class ScreenStream {
public:
    /// listener must already be bound (port known). streamConnectionIdDecimal = unsigned decimal string.
    /// onNeedKeyFrame: callback when feeding a frame fails (dropped frame, broken reference chain), used to request a keyframe from the phone immediately.
    ScreenStream(std::unique_ptr<core::ITcpListener> listener,
                 const std::vector<uint8_t>& sharedSecret,
                 const std::string& streamConnectionIdDecimal,
                 int width, int height, core::IVideoOutput* video,
                 std::function<void()> onNeedKeyFrame = {});
    ~ScreenStream();
    ScreenStream(const ScreenStream&) = delete;

    uint16_t port() const { return port_; }
    void stop();

    /// Keyframe recovery: reset the decoder and re-arm the gate (triggered by /command forceKeyFrame after a dropped keyframe).
    void requestKeyFrame();

private:
    void run();
    void handleConnection(core::ITcpStream& stream);

    static bool detectConfig(const std::vector<uint8_t>& body, core::VideoCodec* codec,
                             std::vector<uint8_t>* codecData);
    /// Logged every 5s: how many frames/bitrate the sender provided vs. how many we gated
    void reportWindow();

    static std::vector<uint8_t> avccToAnnexB(const std::vector<uint8_t>& p);
    static bool isRandomAccess(core::VideoCodec codec, const uint8_t* d, size_t n);

    std::unique_ptr<core::ITcpListener> listener_;
    std::vector<uint8_t> key_;           // 32-byte stream key
    core::IVideoOutput* video_ = nullptr;
    std::function<void()> onNeedKeyFrame_;
    int  width_ = 1280, height_ = 720;
    uint16_t port_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;

    // Parse state (single connection, inside the run/connection thread)
    uint64_t counter_ = 0;
    uint64_t frames_  = 0;   // received video frame count (log throttling)
    bool configured_ = false;
    bool needsKeyFrame_ = true;

    // 5s window stats: frames/bitrate from the sender vs. how many we gated
    // (these three numbers tell whether "stutter" is sender starvation or our side not consuming fast enough)
    uint64_t winFrames_ = 0;
    uint64_t winBytes_  = 0;
    uint64_t winGated_  = 0;   // frames gated while waiting for a keyframe
    uint64_t winDrop_   = 0;   // frames that could not be delivered (decoder enqueue failure)
    std::chrono::steady_clock::time_point winStart_ = std::chrono::steady_clock::now();
    core::VideoCodec codec_ = core::VideoCodec::H264;
};

}  // namespace core::airplay
