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

constexpr const char* kTag = "audio";

/// AAC-LC AudioSpecificConfig (2 bytes): 5-bit objectType=2 (LC) | 4-bit sample-rate index | 4-bit channels
/// The frequency index table matches Android AudioSink.kt:151-158 -- these are fixed protocol values, do not derive them yourself.
uint16_t aacAsc(int sampleRate, int channels)
{
    int freqIndex;
    switch (sampleRate) {
    case 8000:  freqIndex = 11; break;
    case 16000: freqIndex = 8;  break;
    case 32000: freqIndex = 5;  break;
    case 44100: freqIndex = 4;  break;
    case 48000: freqIndex = 3;  break;
    default:    freqIndex = 4;  break;   // no other value occurs in the protocol
    }
    const int ch = (channels >= 1 && channels <= 7) ? channels : 2;
    return uint16_t((2 << 11) | (freqIndex << 7) | (ch << 3));
}

/// Minimal OpusHead (19 bytes): ffmpeg's opus decoder only requires this header,
/// with the real channels/sample rate determined by the in-packet TOC and the channel count here.
std::vector<uint8_t> opusHead(int channels)
{
    std::vector<uint8_t> h(19, 0);
    std::memcpy(h.data(), "OpusHead", 8);
    h[8]  = 1;                                   // version
    h[9]  = uint8_t(channels >= 1 && channels <= 2 ? channels : 2);
    // [10..11] pre-skip = 0, [12..15] input sample rate, [16..17] gain, [18] mapping family
    h[12] = 0x80; h[13] = 0xBB;                  // 48000 (LE) -- informational only, does not affect decoding
    return h;
}

}  // namespace

SdlAudioSink::~SdlAudioSink()
{
    // Close the device first: SDL_CloseAudioDevice waits for the callback to finish, and the callback takes mu_ -- so do not hold the lock while closing.
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
        return true;   // LPCM is not decoded, no decoder needed
    }

    const AVCodec* c = avcodec_find_decoder(id);
    if (!c) {
        LOG_ERROR("%s: this ffmpeg build has no such decoder (codec=%d)", kTag, int(cfg_.codec));
        return false;
    }
    dec_ = avcodec_alloc_context3(c);
    if (!dec_) return false;

    // extradata must be zero-padded by AV_INPUT_BUFFER_PADDING_SIZE, otherwise the decoder may read out of bounds.
    dec_->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!dec_->extradata) {
        closeDecoderLocked();
        return false;
    }
    std::memcpy(dec_->extradata, extradata.data(), extradata.size());
    dec_->extradata_size = int(extradata.size());

    // ⚠️ Deliberately **do not set** channels/ch_layout: old and new ffmpeg differ in field name and type
    //    (see media/ffmpeg_compat.h); let the decoder infer them from the bitstream and read the values from frames.

    const int ret = avcodec_open2(dec_, c, nullptr);
    if (ret < 0) {
        LOG_ERROR("%s: avcodec_open2 failed (%d)", kTag, ret);
        closeDecoderLocked();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        closeDecoderLocked();
        return false;
    }
    LOG_INFO("%s: decoder configured (codec=%d %dHz %dch, extradata %zu bytes)",
             kTag, int(cfg_.codec), cfg_.rate, cfg_.channels, extradata.size());
    return true;
}

void SdlAudioSink::configure(core::AudioCodec codec, int sampleRate, int channels)
{
    // RTSP thread -- **no SDL here**; just record the pending change, and the main thread's serviceDevice() opens/closes the device.
    std::lock_guard<std::mutex> lk(mu_);
    if (cfg_.valid && cfg_.codec == codec && cfg_.rate == sampleRate &&
        cfg_.channels == channels)
        return;   // same negotiated values on a repeated SETUP; do not interrupt audio that is playing

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
    openDecoderLocked();   // failure is not fatal: feed() degrades to dropping packets, no crash

    LOG_INFO("%s: configure codec=%d %dHz %dch", kTag, int(codec), cfg_.rate, cfg_.channels);
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
    if (n > cap) {                    // incoming block larger than the whole buffer: keep only the newest
        d += (n - cap);
        n = cap;
    }
    bool over = false;
    while (size_ + n > cap) {         // drop the oldest -- extra buffering just adds latency
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
    case AV_SAMPLE_FMT_FLTP:   // planar float (the usual output of AAC/Opus software decoding)
        for (int c = 0; c < ch; ++c) {
            const float* src = reinterpret_cast<const float*>(f->data[c]);
            if (!src) continue;
            for (int i = 0; i < n; ++i) out[size_t(i) * ch + c] = f2s(src[i]);
        }
        break;
    case AV_SAMPLE_FMT_FLT: {  // interleaved float
        const float* src = reinterpret_cast<const float*>(f->data[0]);
        for (size_t i = 0; i < size_t(n) * ch; ++i) out[i] = f2s(src[i]);
        break;
    }
    case AV_SAMPLE_FMT_S16P:   // planar s16
        for (int c = 0; c < ch; ++c) {
            const int16_t* src = reinterpret_cast<const int16_t*>(f->data[c]);
            if (!src) continue;
            for (int i = 0; i < n; ++i) out[size_t(i) * ch + c] = src[i];
        }
        break;
    case AV_SAMPLE_FMT_S16:    // interleaved s16, use directly
        pushLocked(reinterpret_cast<const int16_t*>(f->data[0]), size_t(n) * ch);
        return;
    default:
        // Add support if other formats actually show up -- log once so audio is not dropped silently
        LOG_WARN("%s: unsupported sample format %d, dropping", kTag, int(fmt));
        return;
    }
    pushLocked(out, size_t(n) * ch);
}

void SdlAudioSink::decodeLocked(const uint8_t* payload, size_t len)
{
    if (!dec_ || !pkt_ || !frame_) return;

    // Drain the output accumulated in the previous round first, so the send below does not hit EAGAIN
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
    (void)sample;   // core has already reordered by RTP sequence; the timestamp is not needed here
    if (!payload || len == 0) return;

    std::lock_guard<std::mutex> lk(mu_);
    if (stop_ || !cfg_.valid) return;

    if (cfg_.codec == core::AudioCodec::LPCM) {
        // Raw PCM: AirPlay's LPCM is **big-endian** s16 (network order), while SDL wants host order.
        // Measured on hardware: feeding it to SDL as little-endian without swapping bytes yields pure noise
        // (each sample's high and low bytes are reversed, so the waveform becomes noise). Here each 16-bit sample is byte-swapped; an odd trailing byte is dropped.
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

    // AAC config packets (<=12 bytes) are not audio frames -- feeding them makes the decoder error out repeatedly
    if (cfg_.codec == core::AudioCodec::AAC_LC && len <= 12) return;

    decodeLocked(payload, len);
    decoded_.fetch_add(1);
}

void SdlAudioSink::sdlCallback(void* user, Uint8* stream, int len)
{
    auto* self = static_cast<SdlAudioSink*>(user);
    std::memset(stream, 0, size_t(len));   // play silence when there is no data; do not leave the previous round's leftovers

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
    // Decide "whether to touch the device" first, **then release the lock** -- SDL_CloseAudioDevice waits for the callback to finish,
    // and the callback takes mu_; closing while holding the lock deadlocks.
    Config want;
    bool   reopen = false, unpause = false;
    size_t startSamples = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (deviceDirty_) {
            deviceDirty_ = false;
            want         = cfg_;
            startSamples = startSamples_;   // snapshot: the lock is already released by the time we log below
            reopen       = true;
        } else if (dev_ && !playing_ && size_ >= startSamples_) {
            playing_ = true;          // state decided under the lock, SDL calls outside it
            unpause  = true;
        }
    }
    if (unpause) {
        SDL_PauseAudioDevice(dev_, 0);   // enough has accumulated, let it play
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

    // Do not set SDL_AUDIO_ALLOW_ANY_CHANGE: we want exactly 44100/48000 x 1/2ch, and SDL supports both natively.
    // Letting it change these would make the interleaving layout computed from the bitstream mismatch the device.
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    if (!dev_) {
        LOG_ERROR("%s: SDL_OpenAudioDevice failed: %s", kTag, SDL_GetError());
        return;
    }
    // **Deliberately do not start playback yet** (a device opened by SDL is paused by default):
    // wait until the ring buffer reaches startSamples_, then the branch above releases it -- see the kStartChunks notes in the header.
    LOG_INFO("%s: SDL audio device opened (%dHz %dch), waiting for %zu samples of buffer before starting playback",
             kTag, want.rate, want.channels, startSamples);
}

}  // namespace allplay
