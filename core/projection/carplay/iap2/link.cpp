#include "iap2/link.h"

namespace core::iap2 {

namespace {

const uint8_t  kMarker[6]      = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
constexpr int  kHeaderBytes    = 9;
constexpr int  kChecksumBytes  = 1;
constexpr uint8_t kCtrlSyn     = 0x80;
constexpr uint8_t kCtrlAck     = 0x40;
constexpr uint8_t kCtrlEak     = 0x20;
constexpr uint8_t kCtrlReset   = 0x10;
constexpr uint8_t kSessionCtrl = 10;
constexpr uint8_t kSessionEa   = 11;
constexpr uint8_t kSessionFile = 12;
constexpr uint64_t kMarkerResendMs = 1000;
constexpr uint64_t kSyncResendMs   = 500;
constexpr int  kAckSlack           = 10;
constexpr size_t kMaxQueuedControl = 64;

uint8_t seqNext(uint8_t s) { return uint8_t(s + 1); }
int     seqDist(uint8_t a, uint8_t b) { return (int(uint8_t(a - b))); }   // (a-b) mod 256

uint8_t checksumNeg(const uint8_t* p, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return uint8_t(0u - sum);
}

}  // namespace

LinkEngine::LinkEngine(LinkConfig cfg) : cfg_(cfg) {}

void LinkEngine::die(const std::string& why)
{
    if (state_ == State::Dead) return;
    state_ = State::Dead;
    deadReason_ = why;
    output_.clear();
    unacked_.clear();
    queued_.clear();
}

void LinkEngine::appendOutput(const std::vector<uint8_t>& bytes)
{
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void LinkEngine::writeFrame(uint8_t control, uint8_t seq, uint8_t session,
                            const std::vector<uint8_t>* payload)
{
    // 空载荷帧（纯 ACK）**不带**载荷校验字节，长度就是 9 —— 这一点写错会让
    // 长度字段和实际字节数差 1，整个流从此错位（症状：两端一直重发 SYN/ACK）。
    const size_t len = kHeaderBytes + (payload ? payload->size() + kChecksumBytes : 0);
    std::vector<uint8_t> f;
    f.reserve(len);
    f.push_back(0xff);
    f.push_back(0x5a);
    f.push_back(uint8_t(len >> 8));
    f.push_back(uint8_t(len));
    f.push_back(control);
    f.push_back(seq);
    f.push_back(lastReceivedInOrder_);
    f.push_back(session);
    f.push_back(checksumNeg(f.data(), 8));
    if (payload) {
        f.insert(f.end(), payload->begin(), payload->end());
        f.push_back(checksumNeg(payload->data(), payload->size()));
    }
    appendOutput(f);
    pendingAcks_ = 0;   // 出帧即携带了累积确认
}

void LinkEngine::start(uint64_t nowMs)
{
    if (state_ != State::Idle) return;
    state_ = State::Detecting;
    appendOutput(std::vector<uint8_t>(kMarker, kMarker + sizeof kMarker));
    markerDeadlineMs_ = nowMs + kMarkerResendMs;
    // 主动发起（有线）：marker 只发这一次，紧接着自己进协商、开始发 SYN ——
    // 不等对端 marker。有线这条路上对端不会先开口。
    if (cfg_.initiator) enterNegotiating(nowMs);
}

/// 进协商态：停掉 marker 重发，开始发 SYN 并按 kSyncResendMs 重发（在 tick 里）。
void LinkEngine::enterNegotiating(uint64_t nowMs)
{
    if (state_ == State::Dead) return;
    state_ = State::Negotiating;
    markerDeadlineMs_ = 0;
    sendSynchronization();
    syncDeadlineMs_ = nowMs + kSyncResendMs;
}

void LinkEngine::sendSynchronization()
{
    // SYN 载荷 v1（10 字节固定 + 每会话 3 字节）
    std::vector<uint8_t> p;
    p.push_back(1);                          // version
    p.push_back(cfg_.maxOutgoing);
    p.push_back(uint8_t(0xff)); p.push_back(uint8_t(0xff));   // maxLength = 0xffff
    p.push_back(uint8_t(cfg_.retransmissionTimeoutMs >> 8));
    p.push_back(uint8_t(cfg_.retransmissionTimeoutMs));
    p.push_back(uint8_t(cfg_.ackTimeoutMs >> 8));
    p.push_back(uint8_t(cfg_.ackTimeoutMs));
    p.push_back(cfg_.maxRetransmissions);
    p.push_back(cfg_.maxAcknowledgements);
    p.push_back(kSessionCtrl); p.push_back(0); p.push_back(cfg_.controlSessionVersion);
    p.push_back(kSessionEa);   p.push_back(2); p.push_back(1);
    p.push_back(kSessionFile); p.push_back(1); p.push_back(2);
    writeFrame(kCtrlSyn, sentSeq_, 0, &p);
}

void LinkEngine::sendAckOnly()
{
    writeFrame(kCtrlAck, sentSeq_, 0, nullptr);
}

void LinkEngine::sendData(uint8_t seq, uint8_t session, const std::vector<uint8_t>& payload)
{
    writeFrame(kCtrlAck, seq, session, &payload);
}

bool LinkEngine::takeOutput(std::vector<uint8_t>* out)
{
    if (output_.empty()) return false;
    out->insert(out->end(), output_.begin(), output_.end());
    output_.clear();
    return true;
}

bool LinkEngine::nextControl(std::vector<uint8_t>* payload)
{
    if (controlQueue_.empty()) return false;
    *payload = std::move(controlQueue_.front());
    controlQueue_.pop_front();
    return true;
}

size_t LinkEngine::peerMaxPayload() const
{
    if (!peerSyncOk_ || peerMaxLength_ <= kHeaderBytes + kChecksumBytes) return 512;
    return peerMaxLength_ - kHeaderBytes - kChecksumBytes;
}

std::string LinkEngine::peerSummary() const
{
    if (!peerSyncOk_) return "（还没收到对端 SYN）";
    return "maxOutgoing=" + std::to_string(peerMaxOutgoing_) +
           " maxLength=" + std::to_string(peerMaxLength_) +
           " 重传超时=" + std::to_string(peerRetransTimeoutMs_) + "ms" +
           " maxRetrans=" + std::to_string(peerMaxRetransmissions_) +
           " maxAcks=" + std::to_string(peerMaxAcks_);
}

bool LinkEngine::sendControl(const std::vector<uint8_t>& payload, uint64_t nowMs)
{
    if (state_ == State::Dead) return false;
    if (state_ == State::Normal && seqDist(sentSeq_, lastSentAcked_) < peerMaxOutgoing_) {
        sentSeq_ = seqNext(sentSeq_);
        OutPacket p;
        p.seq = sentSeq_;
        p.session = kSessionCtrl;
        p.payload = payload;
        p.deadlineMs = nowMs + peerRetransTimeoutMs_;
        sendData(p.seq, p.session, p.payload);
        if (peerMaxRetransmissions_ > 0) unacked_.push_back(p);
        else lastSentAcked_ = sentSeq_;
        return true;
    }
    if (queued_.size() >= kMaxQueuedControl) {
        die("iAP2 发送队列溢出");
        return false;
    }
    OutPacket p;
    p.session = kSessionCtrl;
    p.payload = payload;
    queued_.push_back(std::move(p));
    return true;
}

void LinkEngine::flushQueued(uint64_t nowMs)
{
    while (!queued_.empty() &&
           seqDist(sentSeq_, lastSentAcked_) < peerMaxOutgoing_) {
        OutPacket p = queued_.front();
        queued_.pop_front();
        sentSeq_ = seqNext(sentSeq_);
        p.seq = sentSeq_;
        p.deadlineMs = nowMs + peerRetransTimeoutMs_;
        sendData(p.seq, p.session, p.payload);
        if (peerMaxRetransmissions_ > 0) unacked_.push_back(p);
        else lastSentAcked_ = sentSeq_;
    }
}

// ---------------------------------------------------------------------------
// 收方向
// ---------------------------------------------------------------------------

void LinkEngine::feed(const uint8_t* p, size_t n, uint64_t nowMs)
{
    if (state_ == State::Dead || n == 0) return;
    rx_.insert(rx_.end(), p, p + n);
    while (parseOne(nowMs) && state_ != State::Dead) {}
}

bool LinkEngine::parseOne(uint64_t nowMs)
{
    if (state_ == State::Detecting) {
        if (rx_.size() < sizeof kMarker) return false;
        if (std::equal(kMarker, kMarker + sizeof kMarker, rx_.begin())) {
            rx_.erase(rx_.begin(), rx_.begin() + sizeof kMarker);
            enterNegotiating(nowMs);
            return true;
        }
        // 不匹配：找下一个可能的 marker 起点（丢 1 字节重试）
        rx_.erase(rx_.begin());
        return !rx_.empty();
    }

    // 对帧：找 FF 5A
    size_t off = 0;
    while (rx_.size() - off >= 2 && !(rx_[off] == 0xff && rx_[off + 1] == 0x5a)) ++off;
    if (off) rx_.erase(rx_.begin(), rx_.begin() + off);
    if (rx_.size() < kHeaderBytes) return false;

    const size_t len = (size_t(rx_[2]) << 8) | rx_[3];
    if (len < kHeaderBytes || len > 0xffff) {
        rx_.erase(rx_.begin());     // 头不合法：丢 1 字节重新对齐
        return true;
    }
    if (rx_.size() < len) return false;

    unsigned hsum = 0;
    for (int i = 0; i < kHeaderBytes; ++i) hsum += rx_[i];
    if ((hsum & 0xff) != 0) {
        rx_.erase(rx_.begin());
        return true;
    }
    if (len > kHeaderBytes) {
        unsigned psum = 0;
        for (size_t i = kHeaderBytes; i < len; ++i) psum += rx_[i];
        if ((psum & 0xff) != 0) {
            rx_.erase(rx_.begin(), rx_.begin() + len);
            return true;
        }
    }

    Frame f;
    f.length  = uint16_t(len);
    f.control = rx_[4];
    f.seq     = rx_[5];
    f.ack     = rx_[6];
    f.session = rx_[7];
    if (len > kHeaderBytes + kChecksumBytes) {
        f.hasPayload = true;
        f.payload.assign(rx_.begin() + kHeaderBytes, rx_.begin() + len - kChecksumBytes);
    }
    rx_.erase(rx_.begin(), rx_.begin() + len);
    handleFrame(f, nowMs);
    return true;
}

void LinkEngine::handleFrame(const Frame& f, uint64_t nowMs)
{
    if (f.control & kCtrlReset) {
        die("对端发了 iAP2 reset");
        return;
    }
    if ((f.control & kCtrlSyn) && f.hasPayload) handleSync(f);
    if (f.control & kCtrlAck) handleAckField(f.ack, nowMs);
    if (f.control == kCtrlAck && f.hasPayload) handleData(f, nowMs);
    else if (f.control == kCtrlAck && !f.hasPayload) {
        // 纯 ACK：确认到位后可能要冲队列
        if (state_ == State::Normal) flushQueued(nowMs);
    }
    (void)kCtrlEak;
}

void LinkEngine::handleSync(const Frame& f)
{
    if (state_ != State::Negotiating) return;
    const auto& p = f.payload;
    if (p.size() < 10 || p[0] != 1) return;
    peerMaxOutgoing_ = p[1];
    peerMaxLength_ = uint16_t((uint16_t(p[2]) << 8) | p[3]);
    peerRetransTimeoutMs_ = uint16_t((uint16_t(p[4]) << 8) | p[5]);
    peerAckTimeoutMs_ = uint16_t((uint16_t(p[6]) << 8) | p[7]);
    peerMaxRetransmissions_ = p[8];
    peerMaxAcks_ = p[9];
    peerSyncOk_ = true;
    lastReceivedInOrder_ = f.seq;
    haveReceived_ = true;
    sendAckOnly();
}

void LinkEngine::handleAckField(uint8_t ack, uint64_t nowMs)
{
    if (state_ == State::Negotiating) {
        if (!peerSyncOk_ || peerMaxLength_ <= kHeaderBytes + kChecksumBytes ||
            peerMaxOutgoing_ == 0) {
            die("对端 iAP2 参数不可用");
            return;
        }
        state_ = State::Normal;
        syncDeadlineMs_ = 0;
    }
    if (state_ != State::Normal) return;

    lastSentAcked_ = ack;
    // 丢已确认的
    for (auto it = unacked_.begin(); it != unacked_.end();) {
        const int d = seqDist(it->seq, lastSentAcked_);
        if (d > 0 && d <= peerMaxAcks_ + kAckSlack) {
            it->deadlineMs = it->deadlineMs;   // 保持
            ++it;
        } else {
            it = unacked_.erase(it);
        }
    }
    flushQueued(nowMs);
}

void LinkEngine::handleData(const Frame& f, uint64_t nowMs)
{
    if (state_ != State::Normal) return;
    const int d = haveReceived_ ? seqDist(f.seq, lastReceivedInOrder_)
                                : 0;
    if (!haveReceived_) {
        // 本端还没收到过任何数据（可能首个数据帧先于对端 SYN 被吃）：接受为起点
        lastReceivedInOrder_ = f.seq;
        haveReceived_ = true;
        controlQueue_.push_back(f.payload);
        ++pendingAcks_;
    } else if (d == 0) {
        sendAckOnly();          // 重复帧：立刻确认
        return;
    } else if (d == 1) {
        lastReceivedInOrder_ = f.seq;
        controlQueue_.push_back(f.payload);
        ++pendingAcks_;
        // 顺带处理缓存里接上的乱序帧
        bool progressed = true;
        while (progressed) {
            progressed = false;
            for (auto it = outOfOrder_.begin(); it != outOfOrder_.end(); ++it) {
                if (seqDist(it->first, lastReceivedInOrder_) == 1) {
                    lastReceivedInOrder_ = it->first;
                    controlQueue_.push_back(it->second);
                    outOfOrder_.erase(it);
                    ++pendingAcks_;
                    progressed = true;
                    break;
                }
            }
        }
    } else {
        // 乱序：缓存（去重），并立刻 ACK 促使对端重传缺口
        bool dup = false;
        for (const auto& kv : outOfOrder_) if (kv.first == f.seq) { dup = true; break; }
        if (!dup && outOfOrder_.size() < 64) outOfOrder_.emplace_back(f.seq, f.payload);
        sendAckOnly();
        return;
    }

    const uint8_t threshold = peerMaxAcks_ > 0 ? peerMaxAcks_ : 1;
    if (pendingAcks_ >= threshold) sendAckOnly();
    (void)nowMs;
}

// ---------------------------------------------------------------------------
// 时间推进
// ---------------------------------------------------------------------------

void LinkEngine::tick(uint64_t nowMs)
{
    if (state_ == State::Dead) return;

    if (state_ == State::Detecting && markerDeadlineMs_ &&
        nowMs >= markerDeadlineMs_) {
        appendOutput(std::vector<uint8_t>(kMarker, kMarker + sizeof kMarker));
        markerDeadlineMs_ = nowMs + kMarkerResendMs;
    }

    if (state_ == State::Negotiating && syncDeadlineMs_ && nowMs >= syncDeadlineMs_) {
        sendSynchronization();
        syncDeadlineMs_ = nowMs + kSyncResendMs;
    }

    if (state_ == State::Normal) {
        // 重传到期
        for (auto it = unacked_.begin(); it != unacked_.end();) {
            if (nowMs >= it->deadlineMs) {
                ++it->retransmissions;
                if (it->retransmissions > peerMaxRetransmissions_) {
                    die("iAP2 数据帧未被确认（seq " + std::to_string(it->seq) + "）");
                    return;
                }
                sendData(it->seq, it->session, it->payload);
                it->deadlineMs = nowMs + (peerRetransTimeoutMs_ ? peerRetransTimeoutMs_ : 4000);
            }
            ++it;
        }
    }
}

}  // namespace core::iap2
