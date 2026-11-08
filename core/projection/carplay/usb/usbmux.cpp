/**
 * @file   usbmux.cpp
 * @brief  usbmux v2 + 迷你 TCP 的实现
 * @author blt
 */

#include "usb/usbmux.h"

#include "common/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>

namespace core::usb {
namespace {

constexpr const char* kTag = "usbmux";

// ---- MUX 层 ----
constexpr size_t   kMuxHeader     = 16;
constexpr uint32_t kMuxMagic      = 0xfeedface;  ///< 主机发出去的每帧，偏移 8 都是它
constexpr uint32_t kReplyMagic    = 0xfaceface;  ///< 手机回的 TCP 帧，偏移 8 是它
constexpr uint32_t kProtoVersion  = 0;
constexpr uint32_t kProtoControl  = 1;   ///< 手机发来的控制/报错消息（载荷是给人看的文本）
constexpr uint32_t kProtoSetup    = 2;
constexpr uint32_t kProtoTcp      = 6;
constexpr uint32_t kUsbMuxVersion = 2;
constexpr size_t   kVersionBytes  = 20;      ///< 版本消息/应答的整帧长度
constexpr uint8_t  kSetupValue    = 0x07;
constexpr uint32_t kMaxFrame      = 65536;
constexpr int      kHandshakeTimeoutMs = 60000;
constexpr int      kStaleFrameLimit    = 32; ///< 握手期间最多容忍这么多陈旧 TCP 帧
constexpr int      kReadPollMs    = 100;
constexpr size_t   kReadChunk     = 64 * 1024;

// ---- 迷你 TCP ----
constexpr size_t   kTcpHeader      = 20;
constexpr uint16_t kTcpWindow      = 512;
constexpr uint8_t  kTcpFin = 0x01, kTcpSyn = 0x02, kTcpRst = 0x04, kTcpAck = 0x10;
constexpr uint8_t  kTcpSynAck      = kTcpSyn | kTcpAck;
constexpr uint8_t  kTcpFinAck      = kTcpFin | kTcpAck;
constexpr size_t   kMaxSendPayload = 16 * 1024;

// 全部字段都是大端
uint16_t rdU16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])); }
uint32_t rdU32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void wrU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void wrU32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

/// 十六进制 dump（只用于 debug 档，排查"对端发了什么我们没看懂"）
std::string hexDump(const std::vector<uint8_t>& v, size_t maxBytes)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    const size_t n = std::min(maxBytes, v.size());
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) out += (i % 16 == 0) ? '\n' : ' ';
        out += kHex[v[i] >> 4];
        out += kHex[v[i] & 0x0f];
    }
    return out;
}

}  // namespace

// ============================================================================
// 一条迷你 TCP 连接。对上层就是个 IByteStream。
// ============================================================================
class UsbMuxChannel final : public IByteStream {
public:
    UsbMuxChannel(UsbMux& owner, uint16_t localPort, uint16_t remotePort)
        : owner_(&owner), localPort_(localPort), remotePort_(remotePort) {}

    ~UsbMuxChannel() override
    {
        close();
        if (owner_ != nullptr) owner_->removeChannel(localPort_);
    }

    int  read(uint8_t* buf, size_t n, int timeoutMs) override;
    bool writeAll(const uint8_t* buf, size_t n) override;
    void close() override;

    /// UsbMux 的读线程调用：投递一个 TCP 段。tcp 指向段首，total 是整段长度（含 TCP 头）。
    void onSegment(const uint8_t* tcp, size_t headerBytes, size_t total, uint32_t seq, uint8_t flags);

    /// 等 SYN-ACK（connectTcp 用）
    bool waitConnected(int timeoutMs);

    /// 管道断了：叫醒所有等待者
    void markBroken();

    /// UsbMux 正在析构：断开回指，之后析构也别再去碰那个死掉的对象
    void orphan();

private:
    void sendControl(uint8_t flags);

    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<uint8_t>     rx_;
    UsbMux*                 owner_;
    const uint16_t          localPort_;
    const uint16_t          remotePort_;
    uint32_t                nextSeq_ = 0;
    uint32_t                nextAck_ = 0;
    bool                    connected_ = false;
    bool                    eof_ = false;     ///< 对端 FIN
    bool                    reset_ = false;   ///< 对端 RST / 管道断
    bool                    rstLogged_ = false;  ///< RST 只报一次，见 read()
    bool                    closed_ = false;  ///< 本端关
};

void UsbMuxChannel::sendControl(uint8_t flags)
{
    uint32_t seq = 0, ack = 0;
    UsbMux* owner = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        seq = nextSeq_;
        ack = nextAck_;
        owner = owner_;
    }
    if (owner != nullptr) owner->sendTcp(localPort_, remotePort_, seq, ack, flags, nullptr, 0);
}

int UsbMuxChannel::read(uint8_t* buf, size_t n, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [this] { return !rx_.empty() || eof_ || reset_ || closed_; });

    if (!rx_.empty()) {
        const size_t take = std::min(n, rx_.size());
        for (size_t i = 0; i < take; ++i) {
            buf[i] = rx_.front();
            rx_.pop_front();
        }
        return int(take);
    }
    if (closed_) return -1;
    if (reset_) {
        // 只报一次：read() 会被上层反复调用（TLS 重试、轮询），每次都打就是刷屏 ——
        // 真机上这个循环曾经刷出两千万行。
        if (!rstLogged_) {
            rstLogged_ = true;
            LOG_WARN("[%s] 本端 %u ← 手机 %u 收到 RST", kTag, localPort_, remotePort_);
        }
        return -1;
    }
    if (eof_) return -1;
    return 0;  // 超时
}

bool UsbMuxChannel::writeAll(const uint8_t* buf, size_t n)
{
    if (n == 0) return true;

    std::lock_guard<std::mutex> lk(mu_);
    if (owner_ == nullptr || closed_ || reset_) return false;

    // 底下的 UL 是 USB bulk，不丢包；但单帧有上限，长的自己切成若干段
    size_t off = 0;
    while (off < n) {
        const size_t chunk = std::min(n - off, kMaxSendPayload);
        if (!owner_->sendTcp(localPort_, remotePort_, nextSeq_, nextAck_, kTcpAck, buf + off, chunk))
            return false;
        nextSeq_ += uint32_t(chunk);
        off += chunk;
    }
    return true;
}

void UsbMuxChannel::close()
{
    uint32_t seq = 0, ack = 0;
    UsbMux* owner = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        closed_ = true;
        seq = nextSeq_;
        ack = nextAck_;
        owner = owner_;
    }
    cv_.notify_all();
    if (owner != nullptr) owner->sendTcp(localPort_, remotePort_, seq, ack, kTcpFinAck, nullptr, 0);
}

void UsbMuxChannel::onSegment(const uint8_t* tcp, size_t headerBytes, size_t total, uint32_t seq,
                              uint8_t flags)
{
    if ((flags & kTcpRst) != 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            reset_ = true;
            connected_ = true;  // 别让等 SYN-ACK 的人一直等到超时
        }
        cv_.notify_all();
        return;
    }

    if ((flags & kTcpSyn) != 0 && (flags & kTcpAck) != 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextSeq_ = 1;       // 我们的 SYN 占掉了序号 0
            nextAck_ = seq + 1; // 对端 SYN 占掉它自己的序号 0
        }
        sendControl(kTcpAck);
        {
            std::lock_guard<std::mutex> lk(mu_);
            connected_ = true;
        }
        cv_.notify_all();
        return;  // SYN 段不带数据
    }

    const size_t payloadLen = total > headerBytes ? total - headerBytes : 0;
    if (payloadLen > 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextAck_ += uint32_t(payloadLen);
            rx_.insert(rx_.end(), tcp + headerBytes, tcp + total);
        }
        sendControl(kTcpAck);  // 收多少 ACK 多少，不做延迟确认
        cv_.notify_all();
    }

    if ((flags & kTcpFin) != 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextAck_ += 1;  // FIN 也占一个序号
        }
        sendControl(kTcpAck);
        {
            std::lock_guard<std::mutex> lk(mu_);
            eof_ = true;
        }
        cv_.notify_all();
    }
}

bool UsbMuxChannel::waitConnected(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [this] { return connected_ || closed_ || reset_; });
    return connected_ && !reset_ && !closed_;
}

void UsbMuxChannel::markBroken()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        reset_ = true;
        connected_ = true;
    }
    cv_.notify_all();
}

void UsbMuxChannel::orphan()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        owner_ = nullptr;
        reset_ = true;
    }
    cv_.notify_all();
}

// ============================================================================
// UsbMux
// ============================================================================
UsbMux::UsbMux(IByteStream& pipe) : pipe_(pipe) {}

UsbMux::~UsbMux()
{
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        closed_ = true;
    }
    hsCv_.notify_all();
    if (reader_.joinable()) reader_.join();  // 读线程最多阻塞一个 kReadPollMs

    std::vector<UsbMuxChannel*> chans;
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        for (auto& kv : channels_) chans.push_back(kv.second);
        channels_.clear();
    }
    for (auto* c : chans) c->orphan();
}

bool UsbMux::isClosed()
{
    std::lock_guard<std::mutex> lk(stateMu_);
    return closed_;
}

bool UsbMux::begin(std::string* err)
{
    reader_ = std::thread([this] { readerLoop(); });

    const uint8_t zeros[4] = {0, 0, 0, 0};
    {
        std::lock_guard<std::mutex> lk(writeMu_);
        if (!sendFrame(kProtoVersion, kUsbMuxVersion, zeros, sizeof(zeros))) {
            if (err != nullptr) *err = "USBMUX 管道写不进去（版本消息没发出去）";
            return false;
        }
    }
    LOG_DEBUG("[%s] 已发版本消息，等应答", kTag);

    {
        std::unique_lock<std::mutex> lk(hsMu_);
        hsCv_.wait_for(lk, std::chrono::milliseconds(kHandshakeTimeoutMs),
                       [this] { return versionReplied_ || handshakeFailed_ || isClosed(); });
        if (handshakeFailed_) {
            if (err != nullptr)
                *err = "USBMUX 握手期间收到太多陈旧 TCP 帧 —— 这条口上还挂着上一个进程的会话";
            return false;
        }
        if (!versionReplied_) {
            if (err != nullptr) *err = "等 USBMUX 版本应答超时（60s）";
            return false;
        }
    }

    const uint8_t setup = kSetupValue;
    {
        std::lock_guard<std::mutex> lk(writeMu_);
        // **版本消息不占序号**：实测手机对 seq=1 的 setup 会回
        // "detected duplicate packet. Expected 0 received 1"（proto=1 的文本消息）。
        // 版本交换之后，主机的 tx_seq 从 0 重新起。
        nextMuxSeq_ = 0;
        if (!sendFrame(kProtoSetup, kMuxMagic, &setup, 1)) {
            if (err != nullptr) *err = "USBMUX setup 帧没发出去";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(hsMu_);
        handshakeDone_ = true;
    }
    LOG_INFO("[%s] 握手完成（v%u），可以开连接了", kTag, kUsbMuxVersion);
    return true;
}

std::unique_ptr<IByteStream> UsbMux::connectTcp(uint16_t port, int timeoutMs, std::string* err)
{
    {
        std::lock_guard<std::mutex> lk(hsMu_);
        if (!handshakeDone_) {
            if (err != nullptr) *err = "USBMUX 还没握手完，不能开连接";
            return nullptr;
        }
    }

    uint16_t localPort = 0;
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        for (int i = 0; i < 65535; ++i) {
            const uint16_t cand = nextSourcePort_;
            nextSourcePort_ = uint16_t(cand == 0xffff ? 1 : cand + 1);
            if (cand != 0 && channels_.find(cand) == channels_.end()) {
                localPort = cand;
                break;
            }
        }
        if (localPort == 0) {
            if (err != nullptr) *err = "USBMUX 本端端口用光了";
            return nullptr;
        }
    }

    auto ch = std::make_unique<UsbMuxChannel>(*this, localPort, port);
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        channels_[localPort] = ch.get();
    }

    if (!sendTcp(localPort, port, 0, 0, kTcpSyn, nullptr, 0)) {
        removeChannel(localPort);
        if (err != nullptr) *err = "SYN 发不出去";
        return nullptr;
    }
    LOG_DEBUG("[%s] 本端 %u → 手机 %u，已发 SYN", kTag, localPort, port);

    if (!ch->waitConnected(timeoutMs)) {
        removeChannel(localPort);
        if (err != nullptr)
            *err = "连手机端口 " + std::to_string(port) + " 超时（没等到 SYN-ACK）";
        return nullptr;
    }
    LOG_INFO("[%s] 到手机 %u 的连接通了（本端端口 %u）", kTag, port, localPort);
    return ch;
}

void UsbMux::removeChannel(uint16_t localPort)
{
    std::lock_guard<std::mutex> lk(chanMu_);
    channels_.erase(localPort);
}

bool UsbMux::sendFrame(uint32_t protocol, uint32_t word8, const uint8_t* payload, size_t n)
{
    // 调用方须持 writeMu_（nextMuxSeq_/muxAck_ 在这里面）
    std::vector<uint8_t> buf(kMuxHeader + n, 0);
    wrU32(buf.data(), protocol);
    wrU32(buf.data() + 4, uint32_t(kMuxHeader + n));
    wrU32(buf.data() + 8, word8);
    wrU16(buf.data() + 12, nextMuxSeq_);
    wrU16(buf.data() + 14, muxAck_);
    if (n > 0) std::memcpy(buf.data() + kMuxHeader, payload, n);
    LOG_DEBUG("[%s] → proto=%u len=%u seq=%u ack=%u", kTag, protocol, uint32_t(kMuxHeader + n),
              nextMuxSeq_, muxAck_);
    nextMuxSeq_ = uint16_t(nextMuxSeq_ + 1);

    return pipe_.writeAll(buf.data(), buf.size());
}

bool UsbMux::sendTcp(uint16_t srcPort, uint16_t dstPort, uint32_t seq, uint32_t ack, uint8_t flags,
                     const uint8_t* payload, size_t n)
{
    std::vector<uint8_t> seg(kTcpHeader + n, 0);
    wrU16(seg.data(), srcPort);
    wrU16(seg.data() + 2, dstPort);
    wrU32(seg.data() + 4, seq);
    wrU32(seg.data() + 8, ack);
    seg[12] = 0x50;  // 数据偏移 5（20 字节头），低 4 位 0
    seg[13] = flags;
    wrU16(seg.data() + 14, kTcpWindow);
    // 偏移 16 的 checksum 与 18 的 urgent 恒为 0：这套迷你 TCP 不校验、无带外数据
    if (n > 0) std::memcpy(seg.data() + kTcpHeader, payload, n);

    std::lock_guard<std::mutex> lk(writeMu_);
    return sendFrame(kProtoTcp, kMuxMagic, seg.data(), seg.size());
}

void UsbMux::readerLoop()
{
    std::vector<uint8_t> chunk(kReadChunk);
    std::vector<Frame>   frames;

    for (;;) {
        if (isClosed()) break;

        bool got = false;
        {
            std::lock_guard<std::mutex> lk(rxMu_);
            Frame f;
            while (takeFrame(&f)) {
                frames.push_back(std::move(f));
                got = true;
            }
        }
        if (got) {
            // 投递时不能持 rxMu_：onSegment 会回头往管道里写 ACK
            for (const Frame& f : frames) handleFrame(f);
            frames.clear();
            continue;
        }

        const int n = pipe_.read(chunk.data(), chunk.size(), kReadPollMs);
        if (n < 0) {
            failAll("USBMUX 管道断开");
            break;
        }
        if (n == 0) continue;  // 超时，回去看有没有被叫停

        std::lock_guard<std::mutex> lk(rxMu_);
        rx_.insert(rx_.end(), chunk.begin(), chunk.begin() + n);
    }
}

bool UsbMux::takeFrame(Frame* out)
{
    // 怪癖：个别回复会比声明的长度多写 4 字节。只允许在"版本应答"或"无 payload 的
    // SYN-ACK"之后跳，而且必须先确认跳过 4 字节后确实是个像样的 TCP 帧 —— 宁可
    // 卡在这儿等更多数据，也别把正常帧头当成 padding 吃掉。
    if (prevAllowsPadding_ && rx_.size() >= kTcpHeader + 2 * sizeof(uint32_t) &&
        rdU32(rx_.data()) != kProtoTcp && rdU32(rx_.data() + 4) == kProtoTcp) {
        const uint32_t length = rdU32(rx_.data() + 8);
        const uint32_t word8 = rdU32(rx_.data() + 12);
        if (length >= kMuxHeader + kTcpHeader && length <= kMaxFrame && word8 == kReplyMagic) {
            const uint8_t* tcp = rx_.data() + 4 + kMuxHeader;
            const size_t   hdr = size_t((tcp[12] >> 4) & 0x0f) * 4;
            if (hdr >= kTcpHeader && hdr <= length - kMuxHeader && rdU16(tcp) != 0 &&
                rdU16(tcp + 2) != 0) {
                LOG_DEBUG("[%s] 跳过 4 字节可选 padding（对端多写了 4 字节）", kTag);
                rx_.erase(rx_.begin(), rx_.begin() + 4);
                prevAllowsPadding_ = false;
            }
        }
    }

    if (rx_.size() < kMuxHeader) return false;

    const uint32_t protocol = rdU32(rx_.data());
    const uint32_t length = rdU32(rx_.data() + 4);
    if (length < kMuxHeader || length > kMaxFrame) {
        // 正常路径不该走到这儿；真走到了只能丢 1 字节重新找同步
        LOG_WARN("[%s] 帧长 %u 不合法，丢 1 字节重新找同步", kTag, length);
        prevAllowsPadding_ = false;
        rx_.erase(rx_.begin());
        return false;
    }
    if (rx_.size() < length) return false;

    out->protocol = protocol;
    out->length = length;
    out->word8 = rdU32(rx_.data() + 8);
    out->sequence = rdU16(rx_.data() + 12);
    out->ack = rdU16(rx_.data() + 14);
    out->payload.assign(rx_.begin() + kMuxHeader, rx_.begin() + length);
    rx_.erase(rx_.begin(), rx_.begin() + length);

    const bool versionReply =
        out->protocol == kProtoVersion && out->length == kVersionBytes && out->word8 == kUsbMuxVersion;
    const bool bareSynAck =
        out->protocol == kProtoTcp && out->word8 == kReplyMagic && out->payload.size() == kTcpHeader &&
        ((out->payload[12] >> 4) & 0x0f) == 5 && out->payload[13] == kTcpSynAck &&
        rdU16(out->payload.data()) != 0 && rdU16(out->payload.data() + 2) != 0;
    prevAllowsPadding_ = versionReply || bareSynAck;
    return true;
}

void UsbMux::handleFrame(const Frame& f)
{
    LOG_DEBUG("[%s] ← proto=%u len=%u word8=0x%08x seq=%u ack=%u payload=%zu", kTag, f.protocol,
              f.length, f.word8, f.sequence, f.ack, f.payload.size());
    {
        std::lock_guard<std::mutex> lk(writeMu_);
        muxAck_ = f.sequence;  // 我们不单发 MUX 层 ACK，就靠往后捎带
    }

    if (f.protocol == kProtoVersion) {
        if (f.length == kVersionBytes && f.word8 == kUsbMuxVersion) {
            {
                std::lock_guard<std::mutex> lk(hsMu_);
                versionReplied_ = true;
            }
            hsCv_.notify_all();
        } else {
            LOG_WARN("[%s] 版本应答不认识：len=%u word8=%u", kTag, f.length, f.word8);
        }
        return;
    }

    if (f.protocol == kProtoSetup) return;  // setup 没有回应

    if (f.protocol == kProtoControl) {
        // 手机主动发来的控制消息。载荷 = 1 字节类型 + 文本，实测就是拿它报错：
        //   "\x04detected duplicate packet. Expected 0 received 1"
        // 序号对不上时**必然**出现，所以原样打出来 —— 这类"手机骂人"的消息比超时好查一万倍。
        const std::string text(f.payload.begin() + (f.payload.empty() ? 0 : 1), f.payload.end());
        LOG_WARN("[%s] 手机发来控制消息（type=0x%02x）：%s", kTag,
                 f.payload.empty() ? 0 : f.payload[0], text.c_str());
        return;
    }

    if (f.protocol == kProtoTcp) {
        bool tooMany = false;
        {
            std::lock_guard<std::mutex> lk(hsMu_);
            if (!handshakeDone_ && ++staleFrames_ > kStaleFrameLimit) {
                handshakeFailed_ = true;
                tooMany = true;
            }
        }
        if (tooMany) {
            hsCv_.notify_all();
            return;
        }
        dispatchTcp(f);
        return;
    }

    LOG_DEBUG("[%s] 忽略未知协议帧 proto=%u len=%u，载荷如下：\n%s", kTag, f.protocol, f.length,
              hexDump(f.payload, 64).c_str());
}

void UsbMux::dispatchTcp(const Frame& f)
{
    if (f.payload.size() < kTcpHeader) return;
    const uint8_t* t = f.payload.data();
    const size_t headerBytes = size_t((t[12] >> 4) & 0x0f) * 4;
    if (headerBytes < kTcpHeader || headerBytes > f.payload.size()) {
        LOG_WARN("[%s] TCP 数据偏移 %zu 不合理，丢帧", kTag, headerBytes);
        return;
    }

    // 对端把我们本端的**源端口**回填在目的端口字段里 —— 这就是唯一的连接标识
    const uint16_t dstPort = rdU16(t + 2);
    LOG_DEBUG("[%s] TCP 段 src=%u dst=%u seq=%u ack=%u flags=0x%02x 载荷=%zu", kTag, rdU16(t),
              dstPort, rdU32(t + 4), rdU32(t + 8), t[13], f.payload.size() - headerBytes);

    UsbMuxChannel* ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        auto it = channels_.find(dstPort);
        if (it != channels_.end()) ch = it->second;
    }
    if (ch == nullptr) {
        LOG_DEBUG("[%s] 本端端口 %u 上没有连接，丢弃该段", kTag, dstPort);
        return;
    }

    ch->onSegment(t, headerBytes, f.payload.size(), rdU32(t + 4), t[13]);
}

void UsbMux::failAll(const char* why)
{
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        closed_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(hsMu_);
        handshakeFailed_ = true;
    }
    hsCv_.notify_all();

    std::vector<UsbMuxChannel*> chans;
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        for (auto& kv : channels_) chans.push_back(kv.second);
    }
    for (auto* c : chans) c->markBroken();

    LOG_WARN("[%s] %s —— 这条口上的连接全部作废", kTag, why);
}

}  // namespace core::usb
