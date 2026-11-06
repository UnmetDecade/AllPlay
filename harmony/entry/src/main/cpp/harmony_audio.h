/**
 * @file   harmony_audio.h
 * @brief  CarPlay 音频输出：OH AudioCodec 解码 → OHAudio 渲染（OHOS NDK）
 * @author blt
 */

// ============================================================================
// harmony_audio.h —— 鸿蒙侧 core::IAudioOutput
// ============================================================================
// core 交给这一层的是一帧**压缩**音频（AAC-LC / Opus 的 RTP 载荷，或已经解好的 LPCM），
// 和桌面那种"ffmpeg 解 + SDL 播"的分工一样，这里分两段：
//   解码：OH_AudioCodec（native_media_acodec）——同步 query 接口，不注册回调，
//         整段跑在 AudioStream 的收包线程里，简单且没有跨线程排队；
//   播放：OHAudio（libohaudio）渲染器，回调里从环形缓冲取 PCM，不够就补静音。
//
// 线程：
//   · feed()/configure()/reset() 来自 AudioStream 线程（core 不管并发，本类自己加锁）
//   · OnWriteData 来自 OHAudio 自己的线程 —— 也走同一把锁
#pragma once

#include "port/media.h"

#include <ohaudio/native_audiostream_base.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// OH_AudioRenderer 是 typedef struct OH_AudioRendererStruct（native_audiostream_base.h 里定义）
struct OH_AVCodec;

namespace allplay::harmony {

class OhosAudioSink : public core::IAudioOutput {
public:
    OhosAudioSink() = default;
    ~OhosAudioSink() override;
    OhosAudioSink(const OhosAudioSink&) = delete;
    OhosAudioSink& operator=(const OhosAudioSink&) = delete;

    void configure(core::AudioCodec codec, int sampleRate, int channels) override;
    void feed(const uint8_t* payload, size_t len, uint32_t sample) override;
    void reset() override;

    void close();

    /// 自检：不开会话，直接用 48k 立体声正弦走一遍渲染器（验证 OHAudio 这条路）
    void selfTest(int ms);

    /// 自检：把一段 ADTS 的 AAC 文件分别按「ADTS 封装」和「裸帧 + 带外 ASC」喂给解码器，
    /// 看哪条路真能解出 PCM（不开会话，不需要手机）
    void selfTestAac(const uint8_t* adts, size_t len);

    long long packets() const { return packets_.load(); }
    long long decodedFrames() const { return decoded_.load(); }
    long long playedBuffers() const { return played_.load(); }

private:
    bool startDecoderLocked();
    void stopDecoderLocked();
    void drainOutputLocked();
    bool ensureRendererLocked(int rate, int channels, int sampleFmt);
    void stopRendererLocked();
    void pushPcm(const uint8_t* p, size_t n);
    /// 按输出格式重算环形缓冲时长；返回缓冲能装多少毫秒
    double setRingFormatLocked(int rate, int channels, int bytesPerSample);

    static int32_t OnWriteData(OH_AudioRenderer* renderer, void* userData,
                               void* buffer, int32_t length);
    /// 系统打断（来电、别的应用抢焦点、路由切换）：不处理会表现为"声音没了/卡住"
    static int32_t OnInterrupt(OH_AudioRenderer* renderer, void* userData,
                               OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint);
    static int32_t OnError(OH_AudioRenderer* renderer, void* userData,
                           OH_AudioStream_Result error);

    // 两把锁，锁序 mu_ → ringMu_（OnWriteData 只拿 ringMu_，永不等 mu_）：
    // 解码那条路会为了等输入 buffer 阻塞十几毫秒，播放回调绝不能排在它后面。
    std::mutex mu_;
    std::mutex ringMu_;

    OH_AVCodec* dec_ = nullptr;
    bool        decRunning_ = false;
    core::AudioCodec codec_ = core::AudioCodec::AAC_LC;
    int         rate_ = 48000;
    int         ch_ = 2;
    /// true = 自己给每帧补 ADTS 头再喂解码器；false = 裸帧 + 带外 ASC
    std::atomic<bool> adtsMode_{true};
    std::vector<uint8_t> scratch_;   // ADTS 封装用的临时缓冲（feed 在本线程，不跨线程）

    OH_AudioRenderer* renderer_ = nullptr;
    int renderRate_ = 0;
    int renderCh_ = 0;
    int renderFmt_ = 0;   // OH_BitsPerSample

    // ---- 以下由 ringMu_ 保护 ----
    int bytesPerFrame_ = 4;
    std::vector<uint8_t> ring_;   // PCM 环形缓冲（线性数组 + 读头，攒够大再压缩）
    size_t ringHead_ = 0;
    size_t ringCap_ = 0;
    size_t prefill_ = 0;          // 现在生效的续播水位（开头高、跑起来后降下来）
    size_t startPrefill_ = 0;     // 开流头一次的水位：要扛住手机开流时的卡顿
    size_t resumePrefill_ = 0;    // 跑起来后的续播水位：欠载恢复要快
    bool   waiting_ = false;      // 正在等缓冲填上来（这段时间输出静音）
    long long underruns_ = 0;
    int chunkBase_ = 0;           // 声卡回调的常规块大小（字节），用来识别"补账"大块
    int chunkOdd_ = 0;            // 非常规块出现次数
    /// 上一次声卡回调的时刻（毫秒，CLOCK_MONOTONIC）。声卡回调停了 = 声音没了/卡住，
    /// 而解码还在往里灌 —— 这种"上不接下"以前完全看不出来，只能靠人耳判断。
    std::atomic<long long> lastCbMs_{0};
    int cbGapLogs_ = 0;

    std::atomic<long long> packets_{0};
    std::atomic<long long> decoded_{0};
    std::atomic<long long> played_{0};
    std::atomic<long long> pcmBytes_{0};   // 解码器交出来的 PCM 总字节
    std::atomic<long long> takenBytes_{0}; // 声卡回调实际取走的总字节
    int pushLogs_ = 0;
};

}  // namespace allplay::harmony
