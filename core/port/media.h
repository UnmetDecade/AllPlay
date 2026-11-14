/**
 * @file   media.h
 * @brief  Video output abstraction (decoding + display in one)
 * @author blt
 */

// ============================================================================
// core/port/media.h — video output abstraction (decoding + display in one)
// ============================================================================
// core feeds "decoded AnnexB frames" into this interface; how decoding/rendering happens is up to the platform:
//   Android = MediaCodec → SurfaceView (hardware decode, csd uses avcC/hvcC)
//   desktop = ffmpeg decode → SDL texture
//
// ⚠️ Threading: configure()/feedAnnexB() come from the ScreenStream thread, while reset() comes from the RTSP thread
//    (/command forceKeyFrame). Implementations must lock themselves; core does not — see docs/architecture.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace core {

enum class VideoCodec { H264, HEVC };

class IVideoOutput {
public:
    virtual ~IVideoOutput() = default;

    /// Called after receiving the config frame (avcC/hvcC); codecData = ISO BMFF avcC/hvcC record (without the fourcc).
    virtual void configure(VideoCodec codec, int width, int height,
                           const std::vector<uint8_t>& codecData) = 0;

    /// Feed one AnnexB frame (NALU prefixed with 00 00 00 01, possibly containing multiple NALUs).
    /// Returning false means it could not be queued (decoder busy/no input buffer, frame dropped);
    /// the caller should re-gate and wait for the next keyframe, to avoid accumulating corruption after a broken P-frame reference chain.
    virtual bool feedAnnexB(const uint8_t* data, size_t len) = 0;

    /// Keyframe recovery: reset decoder / reference chain (called by core after a lost keyframe).
    virtual void reset() = 0;

    /// Flush the decoder before a new IDR (clear the reference chain, avoid a garbled first frame on scene changes).
    virtual void flush() = 0;
};

// ---------------------------------------------------------------------------
// Audio output (CarPlay audio streams 100/101/102, RTP over UDP)
// ---------------------------------------------------------------------------
enum class AudioCodec { AAC_LC, OPUS, LPCM };

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    /// Called once after the audio format is negotiated (on every audio stream SETUP/rebuild).
    virtual void configure(AudioCodec codec, int sampleRate, int channels) = 0;

    /// Feed one decrypted RTP payload (one frame of audio data). sample = RTP timestamp (for reordering/clocking).
    virtual void feed(const uint8_t* payload, size_t len, uint32_t sample) = 0;

    /// Reset when the stream ends/is rebuilt.
    virtual void reset() = 0;
};

}  // namespace core
