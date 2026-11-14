/**
 * @file   sdl_video_sink.h
 * @brief  Desktop implementation of core::IVideoOutput: ffmpeg software decode + SDL texture on screen
 * @author blt
 */

// ============================================================================
// host/media/sdl_video_sink.h -- IVideoOutput: ffmpeg software decode + SDL texture on screen
// ============================================================================
// Threads (dictated by the call sites in core, must be followed):
//   · configure() / feedAnnexB() -- ScreenStream thread (core/airplay/video_stream.cpp)
//   · reset()                    -- **RTSP thread** (triggered when the phone sends forceKeyFrame)
//   · present()                  -- main thread (SDL rendering)
// So the decoder and frame queue are serialized by one lock, and SDL textures are touched only on the main thread.
//
// The frame queue is deliberately shallow (default 3): decoded results only matter while they are fresher
// than "on-screen progress"; piling up frames just adds latency. When full, drop the **oldest**, not the
// newest -- and still return true, so core does not think enqueueing failed and re-gates + asks the phone
// for a keyframe (an expensive round trip).
#pragma once

#include "port/media.h"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "media/ffmpeg_decoder.h"

namespace allplay {

/// Fit srcW x srcH into dstW x dstH proportionally, centered (letterbox). Touch normalization uses this rect too.
void fitLetterbox(int srcW, int srcH, int dstW, int dstH, SDL_Rect* out);

class SdlVideoSink : public core::IVideoOutput {
public:
    SdlVideoSink() = default;
    ~SdlVideoSink() override;
    SdlVideoSink(const SdlVideoSink&) = delete;

    // ---- core::IVideoOutput ----
    void configure(core::VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& codecData) override;
    bool feedAnnexB(const uint8_t* data, size_t len) override;
    void reset() override;
    void flush() override {}   // core never calls this

    // ---- Main thread ----
    /// Draw the latest frame into dst (the rect computed by fitLetterbox).
    /// With no new frame, redraws the one in the texture (the picture holds instead of vanishing). false = nothing to draw.
    bool present(SDL_Renderer* r, const SDL_Rect& dst);

    /// Whether there has been at least one frame (decides whether to switch to the video view)
    bool hasFrame() const { return tex_ != nullptr; }

    /// Drop the current frame (**main thread only**). Needed to return to the launcher when a session ends:
    /// otherwise present() keeps redrawing the last frame in the texture and the screen stays stuck on it
    /// -- the log says "back to launcher" yet the picture does not move.
    ///
    /// Also marks the sink as "awaiting the next stream": while a session is winding down the video thread
    /// may still push a few frames in (arriving after dropFrame emptied the queue); if they are kept, the next
    /// session shows a few frames of the **previous session's picture** as soon as it enters Streaming. See feedAnnexB().
    void dropFrame();

    /// Intrinsic video size (0 = no frame decoded yet). Read on the main thread to compute the letterbox.
    void intrinsicSize(int* w, int* h) const;

    /// Number of frames presented on screen (read on the main thread)
    uint64_t presented() const { return presented_; }

    /// Frames output by the decoder and frames squeezed out by the shallow queue (written by the feed thread, read by any thread).
    /// Presented will be less than decoded: when rendering is slow the queue drops the oldest by design. Judge performance by **decode**,
    /// looking at presented misreads "slow rendering" as "slow decoding".
    uint64_t decoded() const { return decoded_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

    /// Whether the decoder has been configured (a config frame has arrived)
    bool configured() const;

private:
    /// Upload f to the texture (rebuilding the texture / falling back on format as needed). Main thread only.
    bool upload(SDL_Renderer* r, const AVFrame* f);
    /// Formats other than YUV420P/NV12 are converted to YUV420P into scratch_ via sws.
    const AVFrame* toYuv420p(const AVFrame* f);

    mutable std::mutex   mu_;
    FfmpegVideoDecoder   dec_;              ///< Guarded by mu_
    std::deque<AVFrame*> queue_;            ///< Awaiting presentation (guarded by mu_)
    size_t               queueCap_ = 3;
    int                  srcW_ = 0, srcH_ = 0;
    /// true after dropFrame() until the next session's config frame arrives: frames fed in during this window
    /// are all the previous session's tail and are always dropped (guarded by mu_).
    bool                 awaitingNewStream_ = false;
    /// Frames dropped during that tail window above (guarded by mu_): reported once on the next configure(),
    /// to prove this guard is actually working rather than no frames happening to arrive.
    uint64_t             tailDropped_ = 0;

    // -- The following is touched only on the main thread --
    SDL_Texture*          tex_ = nullptr;
    int                   texW_ = 0, texH_ = 0;
    Uint32                texFmt_ = 0;
    SwsContext*           sws_ = nullptr;
    int                   swsSrcFmt_ = -1;
    std::vector<uint8_t>  scratch_;
    AVFrame*              scratchFrame_ = nullptr;
    uint64_t              presented_ = 0;
    std::atomic<uint64_t> decoded_{0};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace allplay
