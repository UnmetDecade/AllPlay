/**
 * @file   harmony_decoder.h
 * @brief  H.264/HEVC 硬解 + 直接渲染到 XComponent 的 surface（OHOS AVCodec NDK）
 * @author blt
 */

// ============================================================================
// harmony_decoder.h —— OHOS 原生硬解
// ============================================================================
// 为什么用 NDK 而不是 ArkTS 的 VideoDecoder：这套 SDK（HarmonyOS 26 / API 26）的
// ArkTS 侧没有 media.VideoDecoder 声明，只有 NDK 的 native_avcodec_videodecoder.h。
// 路径：avcC（ISO BMFF 记录）→ AnnexB 的 SPS/PPS 当 extradata → 解码 → surface。
//
// 线程：configure()/feed() 来自 ScreenStream 线程；setSurface() 来自 ArkTS 主线程；
// 解码器回调来自 AVCodec 自己的线程。三边都过 mu_。
#pragma once

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

struct OH_AVCodec;

namespace allplay::harmony {

class OhosVideoDecoder {
public:
    ~OhosVideoDecoder();
    OhosVideoDecoder() = default;
    OhosVideoDecoder(const OhosVideoDecoder&) = delete;
    OhosVideoDecoder& operator=(const OhosVideoDecoder&) = delete;

    /// XComponent 的 surface id（ArkTS 的 getXComponentSurfaceId()）。0 = 还没有 surface。
    /// surface 到位前 feed() 一律返回 false —— core 会重新放闸等 IDR，正好避开黑屏。
    void setSurface(uint64_t surfaceId);

    /// 用 avcC/hvcC 记录重建解码器。返回 false 表示建不起来（此时保持"未就绪"）。
    bool configure(int codec /*0=H264,1=HEVC*/, int width, int height,
                   const std::vector<uint8_t>& csd);

    /// 喂一段 AnnexB（00 00 00 01 开头，可能多个 NALU）。未就绪返回 false。
    bool feed(const uint8_t* data, size_t len, bool keyframe);

    /// 清参考链（重新等 IDR，由 core 的闸门接手）。
    void reset();

    void close();
    bool ready() const;
    uint64_t rendered() const { return rendered_.load(); }

private:
    void closeLocked();
    bool configureLocked(int codec, int width, int height, const std::vector<uint8_t>& csd);
    bool pushLocked(OH_AVBuffer* buffer, uint32_t index, const uint8_t* data, size_t len, bool key);
    void flushPendingLocked();

    static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
    static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
    static void OnNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);
    static void OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);

    mutable std::mutex mu_;
    OH_AVCodec*     codec_ = nullptr;
    OHNativeWindow* window_ = nullptr;
    uint64_t        surfaceId_ = 0;
    int             width_ = 0, height_ = 0;
    int             codecKind_ = 0;      // 0 = H.264，1 = HEVC
    bool            running_ = false;
    int64_t         ptsUs_ = 0;
    int             configFails_ = 0;    // surface 还没到时不要每帧都打日志
    std::vector<uint8_t> csd_;         // 上次的 avcC/hvcC（用来识别"重发同一份配置"）
    std::vector<uint8_t> extradata_;   // AnnexB 的 SPS+PPS：每个 IDR 前面补一份
    int             pushLogs_ = 0;
    int             inputCbCount_ = 0;

    std::deque<std::pair<uint32_t, OH_AVBuffer*>> freeInput_;   // (下标, buffer)
    struct Frame { std::vector<uint8_t> data; bool key; int64_t pts; };
    std::deque<Frame>      pending_;     // 等输入 buffer 的帧（最多 16）
    std::atomic<uint64_t>  rendered_{0};
};

}  // namespace allplay::harmony
