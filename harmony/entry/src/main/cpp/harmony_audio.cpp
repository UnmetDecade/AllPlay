/**
 * @file   harmony_audio.cpp
 * @brief  OH AudioCodec 解码 + OHAudio 渲染
 * @author blt
 */

#include "harmony_audio.h"

#include "common/log.h"

#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcodec_audiocodec.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_averrors.h>
#include <multimedia/player_framework/native_avformat.h>

#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace allplay::harmony {

namespace {

const char* mimeOf(core::AudioCodec c)
{
    switch (c) {
        case core::AudioCodec::AAC_LC: return "audio/mp4a-latm";
        case core::AudioCodec::OPUS:   return "audio/opus";
        case core::AudioCodec::LPCM:   return nullptr;   // 不用解码器
    }
    return nullptr;
}

/// OH_BitsPerSample（avcodec）→ OH_AudioStream_SampleFormat（OHAudio）
OH_AudioStream_SampleFormat toStreamSampleFormat(int ohBits)
{
    switch (ohBits) {
        case 0: return AUDIOSTREAM_SAMPLE_U8;
        case 1: return AUDIOSTREAM_SAMPLE_S16LE;
        case 2: return AUDIOSTREAM_SAMPLE_S24LE;
        case 3: return AUDIOSTREAM_SAMPLE_S32LE;
        case 4: return AUDIOSTREAM_SAMPLE_F32LE;
        default: return AUDIOSTREAM_SAMPLE_S16LE;
    }
}

/// 采样率在 AAC 的 samplingFrequencyIndex 表里的下标（拿不到返回 -1）
int sampleRateIndex(int rate)
{
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                 24000, 22050, 16000, 12000, 11025, 8000};
    for (int i = 0; i < int(sizeof kRates / sizeof kRates[0]); ++i)
        if (kRates[i] == rate) return i;
    return -1;
}

/// 给一个裸 AAC-LC 帧补 7 字节 ADTS 头（无 CRC）。
/// 字段：syncword | ID=0(MPEG-4) | layer=00 | protection_absent=1 | profile=1(LC)
///       | samplingFrequencyIndex | private=0 | channelConfig | frameLength(=头+帧) | fullness
bool pushAdtsHeader(std::vector<uint8_t>& out, size_t payloadLen, int rate, int channels)
{
    const int sri = sampleRateIndex(rate);
    if (sri < 0 || channels < 1 || channels > 7) return false;
    const size_t len = payloadLen + 7;
    if (len > 0x1FFF) return false;   // ADTS 的 frameLength 只有 13 bit

    const uint8_t h[7] = {
        0xFF,
        0xF1,   // 1111 | ID=0 | layer=00 | protection_absent=1
        uint8_t((0x01 << 6) | (sri << 2) | ((channels >> 2) & 0x01)),   // profile=LC(1)
        uint8_t(((channels & 0x03) << 6) | ((len >> 11) & 0x03)),
        uint8_t((len >> 3) & 0xFF),
        uint8_t(((len & 0x07) << 5) | 0x1F),
        0xFC,   // fullness=0x7FF，raw data blocks=0
    };
    out.assign(h, h + sizeof h);
    return true;
}

long long monoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int bytesPerSample(int ohBits)
{
    switch (ohBits) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        case 3: return 4;
        case 4: return 4;
        default: return 2;
    }
}

}  // namespace

OhosAudioSink::~OhosAudioSink() { close(); }

void OhosAudioSink::close()
{
    std::lock_guard<std::mutex> lk(mu_);
    stopRendererLocked();
    stopDecoderLocked();
    std::lock_guard<std::mutex> rk(ringMu_);
    ring_.clear();
    ringHead_ = 0;
}

void OhosAudioSink::stopDecoderLocked()
{
    if (dec_ != nullptr) {
        if (decRunning_) OH_AudioCodec_Stop(dec_);
        OH_AudioCodec_Destroy(dec_);
        dec_ = nullptr;
    }
    decRunning_ = false;
}

void OhosAudioSink::stopRendererLocked()
{
    if (renderer_ != nullptr) {
        OH_AudioRenderer_Stop(renderer_);
        OH_AudioRenderer_Release(renderer_);
        renderer_ = nullptr;
    }
    renderRate_ = 0;
    renderCh_ = 0;
    renderFmt_ = 0;
    lastCbMs_.store(0);   // 新渲染器要重新起算，别拿旧时刻算出个假空档
}

void OhosAudioSink::configure(core::AudioCodec codec, int sampleRate, int channels)
{
    std::lock_guard<std::mutex> lk(mu_);

    // 新一场 SETUP：旧解码器/渲染器全拆。手机每个音频流都会重来一次，
    // 复用风险（残留 PCM / 旧格式）比重建的代价大。
    stopRendererLocked();
    stopDecoderLocked();
    {
        std::lock_guard<std::mutex> rk(ringMu_);
        ring_.clear();
        ringHead_ = 0;
    }

    codec_ = codec;
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    ch_ = channels > 0 ? channels : 2;
    packets_.store(0);
    decoded_.store(0);
    played_.store(0);
    pushLogs_ = 0;

    // 环形缓冲按 ~250ms 算：够吸收 Wi-Fi 抖动，又不会把延迟堆起来
    setRingFormatLocked(rate_, ch_, 2);   // 先按 S16 估，真正的格式在第一个解码输出时定

    const char* mime = mimeOf(codec);
    if (mime == nullptr) {
        // LPCM：core 直接给的就是 PCM，不需要解码器；渲染器等第一帧再建
        LOG_INFO("[aud] LPCM %dHz %dch（直通播放）", rate_, ch_);
        return;
    }

    OH_AVCodec* c = OH_AudioCodec_CreateByMime(mime, false);
    if (c == nullptr) {
        LOG_ERROR("[aud] CreateByMime(%s) 失败", mime);
        return;
    }

    OH_AVFormat* fmt = OH_AVFormat_Create();
    // 关键：默认是**异步模式**（回调喂数据），不打开这个键，QueryInputBuffer 一律
    // 返回 AV_ERR_OPERATE_NOT_PERMIT —— 表现就是"每包都丢"。必须在 Configure 前设。
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_SAMPLE_RATE, rate_);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUD_CHANNEL_COUNT, ch_);
    // 统一要 S16LE：PCM 交给 OHAudio 时不用再转格式。解码器若不认，
    // 下面按 GetOutputDescription 的真实格式重建渲染器（不转换，只换声明）。
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, 1);   // SAMPLE_S16LE

    if (codec_ == core::AudioCodec::AAC_LC && adtsMode_) {
        // iPhone 给的是**裸 AAC-LC 帧**（RTP 载荷里既没有 ADTS 头也不是 LATM），
        // 而 OHOS 这个键只认 ADTS / LATM 两种：0=LATM 会把裸帧当 LOAS 去对同步字，
        // 对不上就整包丢弃（连报错都没有，表现就是"吃了不吐"）。所以走 ADTS：
        // 我们自己给每帧补 7 字节头，序列里自描述，解码器不需要任何带外配置。
        OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AAC_IS_ADTS, 1);
        LOG_INFO("[aud] AAC-LC 按 ADTS 喂（%dHz %dch）", rate_, ch_);
    } else if (codec_ == core::AudioCodec::AAC_LC) {
        // 备用路：裸帧 + 带外 ASC（5 bit objectType | 4 bit 采样率索引 | 4 bit 声道）
        const int sri = sampleRateIndex(rate_);
        if (sri >= 0) {
            const uint8_t asc[2] = {uint8_t((0x02 << 3) | (sri >> 1)),
                                    uint8_t(((sri & 0x1) << 7) | ((ch_ & 0xF) << 3))};
            OH_AVFormat_SetBuffer(fmt, OH_MD_KEY_CODEC_CONFIG, asc, sizeof asc);
            OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AAC_IS_ADTS, 0);
            LOG_INFO("[aud] AAC-LC ASC=%02x%02x（%dHz %dch）", asc[0], asc[1], rate_, ch_);
        } else {
            LOG_WARN("[aud] 采样率 %d 不在 AAC 索引表里，按裸帧试", rate_);
        }
    }

    OH_AVErrCode e = OH_AudioCodec_Configure(c, fmt);
    OH_AVFormat_Destroy(fmt);
    if (e != AV_ERR_OK) {
        LOG_ERROR("[aud] Configure 失败 code=%d（%s %dHz %dch）", int(e), mime, rate_, ch_);
        OH_AudioCodec_Destroy(c);
        return;
    }
    if ((e = OH_AudioCodec_Prepare(c)) != AV_ERR_OK ||
        (e = OH_AudioCodec_Start(c)) != AV_ERR_OK) {
        LOG_ERROR("[aud] Prepare/Start 失败 code=%d", int(e));
        OH_AudioCodec_Destroy(c);
        return;
    }

    dec_ = c;
    decRunning_ = true;
    LOG_INFO("[aud] 解码器就绪：%s %dHz %dch", mime, rate_, ch_);
}

bool OhosAudioSink::ensureRendererLocked(int rate, int channels, int ohBits)
{
    if (renderer_ != nullptr && renderRate_ == rate && renderCh_ == channels &&
        renderFmt_ == ohBits) {
        return true;
    }
    stopRendererLocked();

    const OH_AudioStream_SampleFormat sf = toStreamSampleFormat(ohBits);
    OH_AudioStreamBuilder* builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) {
        LOG_ERROR("[aud] 创建 AudioStreamBuilder 失败");
        return false;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder, rate);
    OH_AudioStreamBuilder_SetChannelCount(builder, channels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, sf);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    // 不用 FAST：那个模式给的是 ~5ms 缓冲，这台机器上调度一抖就欠载，
    // 表现为连续爆点。普通模式让系统给一个几十毫秒的缓冲，抖动被吸收掉。
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    // 不要把回调帧钉小。钉成 1 帧（21ms）后，系统缓冲也一起变小，
    // 滑动 CarPlay 时 UI/解码抢 CPU、声卡线程晚回来一次就没得缓冲了。
    // 保留系统默认的 ~93ms 大块（抗调度抖动），网络抖动交给下面 700ms 的环形池。

    OH_AudioRenderer_Callbacks cb{};
    cb.OH_AudioRenderer_OnWriteData = &OhosAudioSink::OnWriteData;
    cb.OH_AudioRenderer_OnInterruptEvent = &OhosAudioSink::OnInterrupt;
    cb.OH_AudioRenderer_OnError = &OhosAudioSink::OnError;
    OH_AudioStreamBuilder_SetRendererCallback(builder, cb, this);

    OH_AudioRenderer* r = nullptr;
    OH_AudioStream_Result res = OH_AudioStreamBuilder_GenerateRenderer(builder, &r);
    OH_AudioStreamBuilder_Destroy(builder);
    if (res != AUDIOSTREAM_SUCCESS || r == nullptr) {
        LOG_ERROR("[aud] GenerateRenderer 失败 res=%d", int(res));
        return false;
    }
    if (OH_AudioRenderer_Start(r) != AUDIOSTREAM_SUCCESS) {
        LOG_ERROR("[aud] AudioRenderer Start 失败");
        OH_AudioRenderer_Release(r);
        return false;
    }

    renderer_ = r;
    renderRate_ = rate;
    renderCh_ = channels;
    renderFmt_ = ohBits;
    const double capMs = setRingFormatLocked(rate, channels, bytesPerSample(ohBits));

    LOG_INFO("[aud] 渲染器启动：%dHz %dch 采样格式=%d（%.0fms 缓冲）", rate, channels, ohBits,
             capMs);
    return true;
}

double OhosAudioSink::setRingFormatLocked(int rate, int channels, int bytesPerSample)
{
    std::lock_guard<std::mutex> rk(ringMu_);
    bytesPerFrame_ = channels * (bytesPerSample > 0 ? bytesPerSample : 2);
    // 池子 1.5s。手机送音频是**成团**的：实测连续两包之间能空 550ms（RTP 序号连着，
    // 一包没丢，就是没发），一坨十几包再一起到。水位必须比这个空档高，否则每来一波
    // 就把池子抽干一次 —— 这就是"偶尔、轻微卡一下"的全部来源。
    ringCap_ = size_t(rate) * size_t(bytesPerFrame_) * 3 / 2;
    if (ringCap_ < 8192) ringCap_ = 8192;
    // 两个水位：开流头一拍和续播不是一回事。
    // 手机刚开流时会**先卡 2.4 秒**（实测），然后一口气把积压吐过来；
    // 那之前只攒一点点就开播，第一帧就欠载、连爆好几次。
    // 所以头一拍等 800ms 再开声，开声之后降回续播水位。
    // 续播水位 700ms 来自实测的最大团间空档 552ms（再留 150ms 余量）：
    // 250ms / 450ms 都被一波 550ms 的空档直接抽干过。
    resumePrefill_ = size_t(rate) * size_t(bytesPerFrame_) * 7 / 10;   // 700ms
    startPrefill_  = size_t(rate) * size_t(bytesPerFrame_) * 4 / 5;    // 800ms
    if (startPrefill_ > ringCap_ * 3 / 4) startPrefill_ = ringCap_ * 3 / 4;
    if (resumePrefill_ > ringCap_ / 2) resumePrefill_ = ringCap_ / 2;
    prefill_ = startPrefill_;
    waiting_ = true;   // 起步也先攒够再播，别一上来就欠载
    if (rate <= 0 || bytesPerFrame_ <= 0) return 0.0;
    return double(ringCap_ / size_t(bytesPerFrame_)) * 1000.0 / double(rate);
}

void OhosAudioSink::pushPcm(const uint8_t* p, size_t n)
{
    if (n == 0) return;
    pcmBytes_.fetch_add((long long)n);

    // 声卡回调停摆看门狗：解码还在出帧，但声卡回调长时间没来 = 声音断了。
    // 来电、别的应用抢焦点、路由切换都会这样，以前只能靠人耳发现。
    const long long now = monoMs();
    const long long last = lastCbMs_.load();
    if (last != 0 && cbGapLogs_ < 60) {
        const long long gap = now - last;
        if (gap > 300) {
            ++cbGapLogs_;
            LOG_WARN("[aud] 声卡回调停了 %lldms（上次 #%lld，解码已到 %lld）",
                     gap, played_.load(), decoded_.load());
        }
    }

    std::lock_guard<std::mutex> rk(ringMu_);
    ring_.insert(ring_.end(), p, p + n);
    // 落后太多就丢最老的：音频宁可掉一点也别越积越延迟。
    // 但**一次丢到开流水位**，不是"超出多少丢多少"。实测开流时手机会先囤两秒再一口气吐过来，
    // 逐帧丢弃会把这团积压切成几十个 9~21ms 的小断口 —— 每一个都是一声"咔"，
    // 连起来就是用户听到的那片噪音。切一个大断口只响一下，水位也立刻回到目标，
    // 后面不会反复贴着上限磨。
    size_t avail = ring_.size() - ringHead_;
    if (avail > ringCap_) {
        const size_t target = startPrefill_;
        const size_t drop = avail - target;
        const double bps = double(rate_) * double(bytesPerFrame_ > 0 ? bytesPerFrame_ : 4);
        static int dropLogs = 0;
        if (dropLogs < 20) {
            ++dropLogs;
            LOG_WARN("[aud] 缓冲溢出：水位 %.0fms 超上限，一次切掉最老 %.0fms 回到 %.0fms",
                     double(avail) * 1000.0 / bps, double(drop) * 1000.0 / bps,
                     double(target) * 1000.0 / bps);
        }
        ringHead_ += drop;
        // 按整帧丢，避免把一帧 PCM 劈开
        ringHead_ -= ringHead_ % size_t(bytesPerFrame_ > 0 ? bytesPerFrame_ : 1);
    }
    if (ringHead_ > (1u << 16)) {
        ring_.erase(ring_.begin(), ring_.begin() + long(ringHead_));
        ringHead_ = 0;
    }
}

void OhosAudioSink::drainOutputLocked()
{
    for (;;) {
        uint32_t index = 0;
        // 2ms：解码器是异步吐帧的，timeout=0 时经常"刚喂进去还没出来"，
        // 结果 PCM 一直待在解码器里，环形缓冲反而见底 —— 用一点点等待把帧尽早捞出来
        const OH_AVErrCode qe = OH_AudioCodec_QueryOutputBuffer(dec_, &index, 2'000);
        if (qe != AV_ERR_OK) {
            // 只报前几次和偶尔一次：能区分"暂时没解出来"和"解码器直接不认"
            static int outLogs = 0;
            const long long fed = packets_.load();
            if (outLogs < 3 || fed % 600 == 0) {
                ++outLogs;
                LOG_INFO("[aud] 暂无输出 buffer（code=%d，已送 %lld 包，已解 %lld）",
                         int(qe), fed, decoded_.load());
            }
            break;
        }
        OH_AVBuffer* buf = OH_AudioCodec_GetOutputBuffer(dec_, index);
        if (buf == nullptr) break;

        OH_AVCodecBufferAttr attr{};
        if (OH_AVBuffer_GetBufferAttr(buf, &attr) == AV_ERR_OK && attr.size > 0) {
            // 第一个输出帧：按真实输出格式建渲染器（解码器不一定听我们的 S16 要求）
            if (renderer_ == nullptr) {
                int outRate = rate_, outCh = ch_, outFmt = 1;
                OH_AVFormat* desc = OH_AudioCodec_GetOutputDescription(dec_);
                if (desc != nullptr) {
                    OH_AVFormat_GetIntValue(desc, OH_MD_KEY_AUD_SAMPLE_RATE, &outRate);
                    OH_AVFormat_GetIntValue(desc, OH_MD_KEY_AUD_CHANNEL_COUNT, &outCh);
                    OH_AVFormat_GetIntValue(desc, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, &outFmt);
                    OH_AVFormat_Destroy(desc);
                }
                if (outRate <= 0) outRate = rate_;
                if (outCh <= 0) outCh = ch_;
                ensureRendererLocked(outRate, outCh, outFmt);
            }
            const uint8_t* p = OH_AVBuffer_GetAddr(buf);
            if (p != nullptr) {
                pushPcm(p + attr.offset, size_t(attr.size));
                const long long n = decoded_.fetch_add(1) + 1;
                if (n <= 3 || n % 500 == 0) {
                    LOG_INFO("[aud] 解码输出 #%lld：%d 字节", n, int(attr.size));
                }
            }
        }
        OH_AudioCodec_FreeOutputBuffer(dec_, index);
    }
}

void OhosAudioSink::feed(const uint8_t* payload, size_t len, uint32_t sample)
{
    if (payload == nullptr || len == 0) return;
    std::lock_guard<std::mutex> lk(mu_);

    const long long n = packets_.fetch_add(1) + 1;

    // LPCM：不解码，直接进播放缓冲（格式在 configure 时已按 S16 估）
    if (dec_ == nullptr && codec_ == core::AudioCodec::LPCM) {
        if (renderer_ == nullptr) ensureRendererLocked(rate_, ch_, 1);
        if (renderer_ != nullptr) {
            // AirPlay 的 LPCM 是**大端** s16（网络序），渲染器按小端播 —— 逐样本换字节。
            // 不换的话每个样本的高低字节颠倒，听感是满耳噪音（macOS 宿主上已实测确认）。
            scratch_.resize(len & ~size_t(1));
            for (size_t i = 0; i + 1 < len; i += 2) {
                scratch_[i]     = payload[i + 1];
                scratch_[i + 1] = payload[i];
            }
            pushPcm(scratch_.data(), scratch_.size());
        }
        if (n <= 3 || n % 600 == 0) {
            LOG_INFO("[aud] LPCM 包 #%lld：%zu 字节，已播 %lld", n, len, played_.load());
        }
        return;
    }
    if (!decRunning_ || dec_ == nullptr) return;

    // ADTS 模式：给裸帧补 7 字节头（序列自描述，解码器不用带外配置）
    if (codec_ == core::AudioCodec::AAC_LC && adtsMode_) {
        // 手机开流时的头几个包只有 6 字节（不是有效 AAC 帧），喂进去只会让解码器报错
        if (len < 8) {
            if (n <= 8) LOG_INFO("[aud] 跳过 %zu 字节的短包 #%lld", len, n);
            return;
        }
        // 万一哪天手机直接给 ADTS（0xFFF 同步字），别再包一层
        const bool alreadyAdts = payload[0] == 0xFF && (payload[1] & 0xF0) == 0xF0;
        if (alreadyAdts) {
            // 原样送
        } else {
            scratch_.clear();
            if (!pushAdtsHeader(scratch_, len, rate_, ch_)) return;
            scratch_.insert(scratch_.end(), payload, payload + len);
            payload = scratch_.data();
            len = scratch_.size();
        }
    }

    uint32_t index = 0;
    // 10ms：比一帧 AAC（~21ms）短，宁可丢一包也不把收包线程拖住
    const OH_AVErrCode qe = OH_AudioCodec_QueryInputBuffer(dec_, &index, 10'000);
    if (qe != AV_ERR_OK) {
        if (n <= 5 || n % 600 == 0) {
            LOG_WARN("[aud] 没有输入 buffer，丢包 #%lld（%zu 字节，code=%d）", n, len, int(qe));
        }
        return;
    }
    OH_AVBuffer* in = OH_AudioCodec_GetInputBuffer(dec_, index);
    if (in == nullptr) return;
    uint8_t* addr = OH_AVBuffer_GetAddr(in);
    const int32_t cap = OH_AVBuffer_GetCapacity(in);
    if (addr == nullptr || cap <= 0 || size_t(cap) < len) {
        LOG_WARN("[aud] 输入 buffer 不够：cap=%d need=%zu", int(cap), len);
        return;
    }
    std::memcpy(addr, payload, len);
    OH_AVCodecBufferAttr attr{};
    attr.pts = int64_t(sample);
    attr.size = int32_t(len);
    attr.offset = 0;
    attr.flags = 0;
    OH_AVBuffer_SetBufferAttr(in, &attr);
    if (OH_AudioCodec_PushInputBuffer(dec_, index) != AV_ERR_OK) {
        LOG_WARN("[aud] PushInputBuffer 失败");
        return;
    }
    drainOutputLocked();

    if (pushLogs_ < 3 || n % 600 == 0) {
        ++pushLogs_;
        LOG_INFO("[aud] 音频包 #%lld：%zu 字节 → 解码输出 %lld，已播 %lld", n, len,
                 decoded_.load(), played_.load());
    }
}

void OhosAudioSink::reset()
{
    std::lock_guard<std::mutex> lk(mu_);
    {
        std::lock_guard<std::mutex> rk(ringMu_);
        ring_.clear();
        ringHead_ = 0;
        waiting_ = true;
        underruns_ = 0;
    }
    if (decRunning_ && dec_ != nullptr) OH_AudioCodec_Flush(dec_);
    LOG_INFO("[aud] reset");
}

void OhosAudioSink::selfTest(int ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    stopRendererLocked();
    stopDecoderLocked();
    codec_ = core::AudioCodec::LPCM;
    rate_ = 48000;
    ch_ = 2;
    played_.store(0);
    if (!ensureRendererLocked(48000, 2, 1)) {
        LOG_ERROR("[aud] 自检：渲染器没建起来");
        return;
    }

    // 440Hz 正弦，S16LE 立体声，每 10ms 推一段（和真实 LPCM 包的节奏一致）
    constexpr int kChunk = 480;
    constexpr double kTwoPi = 6.283185307179586;
    const double step = kTwoPi * 440.0 / 48000.0;
    double phase = 0.0;
    std::vector<int16_t> buf(size_t(kChunk) * 2);
    for (int t = 0; t < ms; t += 10) {
        for (int i = 0; i < kChunk; ++i) {
            const int16_t v = int16_t(12000.0 * std::sin(phase));
            phase += step;
            if (phase > kTwoPi) phase -= kTwoPi;
            buf[size_t(i) * 2] = v;
            buf[size_t(i) * 2 + 1] = v;
        }
        pushPcm(reinterpret_cast<const uint8_t*>(buf.data()), buf.size() * sizeof(int16_t));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_INFO("[aud] 自检完成：%dms 正弦，送声卡 %lld 次", ms, played_.load());
}

void OhosAudioSink::selfTestAac(const uint8_t* data, size_t len)
{
    if (data == nullptr || len < 7) {
        LOG_ERROR("[aud] 自检：AAC 样片为空");
        return;
    }
    // 先按 ADTS 切帧（样片本来就是 ADTS 封装的）
    struct Frame { const uint8_t* p; size_t n; };
    std::vector<Frame> frames;
    for (size_t i = 0; i + 7 <= len;) {
        if (data[i] != 0xFF || (data[i + 1] & 0xF0) != 0xF0) break;
        const size_t flen = (size_t(data[i + 3] & 0x03) << 11) | (size_t(data[i + 4]) << 3) |
                            (size_t(data[i + 5]) >> 5);
        const size_t hlen = (data[i + 1] & 0x01) ? 7 : 9;
        if (flen < hlen || i + flen > len) break;
        frames.push_back({data + i + hlen, flen - hlen});
        i += flen;
    }
    if (frames.empty()) {
        LOG_ERROR("[aud] 自检：样片里没切出 ADTS 帧");
        return;
    }
    LOG_INFO("[aud] 自检：样片 %zu 字节，切出 %zu 帧", len, frames.size());

    for (int mode = 0; mode < 2; ++mode) {
        adtsMode_ = (mode == 0);
        configure(core::AudioCodec::AAC_LC, 48000, 2);   // configure 会重建解码器并清零计数
        const std::string tag = adtsMode_ ? "ADTS" : "裸帧+ASC";

        // 按 21ms 一帧的节奏喂，让解码器和渲染器都有时间跑
        for (const auto& f : frames) {
            feed(f.p, f.n, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        }
        LOG_INFO("[aud] 自检[%s]：喂 %zu 帧 → 解码输出 %lld，送声卡 %lld", tag.c_str(),
                 frames.size(), decoded_.load(), played_.load());
    }
    adtsMode_ = true;
    configure(core::AudioCodec::AAC_LC, 48000, 2);
}

int32_t OhosAudioSink::OnWriteData(OH_AudioRenderer* /*renderer*/, void* userData,
                                   void* buffer, int32_t length)
{
    auto* self = static_cast<OhosAudioSink*>(userData);
    auto* out = static_cast<uint8_t*>(buffer);
    self->lastCbMs_.store(monoMs());
    // 只拿 ringMu_：这条是声卡回调线程，绝不能被仍在等解码器输入 buffer 的线程堵住
    std::lock_guard<std::mutex> lk(self->ringMu_);
    if (out == nullptr || length <= 0) return 0;

    const size_t want = size_t(length);
    size_t have = self->ring_.size() - self->ringHead_;

    // 声卡线程被拖慢后会一次要一大块来"补账"，这是滑动时卡顿的关键线索
    if (self->chunkBase_ == 0) {
        self->chunkBase_ = length;
    } else if (length != self->chunkBase_) {
        if (self->chunkOdd_ < 5 || self->chunkOdd_ % 20 == 0) {
            LOG_INFO("[aud] 声卡要了非常规块：%d 字节（常规 %d，第 %d 次）", int(length),
                     self->chunkBase_, self->chunkOdd_ + 1);
        }
        ++self->chunkOdd_;
    }

    // 欠载恢复：宁可先静音一两帧，也要等缓冲重新攒够再续，否则是连续爆点
    if (self->waiting_) {
        if (have < self->prefill_) {
            std::memset(out, 0, want);
            return 0;
        }
        self->waiting_ = false;
        // 开声了就降回续播水位：这一场后面再欠载要 250ms 就回来，不能一直等 600ms
        if (self->prefill_ != self->resumePrefill_) {
            LOG_INFO("[aud] 开声，续播水位从 %.0fms 降到 %.0fms",
                     double(self->prefill_) * 1000.0 /
                         (double(self->renderRate_) * double(self->renderCh_) * 2.0),
                     double(self->resumePrefill_) * 1000.0 /
                         (double(self->renderRate_) * double(self->renderCh_) * 2.0));
            self->prefill_ = self->resumePrefill_;
        }
    }

    const size_t take = std::min(want, have);
    if (take > 0) {
        std::memcpy(out, self->ring_.data() + self->ringHead_, take);
        self->ringHead_ += take;
        self->takenBytes_.fetch_add((long long)take);
    }
    if (take < want) {
        std::memset(out + take, 0, want - take);
        self->waiting_ = true;          // 抽空了：下次先攒够再说
        const long long un = ++self->underruns_;
        // 每次欠载都打一行：之前每 600 次回调才打一行，5 次欠载全被吞了。
        // 欠载是"听着断一下"的直接来源，必须能和上游的收包空档对上时间。
        LOG_WARN("[aud] 欠载 #%lld：只拿到 %zu/%d 字节（缺 %.0fms，水位剩 %zu，入 %lld / 出 %lld）",
                 un, take, int(length),
                 double(want - take) * 1000.0 /
                     (double(self->renderRate_ > 0 ? self->renderRate_ : 48000) *
                      double(self->renderCh_ > 0 ? self->renderCh_ : 2) * 2.0),
                 have - take, self->pcmBytes_.load(), self->takenBytes_.load());
    }
    if (self->ringHead_ == self->ring_.size()) {
        self->ring_.clear();
        self->ringHead_ = 0;
    }
    const long long n = self->played_.fetch_add(1) + 1;
    if (n <= 3 || n % 600 == 0) {
        const size_t left = self->ring_.size() - self->ringHead_;
        LOG_INFO("[aud] 送声卡 #%lld：%zu/%d 字节%s（水位 %zu，累计欠载 %lld，入 %lld / 出 %lld 字节）",
                 n, take, int(length), take < want ? "（补静音）" : "", left, self->underruns_,
                 self->pcmBytes_.load(), self->takenBytes_.load());
    }
    return 0;
}

int32_t OhosAudioSink::OnInterrupt(OH_AudioRenderer* /*renderer*/, void* /*userData*/,
                                   OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint)
{
    // 来电、别的应用抢焦点、耳机插拔都会走到这里。系统会自己暂停/恢复声卡，
    // 我们要做的只是把它记下来 —— 以前这条路径完全静默，"来电后卡死"无从查起。
    LOG_WARN("[aud] 声卡被打断：type=%d hint=%d（type: 0=强制 1=共享；hint: 0=打断 1=降音 2=暂停 3=恢复 4=停止）",
             int(type), int(hint));
    return 0;
}

int32_t OhosAudioSink::OnError(OH_AudioRenderer* /*renderer*/, void* /*userData*/,
                               OH_AudioStream_Result error)
{
    LOG_ERROR("[aud] 声卡报错：%d", int(error));
    return 0;
}

}  // namespace allplay::harmony
