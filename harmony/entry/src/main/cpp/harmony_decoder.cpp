/**
 * @file   harmony_decoder.cpp
 * @brief  OHOS AVCodec 硬解 + surface 渲染
 * @author blt
 */

#include "harmony_decoder.h"

#include "common/log.h"

#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_averrors.h>
#include <native_window/external_window.h>

#include <cstring>
#include <string>

namespace allplay::harmony {

namespace {

// avcC（ISO BMFF 的 AVCDecoderConfigurationRecord）→ AnnexB 的 SPS+PPS。
// OH_MD_KEY_CODEC_CONFIG 对 H.264 要 AnnexB；把 avcC 原样塞进去解不出画面。
std::vector<uint8_t> avcCToAnnexBExtradata(const std::vector<uint8_t>& c)
{
    std::vector<uint8_t> out;
    if (c.size() < 7 || c[0] != 1) return out;   // configurationVersion
    size_t i = 5;
    const int numSps = c[i++] & 0x1f;
    auto append = [&](const uint8_t* p, size_t n) {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), p, p + n);
    };
    for (int s = 0; s < numSps; ++s) {
        if (i + 2 > c.size()) return {};
        const size_t len = (size_t(c[i]) << 8) | c[i + 1];
        i += 2;
        if (i + len > c.size()) return {};
        append(c.data() + i, len);
        i += len;
    }
    if (i >= c.size()) return {};
    const int numPps = c[i++];
    for (int p = 0; p < numPps; ++p) {
        if (i + 2 > c.size()) return {};
        const size_t len = (size_t(c[i]) << 8) | c[i + 1];
        i += 2;
        if (i + len > c.size()) return {};
        append(c.data() + i, len);
        i += len;
    }
    return out;
}

}  // namespace

OhosVideoDecoder::~OhosVideoDecoder() { close(); }

void OhosVideoDecoder::setSurface(uint64_t surfaceId)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (surfaceId_ == surfaceId) return;
    LOG_INFO("[dec] surface id=%llu", (unsigned long long)surfaceId);
    surfaceId_ = surfaceId;

    // surface 换了（横竖屏切换会重建 XComponent）：旧 OHNativeWindow 指向的
    // surface 已经被系统回收，继续往上画要么黑屏要么直接报错。拆掉重建；
    // 新 surface 一到就用上次的 csd 重新建解码器，不用等手机重发配置。
    if (codec_ != nullptr) closeLocked();
    if (surfaceId != 0 && !csd_.empty()) {
        configureLocked(codecKind_, width_, height_, csd_);
    }
}

bool OhosVideoDecoder::ready() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return running_ && surfaceId_ != 0;
}

void OhosVideoDecoder::closeLocked()
{
    if (codec_) {
        if (running_) OH_VideoDecoder_Stop(codec_);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
    running_ = false;
    if (window_) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
    }
    freeInput_.clear();
    pending_.clear();
    ptsUs_ = 0;
}

void OhosVideoDecoder::close()
{
    std::lock_guard<std::mutex> lk(mu_);
    closeLocked();
}

bool OhosVideoDecoder::configure(int codecKind, int width, int height,
                                 const std::vector<uint8_t>& csd)
{
    std::lock_guard<std::mutex> lk(mu_);
    return configureLocked(codecKind, width, height, csd);
}

bool OhosVideoDecoder::configureLocked(int codecKind, int width, int height,
                                       const std::vector<uint8_t>& csd)
{
    // 手机每隔几秒会重发一份完全相同的 avcC（真机实测）。一样就什么都不做 ——
    // 重建解码器会把已解好的参考链清掉，画面每次都要等一个新 IDR 才回来。
    if (running_ && codec_ != nullptr && codecKind_ == codecKind &&
        width_ == width && height_ == height && csd == csd_) {
        LOG_INFO("[dec] 配置未变，跳过重建");
        return true;
    }
    closeLocked();

    csd_ = csd;
    codecKind_ = codecKind;
    width_ = width;
    height_ = height;

    if (surfaceId_ == 0) {
        LOG_WARN("[dec] 还没有 surface（XComponent 没起来），等它到了再建");
        return false;
    }

    const char* mime = codecKind == 1 ? "video/hevc" : "video/avc";
    OH_AVCodec* c = OH_VideoDecoder_CreateByMime(mime);
    if (c == nullptr) {
        LOG_ERROR("[dec] CreateByMime(%s) 失败", mime);
        return false;
    }
    codec_ = c;

    OH_AVCodecCallback cb{};
    cb.onError            = &OhosVideoDecoder::OnError;
    cb.onStreamChanged    = &OhosVideoDecoder::OnStreamChanged;
    cb.onNeedInputBuffer  = &OhosVideoDecoder::OnNeedInputBuffer;
    cb.onNewOutputBuffer  = &OhosVideoDecoder::OnNewOutputBuffer;
    if (OH_VideoDecoder_RegisterCallback(codec_, cb, this) != AV_ERR_OK) {
        LOG_ERROR("[dec] RegisterCallback 失败");
        closeLocked();
        return false;
    }

    OH_AVFormat* fmt = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height);
    if (codecKind == 0) {
        const auto extradata = avcCToAnnexBExtradata(csd);
        if (!extradata.empty()) {
            OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG,
                                  extradata.data(), int32_t(extradata.size()));
            extradata_ = extradata;   // 下面每个 IDR 也会补一份（硬解实测只认 in-band）
            LOG_INFO("[dec] extradata：avcC %zu 字节 → AnnexB SPS/PPS %zu 字节",
                     csd.size(), extradata.size());
        } else {
            LOG_WARN("[dec] avcC 解析失败（%zu 字节），没有 SPS/PPS 解不出画面", csd.size());
        }
    }

    OH_AVErrCode e = OH_VideoDecoder_Configure(codec_, fmt);
    OH_AVFormat_Destroy(fmt);
    if (e != AV_ERR_OK) {
        LOG_ERROR("[dec] Configure 失败 code=%d", int(e));
        closeLocked();
        return false;
    }

    OHNativeWindow* win = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId_, &win) != 0 ||
        win == nullptr) {
        LOG_ERROR("[dec] 从 surfaceId=%llu 建 NativeWindow 失败",
                  (unsigned long long)surfaceId_);
        closeLocked();
        return false;
    }
    window_ = win;

    if ((e = OH_VideoDecoder_SetSurface(codec_, window_)) != AV_ERR_OK) {
        LOG_ERROR("[dec] SetSurface 失败 code=%d", int(e));
        closeLocked();
        return false;
    }
    if ((e = OH_VideoDecoder_Prepare(codec_)) != AV_ERR_OK) {
        LOG_ERROR("[dec] Prepare 失败 code=%d", int(e));
        closeLocked();
        return false;
    }
    if ((e = OH_VideoDecoder_Start(codec_)) != AV_ERR_OK) {
        LOG_ERROR("[dec] Start 失败 code=%d", int(e));
        closeLocked();
        return false;
    }

    running_ = true;
    configFails_ = 0;
    LOG_INFO("[dec] 解码器就绪：%s %dx%d", mime, width, height);
    return true;
}

bool OhosVideoDecoder::pushLocked(OH_AVBuffer* buffer, uint32_t index,
                                  const uint8_t* data, size_t len, bool key)
{
    if (buffer == nullptr) return false;
    uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
    const int32_t cap = OH_AVBuffer_GetCapacity(buffer);
    if (addr == nullptr || cap <= 0) return false;

    // 参数集必须 in-band：OH_MD_KEY_CODEC_CONFIG 这条路在这台机上没进解码器
    // （系统日志：pps is null / EVNT_PARAMETER_SETS_LOST），每个 IDR 前面补 SPS+PPS。
    const bool withCsd = key && !extradata_.empty();
    const size_t total = len + (withCsd ? extradata_.size() : 0);
    if (size_t(cap) < total) return false;
    if (withCsd) {
        std::memcpy(addr, extradata_.data(), extradata_.size());
        std::memcpy(addr + extradata_.size(), data, len);
    } else {
        std::memcpy(addr, data, len);
    }
    OH_AVCodecBufferAttr attr{};
    attr.pts    = ptsUs_;
    attr.size   = int32_t(total);
    attr.offset = 0;
    attr.flags  = key ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);
    ptsUs_ += 16666;   // 60fps；解码器只用它排序/计时

    const OH_AVErrCode e = OH_VideoDecoder_PushInputBuffer(codec_, index);
    if (pushLogs_ < 5) {
        ++pushLogs_;
        LOG_INFO("[dec] push #%d：%zu 字节 key=%d（含 CSD %d）→ %d", pushLogs_, len,
                 int(key), int(withCsd), int(e));
    }
    return e == AV_ERR_OK;
}

void OhosVideoDecoder::flushPendingLocked()
{
    while (!pending_.empty() && !freeInput_.empty()) {
        auto [idx, buf] = freeInput_.front();
        freeInput_.pop_front();
        Frame& f = pending_.front();
        if (!pushLocked(buf, idx, f.data.data(), f.data.size(), f.key)) return;
        pending_.pop_front();
    }
}

bool OhosVideoDecoder::feed(const uint8_t* data, size_t len, bool keyframe)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_ || codec_ == nullptr) return false;

    // 解码器跟不上就丢帧：让 core 重新放闸等 IDR，比堆到 OOM 好
    if (pending_.size() >= 16) return false;

    if (freeInput_.empty()) {
        pending_.push_back(Frame{std::vector<uint8_t>(data, data + len), keyframe, ptsUs_});
        return true;
    }
    auto [idx, buf] = freeInput_.front();
    freeInput_.pop_front();
    return pushLocked(buf, idx, data, len, keyframe);
}

void OhosVideoDecoder::reset()
{
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();
    freeInput_.clear();
    ptsUs_ = 0;
    if (running_ && codec_) OH_VideoDecoder_Flush(codec_);
    LOG_INFO("[dec] reset（等新 IDR）");
}

// ---------------------------------------------------------------------------
// AVCodec 回调
// ---------------------------------------------------------------------------
void OhosVideoDecoder::OnError(OH_AVCodec* /*codec*/, int32_t errorCode, void* /*userData*/)
{
    LOG_ERROR("[dec] 解码器报错 code=%d", int(errorCode));
}

void OhosVideoDecoder::OnStreamChanged(OH_AVCodec* /*codec*/, OH_AVFormat* format,
                                       void* /*userData*/)
{
    int32_t w = 0, h = 0;
    if (format && OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w) &&
        OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h)) {
        LOG_INFO("[dec] 码流尺寸变化：%dx%d", w, h);
    }
}

void OhosVideoDecoder::OnNeedInputBuffer(OH_AVCodec* /*codec*/, uint32_t index,
                                         OH_AVBuffer* buffer, void* userData)
{
    auto* self = static_cast<OhosVideoDecoder*>(userData);
    std::lock_guard<std::mutex> lk(self->mu_);
    if (self->inputCbCount_ < 5) {
        ++self->inputCbCount_;
        LOG_INFO("[dec] 输入 buffer 可用 #%d（index=%u，待发 %zu 帧）",
                 self->inputCbCount_, index, self->pending_.size());
    }
    self->freeInput_.emplace_back(index, buffer);
    self->flushPendingLocked();
}

void OhosVideoDecoder::OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index,
                                         OH_AVBuffer* /*buffer*/, void* userData)
{
    auto* self = static_cast<OhosVideoDecoder*>(userData);
    // surface 模式：RenderOutputBuffer 把这一帧画到 XComponent 的 surface 上并释放 buffer
    if (OH_VideoDecoder_RenderOutputBuffer(codec, index) != AV_ERR_OK) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    const uint64_t n = self->rendered_.fetch_add(1) + 1;
    if (n <= 3 || n % 300 == 0) LOG_INFO("[dec] 已上屏 %llu 帧", (unsigned long long)n);
}

}  // namespace allplay::harmony
