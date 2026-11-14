/**
 * @file   sdl_audio_sink.h
 * @brief  Desktop implementation of core::IAudioOutput: ffmpeg software decode + SDL audio device
 * @author blt
 */

// ============================================================================
// host/media/sdl_audio_sink.h -- IAudioOutput: ffmpeg software decode + SDL audio device
// ============================================================================
// Threads (dictated by the call sites in core, must be followed):
//   · configure() -- **RTSP thread** (core/airplay/session.cpp:392, on every audio-stream SETUP)
//   · feed()      -- AudioStream thread (core/airplay/audio_stream.cpp:run)
//   · serviceDevice() -- **main thread** (SDL opens/closes devices only on the main thread; see the appstate contract)
//   · audio callback -- SDL's own audio thread (only pulls data from the ring buffer)
//
// Out-of-order handling is not needed: core's AudioStream::deliver() already reorders by RTP sequence and
// drops late packets, so feed() always gets them in order. Here we only decode + buffer + feed the device.
//
// Three payload kinds (AudioCodec in core/port/media.h):
//   · AAC_LC -- raw frames, **no ADTS header**, so build a 2-byte ASC from the negotiated values as extradata;
//               small <=12-byte config packets also slip in and must be ignored (feeding them as frames makes the decoder error out repeatedly).
//   · OPUS   -- raw packets, extradata is a minimal 19-byte OpusHead.
//   · LPCM   -- no decoding; **big-endian s16 (network order) converted to host order** goes into the ring buffer.
//
// No swresample: the decoder outputs planar float (FLTP), and "planar float -> interleaved s16" takes only
// about thirty lines to write, which also sidesteps swresample's channel-layout argument, whose signature
// differs between 4.4 and 5.1+ (see media/ffmpeg_compat.h). Sample rate is not resampled -- the device opens
// at the negotiated rate, and CarPlay only sends 44100/48000, both natively supported by SDL.
#pragma once

#include "port/media.h"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace allplay {

class SdlAudioSink : public core::IAudioOutput {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink() override;
    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    // ---- core::IAudioOutput ----
    void configure(core::AudioCodec codec, int sampleRate, int channels) override;
    void feed(const uint8_t* payload, size_t len, uint32_t sample) override;
    void reset() override;

    // ---- Main thread ----
    /// Open the pending device recorded by configure() as an SDL device. Returns immediately if unchanged, so it can be called every frame.
    void serviceDevice();

    // ---- Stats (read from any thread) ----
    uint64_t decoded() const { return decoded_.load(); }   ///< Frames decoded and written into the ring buffer
    uint64_t dropped() const { return dropped_.load(); }   ///< Times the buffer overflowed and the oldest had to be dropped
    /// Times the callback could not get a full chunk. ⚠️ When running headless (SDL_AUDIODRIVER=dummy) this number **must not be used as a sound-quality metric**:
    /// the dummy device does not call back in real time, so all the jitter is attributed to it. Only meaningful on real hardware.
    uint64_t underruns() const { return underruns_.load(); }

    bool deviceOpen() const { return dev_ != 0; }

private:
    /// Number of s16 samples the ring buffer holds (mono count), about 2 seconds.
    static constexpr size_t kRingSamples = 48000 * 2;
    /// Frames SDL requests per callback (about 21 ms @48k at the negotiated rate).
    static constexpr int    kChunkFrames = 1024;
    /// Accumulate this many chunks before starting playback. The phone sends at **real-time rate**, so playing
    /// as soon as the device opens starts from an empty buffer and the first few chunks inevitably underrun --
    /// pad about 43 ms in, without adding too much A/V sync latency.
    /// (The exact chunk count is empirical: SDL's dummy device does not call back precisely, so this number
    ///   makes no measurable difference headless; the value just needs to cover the startup hiccup.)
    static constexpr int    kStartChunks = 2;

    struct Config {
        core::AudioCodec codec = core::AudioCodec::LPCM;
        int rate     = 48000;
        int channels = 2;
        bool valid   = false;   ///< false = no audio stream, the device should be closed
    };

    static void sdlCallback(void* user, Uint8* stream, int len);

    /// feed() decode path: feed one packet to the decoder and append decoded frames as s16 to the ring buffer. Call with the lock held.
    void decodeLocked(const uint8_t* payload, size_t len);
    /// Convert one frame (planar float or s16) to interleaved s16 and append it to the ring buffer. Call with the lock held.
    void appendFrameLocked(const AVFrame* f);
    /// Write to the ring buffer, dropping the **oldest** on overflow (keep latency low, same idea as the video queue). Call with the lock held.
    void pushLocked(const int16_t* d, size_t n);

    void closeDecoderLocked();
    /// Create the decoder for codec (extradata synthesized here). Call with the lock held.
    bool openDecoderLocked();

    mutable std::mutex mu_;

    // -- Guarded by mu_ --
    Config                     cfg_;
    bool                       deviceDirty_ = false;   ///< Set by configure(), consumed by the main thread
    AVCodecContext*            dec_ = nullptr;
    AVPacket*                  pkt_ = nullptr;
    AVFrame*                   frame_ = nullptr;
    std::vector<int16_t>       ring_;
    std::vector<int16_t>       scratch_;               ///< Landing buffer for decoded frame -> interleaved s16
    std::vector<int16_t>       pcmSwap_;               ///< Landing buffer for LPCM big-endian -> host order
    size_t                     head_ = 0, size_ = 0;   ///< Ring buffer read position / stored sample count
    std::atomic<uint64_t>      decoded_{0};
    std::atomic<uint64_t>      dropped_{0};
    std::atomic<uint64_t>      underruns_{0};
    bool                       stop_ = false;          ///< Set on destruction so the callback steps aside

    /// Playback threshold (samples = chunks x frames per chunk x channels), computed at configure time.
    size_t                     startSamples_ = 0;

    // -- Touched only on the main thread --
    SDL_AudioDeviceID dev_ = 0;
    bool              playing_ = false;   ///< Whether the callback is unblocked (true only once enough has accumulated)

    // Read-only snapshot for lock-free reads by callers (written at configure time)
    std::atomic<int> outRate_{0};
    std::atomic<int> outChannels_{0};
};

}  // namespace allplay
