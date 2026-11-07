#include "media/sdl_audio_sink.h"

#include "common/log.h"
#include "media/ffmpeg_compat.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cstring>

namespace allplay {

namespace {

constexpr const char* kTag = "音频";

/// AAC-LC 的 AudioSpecificConfig（2 字节）：5 位 objectType=2(LC) | 4 位采样率索引 | 4 位声道
/// 频率索引表与安卓 AudioSink.kt:151-158 一致 —— 协议固定值，别自己推。
uint16_t aacAsc(int sampleRate, int channels)
{
    int freqIndex;
    switch (sampleRate) {
    case 8000:  freqIndex = 11; break;
    case 16000: freqIndex = 8;  break;
    case 32000: freqIndex = 5;  break;
    case 44100: freqIndex = 4;  break;
    case 48000: freqIndex = 3;  break;
    default:    freqIndex = 4;  break;   // 协议里不会出现别的值
    }
    const int ch = (channels >= 1 && channels <= 7) ? channels : 2;
    return uint16_t((2 << 11) | (freqIndex << 7) | (ch << 3));
}

/// 最小 OpusHead（19 字节）：ffmpeg 的 opus 解码器只要求这一颗头，
/// 真实声道/采样率由包内 TOC 与这里的声道数决定。
std::vector<uint8_t> opusHead(int channels)
{
    std::vector<uint8_t> h(19, 0);
    std::memcpy(h.data(), "OpusHead", 8);
    h[8]  = 1;                                   // version
    h[9]  = uint8_t(channels >= 1 && channels <= 2 ? channels : 2);
    // [10..11] pre-skip = 0，[12..15] input sample rate，[16..17] gain，[18] mapping family
    h[12] = 0x80; h[13] = 0xBB;                  // 48000（LE）—— 只是"信息"，不影响解码
    return h;
}

}  // namespace

SdlAudioSink::~SdlAudioSink()
{
    // 先关设备：SDL_CloseAudioDevice 会等回调跑完，而回调要拿 mu_ —— 所以关之前不能持锁。
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
    closeDecoderLocked();
}

void SdlAudioSink::closeDecoderLocked()
{
    if (dec_) avcodec_free_context(&dec_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    dec_ = nullptr;
    pkt_ = nullptr;
    frame_ = nullptr;
}

bool SdlAudioSink::openDecoderLocked()
{
    AVCodecID id;
    std::vector<uint8_t> extradata;
    switch (cfg_.codec) {
    case core::AudioCodec::AAC_LC: {
        id = AV_CODEC_ID_AAC;
        const uint16_t v = aacAsc(cfg_.rate, cfg_.channels);
        extradata = {uint8_t(v >> 8), uint8_t(v & 0xFF)};
        break;
    }
    case core::AudioCodec::OPUS:
        id = AV_CODEC_ID_OPUS;
        extradata = opusHead(cfg_.channels);
        break;
    case core::AudioCodec::LPCM:
    default:
        return true;   // LPCM 不解码，不需要解码器
    }

    const AVCodec* c = avcodec_find_decoder(id);
    if (!c) {
        LOG_ERROR("%s：本机 ffmpeg 没编入这个解码器（codec=%d）", kTag, int(cfg_.codec));
        return false;
    }
    dec_ = avcodec_alloc_context3(c);
    if (!dec_) return false;

    // extradata 必须带 AV_INPUT_BUFFER_PADDING_SIZE 的零填充，否则解码器可能越界读。
    dec_->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!dec_->extradata) {
        closeDecoderLocked();
        return false;
    }
    std::memcpy(dec_->extradata, extradata.data(), extradata.size());
    dec_->extradata_size = int(extradata.size());

    // ⚠️ 故意**不设** channels/ch_layout：新旧 ffmpeg 的字段名与类型都不同
    //    （见 media/ffmpeg_compat.h），让解码器从码流自己推，我们只读帧里的值。

    const int ret = avcodec_open2(dec_, c, nullptr);
    if (ret < 0) {
        LOG_ERROR("%s：avcodec_open2 失败（%d）", kTag, ret);
        closeDecoderLocked();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        closeDecoderLocked();
        return false;
    }
    LOG_INFO("%s：解码器已配置（codec=%d %dHz %dch，extradata %zu 字节）",
             kTag, int(cfg_.codec), cfg_.rate, cfg_.channels, extradata.size());
    return true;
}

void SdlAudioSink::configure(core::AudioCodec codec, int sampleRate, int channels)
{
    // RTSP 线程 —— 这里**不碰 SDL**，只记待办，由主线程的 serviceDevice() 去开关设备。
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.valid && cfg_.codec == codec && cfg_.rate == sampleRate &&
        cfg_.channels == channels)
        return;   // 同一套协商值重复 SETUP，别把正在放的声音打断

    cfg_.codec    = codec;
    cfg_.rate     = sampleRate > 0 ? sampleRate : 48000;
    cfg_.channels = channels >= 1 ? channels : 2;
    cfg_.valid    = true;
    deviceDirty_  = true;

    outRate_.store(cfg_.rate);
    outChannels_.store(cfg_.channels);

    ring_.assign(kRingSamples * size_t(cfg_.channels), 0);
    head_ = 0;
    size_ = 0;
    startSamples_ = size_t(kStartChunks) * size_t(kChunkFrames) * size_t(cfg_.channels);

    closeDecoderLocked();
    openDecoderLocked();   // 失败也不致命：feed() 会退化成丢包，不会崩

    LOG_INFO("%s：configure codec=%d %dHz %dch", kTag, int(codec), cfg_.rate, cfg_.channels);
}

void SdlAudioSink::reset()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (dec_) avcodec_flush_buffers(dec_);
    head_ = 0;
    size_ = 0;
}

void SdlAudioSink::pushLocked(const int16_t* d, size_t n)
{
    const size_t cap = ring_.size();
    if (cap == 0 || n == 0) return;
    if (n > cap) {                    // 来的一坨比整个缓冲还大：只留最新的
        d += (n - cap);
        n = cap;
    }
    bool over = false;
    while (size_ + n > cap) {         // 丢最旧 —— 堆缓冲等于加延迟
        head_ = (head_ + 1) % cap;
        --size_;
        over = true;
    }
    size_t tail = (head_ + size_) % cap;
    for (size_t i = 0; i < n; ++i) {
        ring_[tail] = d[i];
        tail = (tail + 1) % cap;
    }
    size_ += n;
    if (over) dropped_.fetch_add(1);
}

void SdlAudioSink::appendFrameLocked(const AVFrame* f)
{
    if (!f || f->nb_samples <= 0) return;
    const int ch = frameChannels(f);
    if (ch <= 0) return;

    const int n = f->nb_samples;
    scratch_.resize(size_t(n) * size_t(ch));
    int16_t* out = scratch_.data();
    const AVSampleFormat fmt = AVSampleFormat(f->format);

    auto f2s = [](float v) -> int16_t {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        return int16_t(v * 32767.0f);
    };

    switch (fmt) {
    case AV_SAMPLE_FMT_FLTP:   // 平面 float（AAC/Opus 软解的常规输出）
        for (int c = 0; c < ch; ++c) {
            const float* src = reinterpret_cast<const float*>(f->data[c]);
            if (!src) continue;
            for (int i = 0; i < n; ++i) out[size_t(i) * ch + c] = f2s(src[i]);
        }
        break;
    case AV_SAMPLE_FMT_FLT: {  // 交织 float
        const float* src = reinterpret_cast<const float*>(f->data[0]);
        for (size_t i = 0; i < size_t(n) * ch; ++i) out[i] = f2s(src[i]);
        break;
    }
    case AV_SAMPLE_FMT_S16P:   // 平面 s16
        for (int c = 0; c < ch; ++c) {
            const int16_t* src = reinterpret_cast<const int16_t*>(f->data[c]);
            if (!src) continue;
            for (int i = 0; i < n; ++i) out[size_t(i) * ch + c] = src[i];
        }
        break;
    case AV_SAMPLE_FMT_S16:    // 交织 s16，直接用
        pushLocked(reinterpret_cast<const int16_t*>(f->data[0]), size_t(n) * ch);
        return;
    default:
        // 真出现别的格式再补 —— 记一次日志免得静默丢声音
        LOG_WARN("%s：不支持的样本格式 %d，丢弃", kTag, int(fmt));
        return;
    }
    pushLocked(out, size_t(n) * ch);
}

void SdlAudioSink::decodeLocked(const uint8_t* payload, size_t len)
{
    if (!dec_ || !pkt_ || !frame_) return;

    // 先把上一轮攒的输出取干净，免得下面 send 撞 EAGAIN
    while (avcodec_receive_frame(dec_, frame_) == 0) {
        appendFrameLocked(frame_);
        av_frame_unref(frame_);
    }

    av_packet_unref(pkt_);
    if (av_new_packet(pkt_, int(len)) < 0) return;
    std::memcpy(pkt_->data, payload, len);

    const int ret = avcodec_send_packet(dec_, pkt_);
    if (ret < 0) return;

    while (avcodec_receive_frame(dec_, frame_) == 0) {
        appendFrameLocked(frame_);
        av_frame_unref(frame_);
    }
}

void SdlAudioSink::feed(const uint8_t* payload, size_t len, uint32_t sample)
{
    (void)sample;   // core 已经按 RTP 序号重排好了，时间戳这里用不上
    if (!payload || len == 0) return;

    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || !cfg_.valid) return;

    if (cfg_.codec == core::AudioCodec::LPCM) {
        // 裸 PCM：AirPlay 的 LPCM 是**大端** s16（网络序），SDL 要的是主机序。
        // 真机实测：不换字节、直接按小端喂进 SDL，出来是满耳噪音（每个样本的高低
        // 字节颠倒，波形成了类噪声）。这里每个 16 位样本换一下字节；奇数尾巴丢掉。
        const size_t n = len / sizeof(int16_t);
        if (n == 0) return;
        pcmSwap_.resize(n);
        const uint8_t* p = payload;
        for (size_t i = 0; i < n; ++i)
            pcmSwap_[i] = int16_t((uint16_t(p[2 * i]) << 8) | uint16_t(p[2 * i + 1]));
        pushLocked(pcmSwap_.data(), n);
        decoded_.fetch_add(1);
        return;
    }

    // AAC 的配置小包（≤12 字节）不是音频帧 —— 喂进去解码器会一路报错
    if (cfg_.codec == core::AudioCodec::AAC_LC && len <= 12) return;

    decodeLocked(payload, len);
    decoded_.fetch_add(1);
}

void SdlAudioSink::sdlCallback(void* user, Uint8* stream, int len)
{
    auto* self = static_cast<SdlAudioSink*>(user);
    std::memset(stream, 0, size_t(len));   // 拿不出数据时放静音，别放上一轮的残留

    const size_t want = size_t(len) / sizeof(int16_t);
    std::lock_guard<std::mutex> lk(self->mu_);
    if (self->stop_ || self->size_ == 0) {
        if (!self->stop_) self->underruns_.fetch_add(1);
        return;
    }
    const size_t got = std::min(want, self->size_);
    const size_t cap = self->ring_.size();
    auto* dst = reinterpret_cast<int16_t*>(stream);
    for (size_t i = 0; i < got; ++i)
        dst[i] = self->ring_[(self->head_ + i) % cap];
    self->head_ = (self->head_ + got) % cap;
    self->size_ -= got;
    if (got < want) self->underruns_.fetch_add(1);
}

void SdlAudioSink::serviceDevice()
{
    // 先把"要不要动设备"问清楚，**然后放锁** —— SDL_CloseAudioDevice 会等回调结束，
    // 而回调要拿 mu_，持锁去关就是死锁。
    Config want;
    bool   reopen = false, unpause = false;
    size_t startSamples = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (deviceDirty_) {
            deviceDirty_ = false;
            want         = cfg_;
            startSamples = startSamples_;   // 快照：下面打日志时已经放锁了
            reopen       = true;
        } else if (dev_ && !playing_ && size_ >= startSamples_) {
            playing_ = true;          // 状态在锁里定，SDL 调用放锁外
            unpause  = true;
        }
    }
    if (unpause) {
        SDL_PauseAudioDevice(dev_, 0);   // 垫底够了，放行
        return;
    }
    if (!reopen) return;

    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    playing_ = false;
    if (!want.valid) return;

    SDL_AudioSpec spec{};
    spec.freq     = want.rate;
    spec.format   = AUDIO_S16SYS;
    spec.channels = Uint8(want.channels);
    spec.samples  = Uint16(kChunkFrames);
    spec.callback = &SdlAudioSink::sdlCallback;
    spec.userdata = this;

    // 不设 SDL_AUDIO_ALLOW_ANY_CHANGE：我们要的就是 44100/48000 × 1/2ch，SDL 都原生支持。
    // 允许它改反而会让我们按码流算好的交织布局对不上设备。
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    if (!dev_) {
        LOG_ERROR("%s：SDL_OpenAudioDevice 失败：%s", kTag, SDL_GetError());
        return;
    }
    // **故意先不启播**（SDL 开出来的设备默认就是暂停的）：
    // 等环形缓冲攒到 startSamples_ 再由上面的分支放行 —— 见头文件里 kStartChunks 的说明。
    LOG_INFO("%s：SDL 音频设备已开（%dHz %dch），等缓冲垫到 %zu 样本再启播",
             kTag, want.rate, want.channels, startSamples);
}

}  // namespace allplay
