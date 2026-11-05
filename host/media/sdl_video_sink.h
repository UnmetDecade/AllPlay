/**
 * @file   sdl_video_sink.h
 * @brief  core::IVideoOutput 的桌面实现：ffmpeg 软解 + SDL 纹理上屏
 * @author blt
 */

// ============================================================================
// host/media/sdl_video_sink.h —— IVideoOutput：ffmpeg 软解 + SDL 纹理上屏
// ============================================================================
// 线程（由 core 的调用点决定，必须照做）：
//   · configure() / feedAnnexB() —— ScreenStream 线程（core/airplay/video_stream.cpp）
//   · reset()                    —— **RTSP 线程**（手机发 forceKeyFrame 触发）
//   · present()                  —— 主线程（SDL 渲染）
// 所以解码器与帧队列用一把锁串起来，SDL 纹理只在主线程碰。
//
// 帧队列故意很浅（默认 3）：解码结果比"上屏进度"新才有意义，堆帧只会加延迟。
// 队列满了丢**最旧**的，不是丢新的 —— 且照样返回 true，避免让 core 误以为入队失败
// 而重新放闸 + 向手机要关键帧（那是很贵的往返）。
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

/// 把 srcW×srcH 等比放进 dstW×dstH，居中（letterbox）。触摸归一化也用这个矩形。
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
    void flush() override {}   // core 从不调用

    // ---- 主线程 ----
    /// 取最新一帧画到 dst（由 fitLetterbox 算出的矩形）。
    /// 没有新帧时会用纹理里那一帧重画（画面停住而不是消失）。false = 无帧可画。
    bool present(SDL_Renderer* r, const SDL_Rect& dst);

    /// 是否有过至少一帧（决定要不要切成视频面）
    bool hasFrame() const { return tex_ != nullptr; }

    /// 丢掉当前这一帧（**只在主线程调**）。会话结束时回启动页要用：
    /// 否则纹理里那张末帧会被 present() 一直重画，屏上卡着最后一帧回不去
    /// ——日志里明明打了"回启动页"，画面却纹丝不动。
    ///
    /// 同时把 sink 标成"等下一场"：会话收尾时视频线程还可能再吐几帧进来
    /// （dropFrame 清完队列之后才到的），留着它们，下一场会话一进 Streaming
    /// 就会先把**上一场的画面**画出来几帧再换新画面。见 feedAnnexB()。
    void dropFrame();

    /// 视频固有尺寸（0 = 还没解出过帧）。主线程读，用于算 letterbox。
    void intrinsicSize(int* w, int* h) const;

    /// 已上屏帧数（主线程读）
    uint64_t presented() const { return presented_; }

    /// 解码器吐出的帧数、以及被浅队列挤掉的帧数（喂帧线程写、任意线程读）。
    /// 上屏数会小于解码数：渲染慢时队列按设计丢最旧的。做性能判定要看**解码**，
    /// 看上屏数会把"渲染慢"误判成"解码慢"。
    uint64_t decoded() const { return decoded_.load(); }
    uint64_t dropped() const { return dropped_.load(); }

    /// 是否已经配置过解码器（配置帧来过）
    bool configured() const;

private:
    /// 把 f 传上纹理（必要时重建纹理/做格式回落）。只在主线程调用。
    bool upload(SDL_Renderer* r, const AVFrame* f);
    /// 非 YUV420P/NV12 的格式经 sws 转成 YUV420P 到 scratch_。
    const AVFrame* toYuv420p(const AVFrame* f);

    mutable std::mutex   mu_;
    FfmpegVideoDecoder   dec_;              ///< 受 mu_
    std::deque<AVFrame*> queue_;            ///< 待上屏（受 mu_）
    size_t               queueCap_ = 3;
    int                  srcW_ = 0, srcH_ = 0;
    /// dropFrame() 之后、下一场会话的配置帧到来之前为 true：这期间喂进来的帧
    /// 都是上一场的尾巴，一律丢掉（受 mu_）。
    bool                 awaitingNewStream_ = false;
    /// 上面那段"滴答期"里丢掉的帧数（受 mu_）：下一场 configure() 时报一次，
    /// 用来证明这块保护真的在起作用，而不是碰巧没帧到达。
    uint64_t             tailDropped_ = 0;

    // —— 以下只在主线程碰 ——
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
