/**
 * @file   link.h
 * @brief  iAP2 链路层（marker / 9 字节帧 / 校验和 / seq-ack / 同步握手）
 * @author blt
 */

// ============================================================================
// core/iap2/link.h —— iAP2 链路层（marker / 9 字节帧 / 校验和 / seq-ack / 同步握手）
// ============================================================================
// 协议事实（对照 iAP2 链路层行为整理）：
//   · 起始 marker：FF 55 02 00 EE 10；DETECTING 期间每 1s 重发，直到收到对端 marker
//   · 数据帧 9 字节头：FF 5A | u16 帧长 | 控制 | seq | ack | session | 校验
//     帧长 = 9 + 载荷长 + 1(载荷校验)；头校验 = 前 8 字节之和取负（整帧和 ≡ 0 mod 256）
//     载荷校验 = 载荷和取负，附在载荷后
//   · 控制位：SYN 0x80 / ACK 0x40 / EAK 0x20 / RST 0x10；数据帧控制=0x40 且带 session
//   · 每个出帧的 ack 字段 = 自己已按序收到的最后 seq（累积确认）
//   · 初始 seq = 99（u8 回绕）；session 10=控制会话（CSM 走这里）
//   · SYN 载荷 v1：ver(1B)=1 | maxOutgoing | u16 maxLength | u16 重传超时ms |
//     u16 ack 超时ms | maxRetransmissions | maxAcknowledgements | 每会话 3B(id,kind,ver)
//   · NEGOTIATING 收到对端 SYN → 记参数 + 发 ACK → 收到对端 ACK → NORMAL
//   · 数据重传：超时重发，超 maxRetransmissions 判死
//
// 本实现是"够用且兼容"的子集：窗口/重传/累积 ACK 都实现，但省略了
// EAK 乱序补洞的细节（遇到缺口只发 ACK，等对端重传）。行为对齐见 tools/iap2_test.cpp。
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <cstddef>

namespace core::iap2 {

/// 本地链路参数（放在命名空间层：类内嵌套类型带 NSDMI 时不能做默认实参）
struct LinkConfig {
    uint8_t  maxOutgoing            = 30;
    uint16_t ackTimeoutMs           = 500;
    uint16_t retransmissionTimeoutMs = 4000;
    uint8_t  maxRetransmissions     = 4;
    uint8_t  maxAcknowledgements    = 3;
    uint8_t  controlSessionVersion  = 1;

    /// 主机主动发起协商（有线 = true；无线 = false）。
    /// 无线是手机先发 marker、本端回 SYN；有线反过来 —— 主机发完 marker **立刻自己发 SYN**，
    /// 不等对端 marker。搞反了的现象很好认：marker 发出去之后对端一直沉默，什么都没回来。
    bool     initiator              = false;
};

class LinkEngine {
public:
    explicit LinkEngine(LinkConfig cfg = LinkConfig());

    // ---- 驱动（单线程使用；时间由调用方给单调毫秒）----
    void start(uint64_t nowMs);
    void feed(const uint8_t* p, size_t n, uint64_t nowMs);
    void tick(uint64_t nowMs);

    // ---- 输出 ----
    bool takeOutput(std::vector<uint8_t>* out);          ///< 取走待发字节（写到传输层）
    bool nextControl(std::vector<uint8_t>* payload);     ///< 取一条完整的控制会话(CSM)载荷

    /// 发一条控制会话载荷（未就绪时会排队；超过 64 条判死）
    bool sendControl(const std::vector<uint8_t>& payload, uint64_t nowMs);

    // ---- 状态 ----
    bool ready() const { return state_ == State::Normal; }
    bool dead() const { return state_ == State::Dead; }
    const std::string& deadReason() const { return deadReason_; }

    /// 对端可承载的最大控制载荷（对端 maxLength - 10），未协商时给保守值
    size_t peerMaxPayload() const;

    /// 对端 SYN 里报的那几个参数，一行摘要。协商卡住时第一个要看的就是它。
    std::string peerSummary() const;

private:
    enum class State { Idle, Detecting, Negotiating, Normal, Dead };

    struct Frame {
        uint16_t length = 0;
        uint8_t  control = 0;
        uint8_t  seq = 0;
        uint8_t  ack = 0;
        uint8_t  session = 0;
        std::vector<uint8_t> payload;   // 无载荷 = 空
        bool     hasPayload = false;
    };

    void die(const std::string& why);
    void enterNegotiating(uint64_t nowMs);
    void appendOutput(const std::vector<uint8_t>& bytes);
    void writeFrame(uint8_t control, uint8_t seq, uint8_t session,
                    const std::vector<uint8_t>* payload);
    void sendSynchronization();
    void sendAckOnly();
    void sendData(uint8_t seq, uint8_t session, const std::vector<uint8_t>& payload);

    bool parseOne(uint64_t nowMs);
    void handleFrame(const Frame& f, uint64_t nowMs);
    void handleSync(const Frame& f);
    void handleAckField(uint8_t ack, uint64_t nowMs);
    void handleData(const Frame& f, uint64_t nowMs);
    void flushQueued(uint64_t nowMs);

    struct OutPacket {
        uint8_t seq = 0;
        uint8_t session = 0;
        std::vector<uint8_t> payload;
        int     retransmissions = 0;
        uint64_t deadlineMs = 0;
    };

    LinkConfig cfg_;
    State      state_ = State::Idle;

    // 接收
    std::vector<uint8_t> rx_;

    // 对端同步参数（收到 SYN 后有效）
    bool     peerSyncOk_ = false;
    uint8_t  peerMaxOutgoing_ = 30;
    uint16_t peerMaxLength_ = 0xffff;
    uint16_t peerRetransTimeoutMs_ = 4000;
    uint16_t peerAckTimeoutMs_ = 500;
    uint8_t  peerMaxRetransmissions_ = 4;
    uint8_t  peerMaxAcks_ = 3;

    // 序号/确认
    uint8_t  sentSeq_ = 99;
    uint8_t  lastSentAcked_ = 99;
    uint8_t  lastReceivedInOrder_ = 0;
    bool     haveReceived_ = false;
    int      pendingAcks_ = 0;

    // 乱序缓存（seq → 载荷）
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> outOfOrder_;

    std::deque<std::vector<uint8_t>> controlQueue_;   // 待交付的完整控制载荷
    std::vector<OutPacket> unacked_;
    std::deque<OutPacket>  queued_;
    std::vector<uint8_t>   output_;

    uint64_t markerDeadlineMs_ = 0;
    uint64_t syncDeadlineMs_ = 0;
    std::string deadReason_;
};

}  // namespace core::iap2
