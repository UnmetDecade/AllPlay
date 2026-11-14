#include "media/ffmpeg_decoder.h"

#include "common/log.h"
#include "media/ffmpeg_compat.h"

extern "C" {
#include <libavutil/mem.h>
}

#include <cstring>

namespace allplay {

namespace {

constexpr const char* kTag = "decode";

/// Append one AnnexB NALU to out (4-byte start code + data)
void appendNalu(std::vector<uint8_t>* out, const uint8_t* d, size_t n)
{
    static const uint8_t kStart[] = {0x00, 0x00, 0x00, 0x01};
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), d, d + n);
}

/// avcC record body -> AnnexB extradata (each SPS/PPS gets a start code).
/// Layout: [0]=1, [1..3]=profile/compat/level, [4]=length-prefix size-1, [5]=low 5 bits = SPS count,
///       then x N { u16BE len + SPS }, then 1 byte PPS count, then x M { u16BE len + PPS }.
/// Modeled on the H.264 branch of Android's MediaCodecSink.buildCsd().
bool avcCToAnnexB(const std::vector<uint8_t>& c, std::vector<uint8_t>* out)
{
    if (c.size() < 7) return false;
    size_t off = 5;
    const int numSps = c[off++] & 0x1f;
    for (int i = 0; i < numSps; ++i) {
        if (off + 2 > c.size()) return false;
        const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
        off += 2;
        if (off + len > c.size()) return false;
        appendNalu(out, c.data() + off, len);
        off += len;
    }
    if (off >= c.size()) return false;
    const int numPps = c[off++];
    for (int i = 0; i < numPps; ++i) {
        if (off + 2 > c.size()) return false;
        const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
        off += 2;
        if (off + len > c.size()) return false;
        appendNalu(out, c.data() + off, len);
        off += len;
    }
    return !out->empty();
}

/// hvcC record body -> AnnexB extradata (VPS/SPS/PPS taken array by array, each with a start code).
/// Layout: ... [21] low 2 bits = length-prefix size-1, [22] = array count,
///       then x N { 1B: type (low 6 bits) + u16BE count + x M { u16BE len + NAL } }.
bool hvcCToAnnexB(const std::vector<uint8_t>& c, std::vector<uint8_t>* out)
{
    if (c.size() < 23) return false;
    size_t off = 22;
    const int numArrays = c[off++];
    for (int a = 0; a < numArrays; ++a) {
        if (off + 3 > c.size()) return false;
        off += 1;   // array header (complete/type); type does not matter: take VPS/SPS/PPS all the same
        const int numNalus = (int(c[off]) << 8) | int(c[off + 1]);
        off += 2;
        for (int i = 0; i < numNalus; ++i) {
            if (off + 2 > c.size()) return false;
            const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
            off += 2;
            if (off + len > c.size()) return false;
            appendNalu(out, c.data() + off, len);
            off += len;
        }
    }
    return !out->empty();
}

}  // namespace

FfmpegVideoDecoder::~FfmpegVideoDecoder() { close(); }

void FfmpegVideoDecoder::close()
{
    if (ctx_) avcodec_free_context(&ctx_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    ctx_ = nullptr;
    pkt_ = nullptr;
    frame_ = nullptr;
}

FfmpegVideoDecoder::Action
FfmpegVideoDecoder::configure(core::VideoCodec codec, const std::vector<uint8_t>& codecData)
{
    // The phone resends the same avcC in response to forceKeyFrame -- do not rebuild when the config is unchanged (otherwise periodic black frames)
    if (ctx_ && codec == codec_ && codecData == codecData_) return Action::Unchanged;

    // Prepare extradata first: on failure do not tear down the existing decoder (make do if possible)
    std::vector<uint8_t> extradata;
    const bool parsed = (codec == core::VideoCodec::HEVC) ? hvcCToAnnexB(codecData, &extradata)
                                                          : avcCToAnnexB(codecData, &extradata);
    if (!parsed) {
        LOG_ERROR("%s: codecData parse failed (%s, %zu bytes), cannot get parameter sets",
                  kTag, codec == core::VideoCodec::HEVC ? "hvcC" : "avcC", codecData.size());
        return Action::Failed;
    }

    const AVCodec* dec = avcodec_find_decoder(codec == core::VideoCodec::HEVC ? AV_CODEC_ID_HEVC
                                                                             : AV_CODEC_ID_H264);
    if (!dec) {
        LOG_ERROR("%s: this ffmpeg build has no %s decoder",
                  kTag, codec == core::VideoCodec::HEVC ? "HEVC" : "H.264");
        return Action::Failed;
    }

    close();
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return Action::Failed;

    // extradata uses AnnexB (with start codes) -- if the first byte is 0x01, the decoder thinks the data is
    // length-prefixed and then decodes all fed AnnexB wrong. Must give a zero-padded buffer with +padding size.
    ctx_->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx_->extradata) {
        close();
        return Action::Failed;
    }
    std::memcpy(ctx_->extradata, extradata.data(), extradata.size());
    ctx_->extradata_size = int(extradata.size());

    // Slice threading only: frame threading delays output by N frames overall (N ~= core count),
    // and touch feedback is very latency-sensitive, so it is not worth it. A single-slice stream naturally degrades to one thread.
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;

    const int ret = avcodec_open2(ctx_, dec, nullptr);
    if (ret < 0) {
        LOG_ERROR("%s: avcodec_open2 failed (%d)", kTag, ret);
        close();
        return Action::Failed;
    }

    pkt_   = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        close();
        return Action::Failed;
    }

    codec_     = codec;
    codecData_ = codecData;
    // The negotiated values are only a hint; the real size is updated once the first frame is decoded
    width_ = 0;
    height_ = 0;

    LOG_INFO("%s: decoder configured %s, extradata %zu bytes (parameter sets converted to AnnexB)",
             kTag, codec == core::VideoCodec::HEVC ? "HEVC" : "H.264", extradata.size());
    return Action::Rebuilt;
}

void FfmpegVideoDecoder::receiveAll(const std::function<void(const AVFrame*)>& onFrame)
{
    while (true) {
        const int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
        if (ret < 0) {
            LOG_WARN("%s: avcodec_receive_frame error (%d)", kTag, ret);
            return;
        }
        if (frame_->width > 0 && frame_->height > 0) {
            width_  = frame_->width;
            height_ = frame_->height;
        }
        onFrame(frame_);
        av_frame_unref(frame_);
    }
}

bool FfmpegVideoDecoder::decode(const uint8_t* data, size_t len,
                                const std::function<void(const AVFrame*)>& onFrame)
{
    if (!ctx_ || !pkt_) return false;

    // Drain the output accumulated in the previous round first, so the send below does not hit EAGAIN
    receiveAll(onFrame);

    av_packet_unref(pkt_);
    if (av_new_packet(pkt_, int(len)) < 0) return false;
    std::memcpy(pkt_->data, data, len);

    const int ret = avcodec_send_packet(ctx_, pkt_);
    if (ret < 0) {
        // EAGAIN: still cannot push after draining a round, so the decoder is backed up -- drop this frame but treat it as non-fatal
        if (ret == AVERROR(EAGAIN)) {
            LOG_WARN("%s: decoder backlogged, dropping one frame (%zu bytes)", kTag, len);
            return true;
        }
        LOG_WARN("%s: avcodec_send_packet error (%d)", kTag, ret);
        return false;
    }

    receiveAll(onFrame);
    return true;
}

void FfmpegVideoDecoder::reset()
{
    if (ctx_) avcodec_flush_buffers(ctx_);
}

}  // namespace allplay
