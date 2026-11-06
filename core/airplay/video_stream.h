/**
 * @file   video_stream.h
 * @brief  屏幕流（独立 TCP 数据端口）
 * @author blt
 */

// ============================================================================
// core/airplay/video_stream.h —— 屏幕流（独立 TCP 数据端口）
// ============================================================================
// wire = [128B 头][body]；头 offset0 = u32LE bodyLen，offset4 = opcode
//（0=加密视频帧，1=明文配置帧），其余 120B 不解析、整体作 AAD。
// 密钥 = HKDF-SHA512(sharedSecret, "DataStream-Salt"+streamConnectionID,
//                    "DataStream-Output-Encryption-Key", 32)。
// 解密后明文 = 4B 大端长度前缀的 NALU(AVCC) → 转 AnnexB；关键帧闸门。
// 参照 AirPlay 屏幕流协议（ScreenCodec / MediaCodecSupport 的行为）。
#pragma once

#include "port/media.h"
#include "port/tcp.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstddef>

namespace core::airplay {

class ScreenStream {
public:
    /// listener 需已绑定（port 已知）。streamConnectionIdDecimal = 无符号十进制串。
    /// onNeedKeyFrame：喂帧失败（丢帧、参考链断裂）时回调，用于立刻向手机请求关键帧。
    ScreenStream(std::unique_ptr<core::ITcpListener> listener,
                 const std::vector<uint8_t>& sharedSecret,
                 const std::string& streamConnectionIdDecimal,
                 int width, int height, core::IVideoOutput* video,
                 std::function<void()> onNeedKeyFrame = {});
    ~ScreenStream();
    ScreenStream(const ScreenStream&) = delete;

    uint16_t port() const { return port_; }
    void stop();

    /// 关键帧恢复：重置解码器并重新放闸（丢关键帧后由 /command forceKeyFrame 触发）。
    void requestKeyFrame();

private:
    void run();
    void handleConnection(core::ITcpStream& stream);

    static bool detectConfig(const std::vector<uint8_t>& body, core::VideoCodec* codec,
                             std::vector<uint8_t>* codecData);
    /// 每 5s 打一次：发端到底给了多少帧/多少码率，我们这边拦了多少
    void reportWindow();

    static std::vector<uint8_t> avccToAnnexB(const std::vector<uint8_t>& p);
    static bool isRandomAccess(core::VideoCodec codec, const uint8_t* d, size_t n);

    std::unique_ptr<core::ITcpListener> listener_;
    std::vector<uint8_t> key_;           // 32 字节流密钥
    core::IVideoOutput* video_ = nullptr;
    std::function<void()> onNeedKeyFrame_;
    int  width_ = 1280, height_ = 720;
    uint16_t port_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;

    // 解析状态（单连接，在 run/连接线程内）
    uint64_t counter_ = 0;
    uint64_t frames_  = 0;   // 收到的视频帧计数（日志节流）
    bool configured_ = false;
    bool needsKeyFrame_ = true;

    // 5s 窗口统计：发端到底给了多少帧/多少码率，和我们这边拦了多少
    // （判断"卡"是发端供不上，还是我们消费不动，就看这三个数）
    uint64_t winFrames_ = 0;
    uint64_t winBytes_  = 0;
    uint64_t winGated_  = 0;   // 等关键帧期间被拦下的帧
    uint64_t winDrop_   = 0;   // 交不出去（解码器入队失败）的帧
    std::chrono::steady_clock::time_point winStart_ = std::chrono::steady_clock::now();
    core::VideoCodec codec_ = core::VideoCodec::H264;
};

}  // namespace core::airplay
