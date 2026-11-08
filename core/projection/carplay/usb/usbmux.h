/**
 * @file   usbmux.h
 * @brief  usbmux v2 + 迷你 TCP：在 USBMUX 那条 bulk 管道上开出多条"TCP 连接"
 * @author blt
 */

// ============================================================================
// core/usb/usbmux.h —— usbmux v2 + 迷你 TCP
// ============================================================================
// 有线 CarPlay 的控制面从这条管道开始：
//
//   USBMUX 口(bulk) → usbmux v2 帧 → 迷你 TCP → 手机:62078(lockdown)
//                                            → 手机:carkit 服务端口
//
// 它是一个**极简**的 TCP：没有重传、没有滑窗、没有乱序重组、checksum 恒为 0、
// 窗口恒为 512 —— 因为对端是同一台机器用同一套简化的栈在说话，而且底下是 USB bulk，
// 本来就不丢包。**别拿它当通用 TCP 用。**
//
// 多路复用**只按"本端源端口"**：本端从 1 开始分配，收到帧时用 TCP 头的**目的端口**
// 字段（对端把我们本端的源端口回填在那儿）查表投递，查不到就丢弃。没有别的连接标识。
//
// 分帧靠 MUX 头偏移 4 的 length（整帧长度，含 16 字节头，合法范围 16..65536）。
// 另有一个历史怪癖：个别回复会比声明的多出 4 字节，只允许在"版本应答"和
// "无 payload 的 SYN-ACK"之后、且下一帧确实是 TCP 帧时跳过 —— 见 takeFrame。
#pragma once

#include "port/stream.h"
#include "usb/mux.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core::usb {

class UsbMuxChannel;

/// 一条物理 USBMUX 管道上的 v2 会话。**不拥有**管道（管道由调用方持有，且必须比它活得久）。
class UsbMux : public IMux {
public:
    explicit UsbMux(IByteStream& pipe);
    ~UsbMux();

    UsbMux(const UsbMux&) = delete;
    UsbMux& operator=(const UsbMux&) = delete;

    /// 版本协商 + setup。任何 connectTcp 之前必须先调一次。
    /// 失败返回 false，err 里是给人看的原因。
    bool begin(std::string* err) override;

    /// 开一条到手机 port 的迷你 TCP（典型：62078 = lockdown）。
    /// 成功返回该连接的字节流（归调用方所有）；失败返回 nullptr。
    ///
    /// ⚠️ 返回的流**必须比本对象先析构** —— 它内部会回调本对象发 ACK。
    std::unique_ptr<IByteStream> connectTcp(uint16_t port, int timeoutMs,
                                            std::string* err) override;

private:
    friend class UsbMuxChannel;

    struct Frame {
        uint32_t            protocol = 0;
        uint32_t            length = 0;     ///< 整帧长度（含 16 字节头）
        uint32_t            word8 = 0;      ///< 偏移 8：版本应答里是版本号，TCP 帧里是 0xfaceface
        uint16_t            sequence = 0;
        uint16_t            ack = 0;
        std::vector<uint8_t> payload;
    };

    void readerLoop();
    void handleFrame(const Frame& f);
    void dispatchTcp(const Frame& f);
    void failAll(const char* why);
    bool isClosed();

    /// 从 rx_ 里取一帧（调用方须持 rxMu_）。不足一帧返回 false。
    bool takeFrame(Frame* out);

    /// 发一个 MUX 帧。word8 只有版本消息是版本号，其余都是 kMuxMagic。
    bool sendFrame(uint32_t protocol, uint32_t word8, const uint8_t* payload, size_t n);

    /// 发一个迷你 TCP 段（payload 可为空）
    bool sendTcp(uint16_t srcPort, uint16_t dstPort, uint32_t seq, uint32_t ack,
                 uint8_t flags, const uint8_t* payload, size_t n);

    void removeChannel(uint16_t localPort);

    IByteStream&                       pipe_;
    std::thread                        reader_;

    std::mutex                         rxMu_;
    std::vector<uint8_t>               rx_;             ///< 未成帧的残留字节
    bool                               prevAllowsPadding_ = false;

    std::mutex                         writeMu_;        ///< 串行化对管道的写
    uint16_t                           nextMuxSeq_ = 0; ///< MUX 层序号（版本消息用掉 0）
    uint16_t                           muxAck_ = 0;

    std::mutex                         chanMu_;
    std::map<uint16_t, UsbMuxChannel*> channels_;
    uint16_t                           nextSourcePort_ = 1;

    std::mutex                         hsMu_;
    std::condition_variable            hsCv_;
    bool                               versionReplied_ = false;
    bool                               handshakeFailed_ = false;
    int                                staleFrames_ = 0;
    bool                               handshakeDone_ = false;

    std::mutex                         stateMu_;
    std::condition_variable            stateCv_;
    bool                               closed_ = false;
};

}  // namespace core::usb
