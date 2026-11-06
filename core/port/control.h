/**
 * @file   control.h
 * @brief  车机侧输入口：方控 / 媒体键
 * @author blt
 */

// ============================================================================
// core/port/control.h —— 车机侧输入口：方控 / 媒体键
// ============================================================================
// 方向是**车机 → 手机**：方向盘上的"上一首/下一首/播放暂停"、麦克风键。
// 与 media.h 正好相反（那是手机把画面和声音推给车机）。
//
// 这一层只定义"按键是什么"，不定义"按键怎么送出去"：
//   · 上层（鸿蒙 Index.ets、桌面宿主的按键处理、将来的硬件按键驱动）负责把
//     物理事件翻译成 MediaKey，按进 MediaKeyQueue；
//   · 协议层（core::iap2::WirelessTrigger / WiredLink）各自把队列里的键编码、
//     发到手机上 —— 无线的队伍在 run() 里排干，有线的由调用方在 pump 之间排干。
//
// 为什么不在这里做一次"发送"回调：iAP2 链路是**单线程**的（链路层的 seq/ack
// 与加密写计数都不能被两个线程碰），上层在 UI 线程直接发会撞车。队列是唯一
// 安全的那道门。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace core {

/// 方控按键。值本身没有协议含义，编码在 iap2/hid.h 里做映射。
enum class MediaKey : uint8_t {
    Play = 0,
    Pause,
    PlayPause,      ///< 播放/暂停同一个物理键（方向盘上最常见）
    Stop,
    NextTrack,
    PreviousTrack,
    FastForward,
    Rewind,
    VolumeUp,       ///< 音量：CarPlay 一般期望车机自己调本地音量，
    VolumeDown,     ///< 这两个留着是为了走"统一音量"的车型，默认别接
    Mute,
    Siri,           ///< 长按语音键唤醒 Siri
    Voice,          ///< 短按语音键（系统语音输入）
    PhoneAccept,    ///< 接听
    PhoneEnd,       ///< 挂断
    Home,           ///< 回主屏
};

/// 日志用（"NextTrack" → "下一首"）
const char* mediaKeyName(MediaKey key);

/// 一次按键：按下或抬起。成对的按下/抬起由上层决定什么时候产生
/// （多数按键是按下发一次、抬起发一次，用 0 usage 表示松手）。
struct MediaKeyEvent {
    MediaKey key  = MediaKey::PlayPause;
    bool     down = true;
};

/// 平台无关的按键出口。将来若出现"不走 iAP2 的方控通道"（比如某个车型用私有
/// CAN 转发），照着这个接口再写一个实现即可。
class IMediaKeySink {
public:
    virtual ~IMediaKeySink() = default;
    virtual bool send(const MediaKeyEvent& ev) = 0;
};

/// 线程安全的事件队列：上层 push，协议线程 pop。满了丢最老的（方控按键
/// 堆着没意义，松手比三秒前的"下一首"更该被发出去）。
class MediaKeyQueue {
public:
    static constexpr size_t kCapacity = 32;

    void   push(MediaKey key, bool down);
    void   push(const MediaKeyEvent& ev);
    bool   pop(MediaKeyEvent* out);          ///< 空队列返回 false
    void   clear();
    size_t size() const;

private:
    mutable std::mutex        mu_;
    std::deque<MediaKeyEvent> q_;
};

}  // namespace core
