#include "media/sdl_video_sink.h"

#include "common/log.h"

#include <algorithm>

namespace allplay {

namespace {
constexpr const char* kTag = "video";
}  // namespace

void fitLetterbox(int srcW, int srcH, int dstW, int dstH, SDL_Rect* out)
{
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        *out = SDL_Rect{0, 0, dstW, dstH};
        return;
    }
    const double s = std::min(double(dstW) / double(srcW), double(dstH) / double(srcH));
    const int w = int(double(srcW) * s + 0.5);
    const int h = int(double(srcH) * s + 0.5);
    out->x = (dstW - w) / 2;
    out->y = (dstH - h) / 2;
    out->w = w;
    out->h = h;
}

SdlVideoSink::~SdlVideoSink()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        while (!queue_.empty()) {
            av_frame_free(&queue_.front());
            queue_.pop_front();
        }
    }
    if (tex_) SDL_DestroyTexture(tex_);
    if (sws_) sws_freeContext(sws_);
    if (scratchFrame_) av_frame_free(&scratchFrame_);
}

// ---------------------------------------------------------------------------
// core side (ScreenStream thread / RTSP thread)
// ---------------------------------------------------------------------------

void SdlVideoSink::configure(core::VideoCodec codec, int width, int height,
                             const std::vector<uint8_t>& codecData)
{
    std::lock_guard<std::mutex> lk(mu_);
    // Is this "the next session's first config frame"? (the first configure() after dropFrame())
    // If so, frames decoded but not yet presented from the previous session, and the reference chain held in the
    // decoder, must be cleared -- otherwise they get drawn right as the new session enters Streaming, and the
    // user sees "a few frames of the previous session".
    const bool sessionStart = awaitingNewStream_;
    awaitingNewStream_ = false;
    if (sessionStart) {
        while (!queue_.empty()) {
            av_frame_free(&queue_.front());
            queue_.pop_front();
        }
        dec_.reset();          // clear the reference chain (extradata is kept; configure below re-compares it)
        srcW_ = srcH_ = 0;
        if (tailDropped_) {
            LOG_INFO("%s: %llu frames arrived after the previous session wrapped up, dropped",
                     kTag, static_cast<unsigned long long>(tailDropped_));
            tailDropped_ = 0;
        }
    }
    // width/height are only the negotiated values; the real size comes from the bitstream. Used for logging only here
    const auto action = dec_.configure(codec, codecData);
    if (action == FfmpegVideoDecoder::Action::Unchanged) {
        LOG_INFO("%s: config unchanged (%dx%d), skipping rebuild", kTag, width, height);
        return;
    }
    if (action == FfmpegVideoDecoder::Action::Failed) {
        LOG_ERROR("%s: decoder configuration failed, screen stream will gate and wait for a keyframe", kTag);
        return;
    }
    // Config changed: frames from the old decoder may still be sitting in the queue; keep them out of the new picture
    while (!queue_.empty()) {
        av_frame_free(&queue_.front());
        queue_.pop_front();
    }
    srcW_ = 0;
    srcH_ = 0;
    LOG_INFO("%s: decoder rebuilt for negotiated %dx%d", kTag, width, height);
}

bool SdlVideoSink::feedAnnexB(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mu_);
    // Tail of the previous session: frames arriving after dropFrame() and before the new session's config frame are all dropped.
    // Return true (as if fed) rather than false -- false makes core gate and wait for a keyframe,
    // and this "frame" should not exist anyway; no need to bother the layer above.
    if (awaitingNewStream_) { ++tailDropped_; return true; }
    if (!dec_.usable()) return false;   // config frame not here yet / could not build -> make core gate

    const bool ok = dec_.decode(data, len, [this](const AVFrame* f) {
        AVFrame* copy = av_frame_alloc();
        if (!copy) return;
        if (av_frame_ref(copy, f) < 0) {
            av_frame_free(&copy);
            return;
        }
        queue_.push_back(copy);
        ++decoded_;
        // Drop the oldest: presentation only cares about "latest"; piling up frames just adds latency
        while (queue_.size() > queueCap_) {
            av_frame_free(&queue_.front());
            queue_.pop_front();
            ++dropped_;
        }
    });

    if (dec_.width() > 0) {
        srcW_ = dec_.width();
        srcH_ = dec_.height();
    }
    return ok;
}

void SdlVideoSink::reset()
{
    // Keyframe recovery: clear the reference chain + drop stale frames not yet presented
    std::lock_guard<std::mutex> lk(mu_);
    dec_.reset();
    while (!queue_.empty()) {
        av_frame_free(&queue_.front());
        queue_.pop_front();
    }
}

void SdlVideoSink::dropFrame()
{
    // Main thread only (tex_/sws_ belong to the main thread; see the threading contract in the header).
    if (tex_) {
        SDL_DestroyTexture(tex_);
        tex_ = nullptr;
    }
    texW_ = texH_ = 0;
    std::lock_guard<std::mutex> lk(mu_);
    size_t cleared = 0;
    while (!queue_.empty()) {
        av_frame_free(&queue_.front());
        queue_.pop_front();
        ++cleared;
    }
    if (cleared) LOG_INFO("%s: back to launcher, dropping %zu frames not yet presented", kTag, cleared);
    srcW_ = srcH_ = 0;   // refilled by configure()/feedAnnexB() for the next session
    awaitingNewStream_ = true;   // until the next session's config frame arrives, all frames are the previous session's tail
    tailDropped_ = 0;
}

bool SdlVideoSink::configured() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return dec_.usable();
}

void SdlVideoSink::intrinsicSize(int* w, int* h) const
{
    std::lock_guard<std::mutex> lk(mu_);
    *w = srcW_;
    *h = srcH_;
}

// ---------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------

const AVFrame* SdlVideoSink::toYuv420p(const AVFrame* f)
{
    if (!sws_ || swsSrcFmt_ != f->format || !scratchFrame_ ||
        scratchFrame_->width != f->width || scratchFrame_->height != f->height) {
        if (sws_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (scratchFrame_) av_frame_free(&scratchFrame_);
        sws_ = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format),
                              f->width, f->height, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return nullptr;
        swsSrcFmt_ = f->format;
        scratchFrame_ = av_frame_alloc();
        if (!scratchFrame_) {
            sws_freeContext(sws_);
            sws_ = nullptr;
            return nullptr;
        }
        scratchFrame_->format = AV_PIX_FMT_YUV420P;
        scratchFrame_->width  = f->width;
        scratchFrame_->height = f->height;
        if (av_frame_get_buffer(scratchFrame_, 32) < 0) {
            av_frame_free(&scratchFrame_);
            sws_freeContext(sws_);
            sws_ = nullptr;
            return nullptr;
        }
        LOG_WARN("%s: pixel format %d has no direct texture path, using sws → YUV420P", kTag, f->format);
    }
    if (av_frame_make_writable(scratchFrame_) < 0) return nullptr;
    sws_scale(sws_, f->data, f->linesize, 0, f->height,
              scratchFrame_->data, scratchFrame_->linesize);
    return scratchFrame_;
}

bool SdlVideoSink::upload(SDL_Renderer* r, const AVFrame* f)
{
    Uint32 fmt = 0;
    switch (f->format) {
        case AV_PIX_FMT_YUV420P: fmt = SDL_PIXELFORMAT_IYUV; break;   // plane order Y,U,V -- matches AVFrame
        case AV_PIX_FMT_NV12:    fmt = SDL_PIXELFORMAT_NV12; break;
        default: break;   // YUVJ420P (full range) etc. go through sws: let sws also normalize the range in passing
    }

    const AVFrame* src = f;
    if (!fmt) {
        src = toYuv420p(f);
        if (!src) return false;
        fmt = SDL_PIXELFORMAT_IYUV;
    }

    if (!tex_ || texW_ != src->width || texH_ != src->height || texFmt_ != fmt) {
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(r, fmt, SDL_TEXTUREACCESS_STREAMING, src->width, src->height);
        texW_ = src->width;
        texH_ = src->height;
        texFmt_ = fmt;
        if (!tex_) {
            LOG_ERROR("%s: failed to create texture (%dx%d): %s", kTag, texW_, texH_, SDL_GetError());
            return false;
        }
        LOG_INFO("%s: texture %dx%d created", kTag, texW_, texH_);
    }

    int rc;
    if (fmt == SDL_PIXELFORMAT_NV12) {
        rc = SDL_UpdateNVTexture(tex_, nullptr, src->data[0], src->linesize[0],
                                 src->data[1], src->linesize[1]);
    } else {
        rc = SDL_UpdateYUVTexture(tex_, nullptr,
                                  src->data[0], src->linesize[0],
                                  src->data[1], src->linesize[1],
                                  src->data[2], src->linesize[2]);
    }
    if (rc != 0) {
        LOG_WARN("%s: texture upload failed: %s", kTag, SDL_GetError());
        return false;
    }
    return true;
}

bool SdlVideoSink::present(SDL_Renderer* r, const SDL_Rect& dst)
{
    AVFrame* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!queue_.empty()) {
            // Keep only the latest frame for presentation: when decode outpaces presentation, intermediate frames have no display value
            while (queue_.size() > 1) {
                av_frame_free(&queue_.front());
                queue_.pop_front();
            }
            f = queue_.front();
            queue_.pop_front();
        }
    }

    // Upload only when there is a new frame; otherwise keep redrawing the one in the texture (the picture holds instead of vanishing)
    if (f) {
        const bool ok = upload(r, f);
        av_frame_free(&f);
        if (!ok) return false;
        ++presented_;
    }

    if (!tex_) return false;   // no frame decoded yet

    const int rc = SDL_RenderCopy(r, tex_, nullptr, &dst);
    if (rc != 0) {
        LOG_WARN("%s: SDL_RenderCopy failed: %s", kTag, SDL_GetError());
        return false;
    }
    return true;
}

}  // namespace allplay
