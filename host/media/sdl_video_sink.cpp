#include "media/sdl_video_sink.h"

#include "common/log.h"

#include <algorithm>

namespace allplay {

namespace {
constexpr const char* kTag = "视频";
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
// core 侧（ScreenStream 线程 / RTSP 线程）
// ---------------------------------------------------------------------------

void SdlVideoSink::configure(core::VideoCodec codec, int width, int height,
                             const std::vector<uint8_t>& codecData)
{
    std::lock_guard<std::mutex> lk(mu_);
    // 这是"下一场会话的第一帧配置"吗？（dropFrame() 之后第一次 configure）
    // 是的话，上一场解出来还没上屏的帧、以及解码器里挂着的参考链都得清掉 ——
    // 否则它们会在新会话刚进 Streaming 时被画出来，用户看到的就是"上一场画面的几帧"。
    const bool sessionStart = awaitingNewStream_;
    awaitingNewStream_ = false;
    if (sessionStart) {
        while (!queue_.empty()) {
            av_frame_free(&queue_.front());
            queue_.pop_front();
        }
        dec_.reset();          // 清参考链（extradata 保留，下面 configure 会重新比对）
        srcW_ = srcH_ = 0;
        if (tailDropped_) {
            LOG_INFO("%s：上一场会话收尾后又来了 %llu 帧，已丢弃",
                     kTag, static_cast<unsigned long long>(tailDropped_));
            tailDropped_ = 0;
        }
    }
    // width/height 只是协商值，真实尺寸看码流；这里只用于日志
    const auto action = dec_.configure(codec, codecData);
    if (action == FfmpegVideoDecoder::Action::Unchanged) {
        LOG_INFO("%s：配置未变（%dx%d），跳过重建", kTag, width, height);
        return;
    }
    if (action == FfmpegVideoDecoder::Action::Failed) {
        LOG_ERROR("%s：解码器配置失败，屏幕流将放闸等关键帧", kTag);
        return;
    }
    // 换了配置：队列里可能还躺着旧解码器的帧，别让它出现在新画面里
    while (!queue_.empty()) {
        av_frame_free(&queue_.front());
        queue_.pop_front();
    }
    srcW_ = 0;
    srcH_ = 0;
    LOG_INFO("%s：已按协商 %dx%d 重建解码器", kTag, width, height);
}

bool SdlVideoSink::feedAnnexB(const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lk(mu_);
    // 上一场会话的尾巴：dropFrame() 之后、新会话配置帧之前到的帧，全部丢掉。
    // 返回 true（当喂进去了）而不是 false —— false 会让 core 放闸等关键帧，
    // 这个"帧"本来就不该存在，没必要惊动上层。
    if (awaitingNewStream_) { ++tailDropped_; return true; }
    if (!dec_.usable()) return false;   // 配置帧还没来 / 建不起来 → 让 core 放闸

    const bool ok = dec_.decode(data, len, [this](const AVFrame* f) {
        AVFrame* copy = av_frame_alloc();
        if (!copy) return;
        if (av_frame_ref(copy, f) < 0) {
            av_frame_free(&copy);
            return;
        }
        queue_.push_back(copy);
        ++decoded_;
        // 丢最旧的：上屏只关心"最新"，堆帧等于加延迟
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
    // 关键帧恢复：清参考链 + 丢掉还没上屏的陈旧帧
    std::lock_guard<std::mutex> lk(mu_);
    dec_.reset();
    while (!queue_.empty()) {
        av_frame_free(&queue_.front());
        queue_.pop_front();
    }
}

void SdlVideoSink::dropFrame()
{
    // 只在主线程调（tex_/sws_ 是主线程独占的，见头文件里的线程约定）。
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
    if (cleared) LOG_INFO("%s：回启动页，丢掉还没上屏的 %zu 帧", kTag, cleared);
    srcW_ = srcH_ = 0;   // 下一场会话由 configure()/feedAnnexB() 重新填
    awaitingNewStream_ = true;   // 直到下一场的配置帧到来，期间的帧都是上一场的尾巴
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
// 主线程
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
        LOG_WARN("%s：像素格式 %d 无直达纹理通道，改走 sws → YUV420P", kTag, f->format);
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
        case AV_PIX_FMT_YUV420P: fmt = SDL_PIXELFORMAT_IYUV; break;   // 平面序 Y,U,V —— 与 AVFrame 一致
        case AV_PIX_FMT_NV12:    fmt = SDL_PIXELFORMAT_NV12; break;
        default: break;   // YUVJ420P（全范围）等走 sws：让 sws 顺手把 range 也归一
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
            LOG_ERROR("%s：建纹理失败（%dx%d）：%s", kTag, texW_, texH_, SDL_GetError());
            return false;
        }
        LOG_INFO("%s：纹理 %dx%d 已建", kTag, texW_, texH_);
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
        LOG_WARN("%s：上传纹理失败：%s", kTag, SDL_GetError());
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
            // 只留最新一帧上屏：解码比上屏快时，中间帧没有展示价值
            while (queue_.size() > 1) {
                av_frame_free(&queue_.front());
                queue_.pop_front();
            }
            f = queue_.front();
            queue_.pop_front();
        }
    }

    // 有新帧才上传；没有就继续用纹理里那一帧重画（画面停住而不是消失）
    if (f) {
        const bool ok = upload(r, f);
        av_frame_free(&f);
        if (!ok) return false;
        ++presented_;
    }

    if (!tex_) return false;   // 一张都没解出来过

    const int rc = SDL_RenderCopy(r, tex_, nullptr, &dst);
    if (rc != 0) {
        LOG_WARN("%s：SDL_RenderCopy 失败：%s", kTag, SDL_GetError());
        return false;
    }
    return true;
}

}  // namespace allplay
