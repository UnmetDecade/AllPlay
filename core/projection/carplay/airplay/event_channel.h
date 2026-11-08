/**
 * @file   event_channel.h
 * @brief  事件通道（M4：HID/触摸回传 + 命令）
 * @author blt
 */

// ============================================================================
// core/airplay/event_channel.h —— 事件通道（M4：HID/触摸回传 + 命令）
// ============================================================================
// iPhone 连到 SETUP 响应里的 eventPort。通道用 ControlCipher 加密：
//   密钥 = HKDF-SHA512(sharedSecret, "Events-Salt", "Events-*", 32)
//   写方向 = "Events-Write-Encryption-Key"，读方向 = "Events-Read-Encryption-Key"
// 触摸：把触点编成 HID report，作为 POST /command 的 bplist 体发过去
//   {"type":"hidSendReport","uuid":"2a2a2a2a","hidReport":<12B report>}
// 参照 AirPlay 事件通道（sendCommand/sendTouch + HID touchReport）。
#pragma once

#include "airplay/control_cipher.h"
#include "airplay/rtsp.h"
#include "port/tcp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core::bplist { class Value; }

namespace core::airplay {

/// 一个触点（坐标归一化 0..1，相对主屏分辨率）。
struct TouchPoint {
    float x = 0;
    float y = 0;
    bool  down = false;
};

class EventChannel {
public:
    /// listener 需已绑定（port 已知）。sharedSecret = pair-verify 的 shared。
    EventChannel(std::unique_ptr<core::ITcpListener> listener,
                 const std::vector<uint8_t>& sharedSecret,
                 int width, int height);
    ~EventChannel();
    EventChannel(const EventChannel&) = delete;

    uint16_t port() const { return port_; }
    void stop();

    /// 换用新一场 pair-verify 的 sharedSecret 重算读写密钥，并踢掉旧连接。
    /// 通道对象跨会话复用（监听端口不变），但密钥是**每场会话**的 —— 不换的话，
    /// 宿主发给手机的包手机一侧解不开，iOS 会把整个 CarPlay 会话掐断。
    void rekey(const std::vector<uint8_t>& sharedSecret);

    /// 发触摸（归一化坐标 → 缩放成像素 → HID report → 加密 POST /command）。
    bool sendTouch(const std::vector<TouchPoint>& contacts);

    /// 发任意命令（bplist dict）。
    bool sendCommand(const std::shared_ptr<core::bplist::Value>& cmd);

private:
    void run();   // accept + 读侧（解密 + 解析 + 回 200 ACK）
    /// 独立的写线程：加密 + 发出去。见 .cpp 里"为什么不能让调用方自己写"。
    void writeLoop();
    /// 入队一段待发明文。非阻塞 —— 触摸是 UI 线程调的，绝不能在这里等 socket。
    bool submit(std::vector<uint8_t> plain, bool droppable);
    bool sendCommandImpl(const std::shared_ptr<core::bplist::Value>& cmd, bool droppable);

    struct OutMsg {
        std::vector<uint8_t> plain;
        bool droppable = false;   ///< true = 触摸这种"新的覆盖旧的"的包，队列挤时可丢
    };

    std::unique_ptr<core::ITcpListener> listener_;
    int  width_ = 1280, height_ = 720;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::thread writer_;

    std::mutex mu_;                          // 保护 stream_/cipher_/cseq_/plain_
    std::shared_ptr<core::ITcpStream> stream_;
    std::shared_ptr<ControlCipher> cipher_;
    uint64_t cseq_ = 0;
    std::vector<uint8_t> writeKey_, readKey_;
    std::vector<uint8_t> plain_;             // 读侧解密后的明文缓冲（未凑整帧的部分）

    std::mutex outMu_;                       // 保护发送队列
    std::condition_variable outCv_;
    std::deque<OutMsg> out_;
    size_t outBytes_ = 0;
    std::atomic<int> writeFails_{0};
};

}  // namespace core::airplay
