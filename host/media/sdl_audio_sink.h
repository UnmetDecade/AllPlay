/**
 * @file   sdl_audio_sink.h
 * @brief  core::IAudioOutput 的桌面实现：ffmpeg 软解 + SDL 音频设备
 * @author blt
 */

// ============================================================================
// host/media/sdl_audio_sink.h —— IAudioOutput：ffmpeg 软解 + SDL 音频设备
// ============================================================================
// 线程（由 core 的调用点决定，必须照做）：
//   · configure() —— **RTSP 线程**（core/airplay/session.cpp:392，每次 SETUP 音频流）
//   · feed()      —— AudioStream 线程（core/airplay/audio_stream.cpp:run）
//   · serviceDevice() —— **主线程**（SDL 只在主线程开/关设备，见 appstate 的约定）
//   · 音频回调    —— SDL 自己的音频线程（只从环形缓冲取数据）
//
// 乱序不用管：core 的 AudioStream::deliver() 已经按 RTP 序号重排、丢掉太晚到的包，
// feed() 拿到的永远是按序的。这里只管解码 + 缓冲 + 喂设备。
//
// 三种载荷（core/port/media.h 的 AudioCodec）：
//   · AAC_LC —— 裸帧，**没有 ADTS 头**，得自己按协商值造 2 字节 ASC 当 extradata；
//               还会混进 ≤12 字节的配置小包，要忽略（当帧喂会让解码器一直报错）。
//   · OPUS   —— 裸包，extradata 是最小 19 字节 OpusHead。
//   · LPCM   —— 不解码，**大端 s16（网络序）转成主机序**后进环形缓冲。
//
// 不引 swresample：解码器吐的是平面 float（FLTP），"平面 float → 交织 s16" 自己写
// 三十行就够了，顺带绕开 swresample 那套在 4.4 与 5.1+ 之间签名不同的 channel-layout
// 参数（见 media/ffmpeg_compat.h 的说明）。采样率不重采样 —— 设备就按协商值打开，
// CarPlay 只会给 44100/48000，SDL 两个都原生支持。
#pragma once

#include "port/media.h"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace allplay {

class SdlAudioSink : public core::IAudioOutput {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink() override;
    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    // ---- core::IAudioOutput ----
    void configure(core::AudioCodec codec, int sampleRate, int channels) override;
    void feed(const uint8_t* payload, size_t len, uint32_t sample) override;
    void reset() override;

    // ---- 主线程 ----
    /// 把 configure() 记下的待办开成 SDL 设备。没变化时立即返回，可以每帧调。
    void serviceDevice();

    // ---- 统计（任意线程读）----
    uint64_t decoded() const { return decoded_.load(); }   ///< 解出并写进环形缓冲的帧数
    uint64_t dropped() const { return dropped_.load(); }   ///< 缓冲溢出、被迫丢最旧的次数
    /// 回调没能拿满一块的次数。⚠️ 无头跑（SDL_AUDIODRIVER=dummy）时这个数**不可当音质指标**：
    /// dummy 声卡不按实时节奏回调，抖动全算在它头上。真机上才有参考价值。
    uint64_t underruns() const { return underruns_.load(); }

    bool deviceOpen() const { return dev_ != 0; }

private:
    /// 环形缓冲能容纳的 s16 样本数（单声道计），约 2 秒。
    static constexpr size_t kRingSamples = 48000 * 2;
    /// SDL 每次回调要的帧数（协商采样率下 ≈21ms @48k）。
    static constexpr int    kChunkFrames = 1024;
    /// 攒够这么多块才启播。手机是按**实时速率**发的，一开声卡就放等于从空缓冲起步，
    /// 前几块必然欠载 —— 垫 ~43ms 上去，同时不至于给音画同步添太多延迟。
    /// （具体几块是经验值：SDL 的 dummy 声卡本身不精确回调，这个数在无头环境下
    ///   量不出差别，取值以"够挡住起播那一下"为准。）
    static constexpr int    kStartChunks = 2;

    struct Config {
        core::AudioCodec codec = core::AudioCodec::LPCM;
        int rate     = 48000;
        int channels = 2;
        bool valid   = false;   ///< false = 没有音频流，应关掉设备
    };

    static void sdlCallback(void* user, Uint8* stream, int len);

    /// feed() 的解码路径：喂一包给解码器，把解出的帧转成 s16 追进环形缓冲。持锁调用。
    void decodeLocked(const uint8_t* payload, size_t len);
    /// 把一帧（平面 float 或 s16）转成交织 s16 追进环形缓冲。持锁调用。
    void appendFrameLocked(const AVFrame* f);
    /// 环形缓冲写入，溢出丢**最旧**的（保低延迟，同视频队列的思路）。持锁调用。
    void pushLocked(const int16_t* d, size_t n);

    void closeDecoderLocked();
    /// 按 codec 造解码器（extradata 自己合成）。持锁调用。
    bool openDecoderLocked();

    mutable std::mutex mu_;

    // —— 受 mu_ 保护 ——
    Config                     cfg_;
    bool                       deviceDirty_ = false;   ///< configure() 置位，主线程消费
    AVCodecContext*            dec_ = nullptr;
    AVPacket*                  pkt_ = nullptr;
    AVFrame*                   frame_ = nullptr;
    std::vector<int16_t>       ring_;
    std::vector<int16_t>       scratch_;               ///< 解码帧 → 交织 s16 的落脚点
    std::vector<int16_t>       pcmSwap_;               ///< LPCM 大端 → 主机序的落脚点
    size_t                     head_ = 0, size_ = 0;   ///< 环形缓冲读位置 / 已存样本数
    std::atomic<uint64_t>      decoded_{0};
    std::atomic<uint64_t>      dropped_{0};
    std::atomic<uint64_t>      underruns_{0};
    bool                       stop_ = false;          ///< 析构时置位，回调让路

    /// 启播阈值（样本数 = 块数 × 每块帧数 × 声道），configure 时算好。
    size_t                     startSamples_ = 0;

    // —— 只在主线程碰 ——
    SDL_AudioDeviceID dev_ = 0;
    bool              playing_ = false;   ///< 是否已放行回调（攒够垫底才置真）

    // 只读快照，供调用方无锁读取（configure 时写）
    std::atomic<int> outRate_{0};
    std::atomic<int> outChannels_{0};
};

}  // namespace allplay
