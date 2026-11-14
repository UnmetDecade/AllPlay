/**
 * @file   ffmpeg_compat.h
 * @brief  Single-point wrapper for ffmpeg version differences
 * @author blt
 */

// ============================================================================
// host/media/ffmpeg_compat.h -- Single-point wrapper for ffmpeg version differences
// ============================================================================
// The dev machine (macOS/brew) has ffmpeg 8.x (libavcodec 63), the target machine (Ubuntu 22.04)
// has ffmpeg 4.4 (libavcodec 58) -- the two **share no modern channel API**:
//   · `AVChannelLayout` / `ch_layout` only arrived in 5.1 (libavcodec 59.24.100);
//   · the old `channels` / `channel_layout` fields were removed again in 7.0.
// So this directory follows three rules:
//   1. Version branches live **only here**; everywhere else uses the wrappers below;
//   2. The decoder sets no channel layout at all (it infers it from the bitstream) and only **reads** the channel count from frames;
//   3. Resampling is hand-written (see sdl_audio_sink.cpp) without swresample -- which also sidesteps the
//      channel-layout argument signature difference between versions.
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/version.h>
}

/// Boundary where ch_layout replaces channels = libavcodec 59.24.100 (ffmpeg 5.1)
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#  define ALLPLAY_AV_NEW_CHLAYOUT 1
#else
#  define ALLPLAY_AV_NEW_CHLAYOUT 0
#endif

namespace allplay {

/// Channel count of a decoded frame (the access point for both old and new APIs)
inline int frameChannels(const AVFrame* f)
{
#if ALLPLAY_AV_NEW_CHLAYOUT
    return f->ch_layout.nb_channels;
#else
    return f->channels;
#endif
}

}  // namespace allplay
