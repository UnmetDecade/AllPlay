/**
 * @file   backend.h
 * @brief  协议后端抽象：CarPlay / Android Auto / CarLife / HiCar 的共同门面
 * @author blt
 */

// ============================================================================
// core/projection/backend.h —— 协议后端抽象（草案：只有接口，没有实现）
// ============================================================================
// 目标：宿主只认 IProjectionBackend / IProjectionSession，不认 iAP2、RTSP、
// AOAP 这些协议细节。第一个后端是 CarPlay —— 把现有 airplay/iap2/mfi/usb
// 包起来，行为不变；Android Auto / CarLife / HiCar 后续按同一接口接入。
//
// 分层：
//   宿主 ──> IProjectionBackend（发现 + 连接）
//              └─> IProjectionSession（一次投屏会话）
//                    └─> 回调 IProjectionSink（媒体帧 + 状态）
//   媒体输出仍由宿主实现 core::IVideoOutput / core::IAudioOutput，core 不解码。
//
// ⚠️ 线程约定（与现有 core 一致，实现必须遵守）：
//   · probe() / connect()   —— 调用方线程；probe 可能较慢（ioreg / USB 枚举 / mDNS）；
//   · start() 立即返回，握手在会话内部线程跑，结果经 onState() 通知；
//   · onVideoFormat/onVideoFrame 来自视频线程，onAudio* 来自音频线程，
//     onState 来自控制线程 —— 三者可能并发，Sink 实现必须自己加锁；
//   · sendInput() / requestKeyFrame() / stop() 任意线程可调；
//   · stop() 幂等；返回后保证不再有回调（实现要 join 内部线程）。
#pragma once

#include "port/media.h"
#include "port/platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core::projection {

// ---------------------------------------------------------------------------
// 协议与目标
// ---------------------------------------------------------------------------

enum class Protocol { CarPlay, AndroidAuto, CarLife, HiCar };

/// 一次 probe() 发现的可连接对象（手机，或盒子伪装成手机）。
struct ProjectionTarget {
    Protocol    protocol = Protocol::CarPlay;
    /// 后端内唯一、跨重连稳定：序列号 / 配对 ID / IP，宿主只当不透明键。
    std::string id;
    std::string name;       ///< 给 UI 展示
    std::string transport;  ///< "usb" / "wifi" / "bt"，仅展示与日志
    /// 后端私有句柄（USB 设备、地址、配对材料…）。只允许创建它的后端解释；
    /// 宿主不得取值，也不得跨后端传递。
    std::shared_ptr<void> handle;
};

/// 连接前就能问的能力，UI 用来决定显示哪些控件（触摸/旋钮/按键）。
struct Capabilities {
    bool touch  = false;
    bool key    = false;
    bool rotary = false;
    /// 0 = 未知 / 无上限。
    int  max_width  = 0;
    int  max_height = 0;
    std::vector<VideoCodec> video_codecs;
    std::vector<AudioCodec> audio_codecs;
};

// ---------------------------------------------------------------------------
// 媒体配置（复用 core::VideoCodec / core::AudioCodec，与 IVideoOutput 同义）
// ---------------------------------------------------------------------------

struct VideoConfig {
    VideoCodec           codec = VideoCodec::H264;
    int                  width = 0;
    int                  height = 0;
    /// ISO BMFF avcC/hvcC 记录体（不含 fourcc），含义同 IVideoOutput::configure。
    std::vector<uint8_t> codec_data;
};

struct AudioConfig {
    AudioCodec codec       = AudioCodec::LPCM;
    int        sample_rate = 48000;
    int        channels    = 2;
};

// ---------------------------------------------------------------------------
// 协议中立的输入事件
// ---------------------------------------------------------------------------
// 后端负责把各自协议的 HID / 按键 / 旋钮翻译成这里的语义值，宿主只收发这一种。

enum class InputKind { Touch, Key, Rotary };

enum class TouchPhase { Down, Move, Up, Cancel };

enum class KeyCode { Home, Back, Media, Voice, Phone, Navigation, Custom };

struct InputEvent {
    InputKind kind = InputKind::Touch;

    // —— Touch：归一化坐标，左上角 (0,0)、右下角 (1,1)；沿用现有 TouchPoint 约定 ——
    TouchPhase phase   = TouchPhase::Down;
    uint8_t    pointer = 0;    ///< 触点编号，0 为主触点（CarPlay 目前最多 2 触点）
    float      x       = 0.f;
    float      y       = 0.f;

    // —— Key ——
    KeyCode    key  = KeyCode::Home;
    bool       down = true;

    // —— Rotary：每格 = 1，正数顺时针 ——
    int8_t     rotary = 0;
};

// ---------------------------------------------------------------------------
// 状态与会话回调
// ---------------------------------------------------------------------------

enum class SessionState {
    Idle,          ///< 建好但未 start
    Connecting,    ///< 物理链路已通，正在识别/握手
    Negotiating,   ///< 认证/配对/服务协商
    Running,       ///< 音视频已起流
    Stopping,
    Disconnected,  ///< 对端或链路断开（可重连，交给宿主决定）
    Failed,        ///< 不可恢复错误，detail 写原因
};

/// 宿主实现：把帧转给 IVideoOutput / IAudioOutput，把状态转给 UI。
class IProjectionSink {
public:
    virtual ~IProjectionSink() = default;

    virtual void onVideoConfig(const VideoConfig& cfg) = 0;

    /// AnnexB 帧。返回 false = 未能入队，后端应重新放闸并请求关键帧
    /// （与 IVideoOutput::feedAnnexB 的约定一致）。
    virtual bool onVideoFrame(const uint8_t* data, size_t len) = 0;

    /// 关键帧恢复 / 新 IDR 前清参考链。默认空实现：不解码的宿主可以不管。
    virtual void onVideoReset() {}
    virtual void onVideoFlush() {}

    virtual void onAudioConfig(const AudioConfig& cfg) = 0;

    /// 一帧音频载荷（AAC 裸帧 / Opus 裸包 / LPCM 裸 PCM），pts = 时间戳。
    virtual void onAudioFrame(const uint8_t* payload, size_t len, uint32_t pts) = 0;

    /// 音频流结束/重建。
    virtual void onAudioReset() {}

    virtual void onState(SessionState state, const std::string& detail) = 0;
};

// ---------------------------------------------------------------------------
// 会话与后端
// ---------------------------------------------------------------------------

class IProjectionSession {
public:
    virtual ~IProjectionSession() = default;

    /// 立即返回；握手/协商在内部线程进行，成功与否走 onState()。
    virtual bool start(std::string* err) = 0;

    /// 幂等。返回后不再有回调。
    virtual void stop() = 0;

    /// 发送输入。未到 Running 时返回 false。
    virtual bool sendInput(const InputEvent& ev, std::string* err) = 0;

    /// 请求一个关键帧（丢帧/恢复时用）。
    virtual void requestKeyFrame() = 0;

    virtual Capabilities capabilities() const = 0;
};

class IProjectionBackend {
public:
    virtual ~IProjectionBackend() = default;

    virtual Protocol    protocol() const = 0;
    virtual const char* name() const = 0;

    /// 发现可连接目标。可能较慢，由宿主决定在哪个线程调。
    /// services 用于取蓝牙等平台能力（无线 CarPlay 的候选目标就是已配对手机）。
    virtual std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                                std::string* err) = 0;

    /// 建立一次会话；失败返回 nullptr 并写 err。会话生命周期由调用方持有。
    /// services 由组合根注入（与 Receiver::start 同一个约定）。
    virtual std::unique_ptr<IProjectionSession> connect(
        const ProjectionTarget&          target,
        const core::PlatformServices&    services,
        IProjectionSink&                 sink,
        std::string*                     err) = 0;
};

/// 协议注册表：宿主按名字/枚举挑后端，不直接 include 任何后端头。
class BackendRegistry {
public:
    void add(std::unique_ptr<IProjectionBackend> backend);
    IProjectionBackend* find(Protocol p) const;
    const std::vector<std::unique_ptr<IProjectionBackend>>& all() const { return backends_; }

private:
    std::vector<std::unique_ptr<IProjectionBackend>> backends_;
};

}  // namespace core::projection
