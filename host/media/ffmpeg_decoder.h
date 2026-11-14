/**
 * @file   ffmpeg_decoder.h
 * @brief  H.264/HEVC software decode (libavcodec), AnnexB in, AVFrame out
 * @author blt
 */

// ============================================================================
// host/media/ffmpeg_decoder.h -- H.264/HEVC software decode (libavcodec)
// ============================================================================
// Decoding only, no SDL -- keeps ffmpeg's API surface in one file so SDK changes are easy to locate.
//
// ⚠️ The extradata trap (ignore it and the picture stays corrupted/black):
//   The codecData core passes in is an **avcC/hvcC record body** (first byte 0x01). But ffmpeg's
//   H.264/HEVC decoder switches to "4-byte length prefix" mode as soon as it sees extradata
//   starting with 1, and then feeding AnnexB (start codes) decodes everything wrong. So configure()
//   splits the SPS/PPS out of avcC/hvcC and gives each a start code as extradata -- the same idea as Android's
//   MediaCodecSink.buildCsd() (there it feeds the split pieces to MediaCodec's csd-N).
#pragma once

#include "port/media.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdint>
#include <functional>
#include <vector>
#include <cstddef>

namespace allplay {

class FfmpegVideoDecoder {
public:
    enum class Action {
        Rebuilt,     ///< Decoder created/rebuilt
        Unchanged,   ///< Config identical to current, rebuild skipped (avoids periodic black frames)
        Failed,      ///< Could not create (codec not compiled in / OOM)
    };

    FfmpegVideoDecoder() = default;
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    /// Configure (rebuild) the decoder from avcC/hvcC.
    /// Compares against the previous (codec, codecData) first: if identical, returns Unchanged and leaves the decoder alone.
    Action configure(core::VideoCodec codec, const std::vector<uint8_t>& codecData);

    bool usable() const { return ctx_ != nullptr; }

    /// Feed one AnnexB frame; decoded frames (0..n) go through the onFrame callback, and a frame is invalid once the callback returns.
    /// Returns false = decoder unusable (the caller should gate until a keyframe instead of continuing to feed).
    bool decode(const uint8_t* data, size_t len,
                const std::function<void(const AVFrame*)>& onFrame);

    /// Clear the reference chain (keyframe recovery). extradata and already-decoded frames are unaffected.
    void reset();

    int width() const { return width_; }
    int height() const { return height_; }

private:
    void close();
    void receiveAll(const std::function<void(const AVFrame*)>& onFrame);

    AVCodecContext*      ctx_   = nullptr;
    AVPacket*            pkt_   = nullptr;
    AVFrame*             frame_ = nullptr;
    core::VideoCodec     codec_ = core::VideoCodec::H264;
    std::vector<uint8_t> codecData_;   ///< Last config (for dedup)
    int                  width_ = 0, height_ = 0;
};

}  // namespace allplay
