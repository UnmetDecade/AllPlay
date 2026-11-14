/**
 * @file   usbmux.cpp
 * @brief  Implementation of usbmux v2 + mini TCP
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

// ---- MUX layer ----
constexpr size_t   kMuxHeader     = 16;
constexpr uint32_t kMuxMagic      = 0xfeedface;  ///< every frame the host sends has this at offset 8
constexpr uint32_t kReplyMagic    = 0xfaceface;  ///< TCP frames from the phone have this at offset 8
constexpr uint32_t kProtoVersion  = 0;
constexpr uint32_t kProtoControl  = 1;   ///< control/error messages from the phone (payload is human-readable text)
constexpr uint32_t kProtoSetup    = 2;
constexpr uint32_t kProtoTcp      = 6;
constexpr uint32_t kUsbMuxVersion = 2;
constexpr size_t   kVersionBytes  = 20;      ///< whole-frame length of a version message/reply
constexpr uint8_t  kSetupValue    = 0x07;
constexpr uint32_t kMaxFrame      = 65536;
constexpr int      kHandshakeTimeoutMs = 60000;
constexpr int      kStaleFrameLimit    = 32; ///< max stale TCP frames tolerated during handshake
constexpr int      kReadPollMs    = 100;
constexpr size_t   kReadChunk     = 64 * 1024;

// ---- mini TCP ----
constexpr size_t   kTcpHeader      = 20;
constexpr uint16_t kTcpWindow      = 512;
constexpr uint8_t  kTcpFin = 0x01, kTcpSyn = 0x02, kTcpRst = 0x04, kTcpAck = 0x10;
constexpr uint8_t  kTcpSynAck      = kTcpSyn | kTcpAck;
constexpr uint8_t  kTcpFinAck      = kTcpFin | kTcpAck;
constexpr size_t   kMaxSendPayload = 16 * 1024;

// All fields are big-endian
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

/// Hex dump (debug level only, for investigating "what did the peer send that we didn't understand")
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
// A mini TCP connection. To the layer above it is just an IByteStream.
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

    /// Called by UsbMux's reader thread: deliver a TCP segment. tcp points at the segment start; total is the whole length (including TCP header).
    void onSegment(const uint8_t* tcp, size_t headerBytes, size_t total, uint32_t seq, uint8_t flags);

    /// Wait for SYN-ACK (used by connectTcp)
    bool waitConnected(int timeoutMs);

    /// Pipe broke: wake all waiters
    void markBroken();

    /// UsbMux is destructing: drop the back-pointer so destruction won't touch the dead object
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
    bool                    eof_ = false;     ///< peer FIN
    bool                    reset_ = false;   ///< peer RST / pipe broken
    bool                    rstLogged_ = false;  ///< log RST only once; see read()
    bool                    closed_ = false;  ///< closed locally
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
        // Log only once: read() is called repeatedly by upper layers (TLS retries, polling); logging each time floods —
        // on the real device this loop once produced twenty million lines.
        if (!rstLogged_) {
            rstLogged_ = true;
            LOG_WARN("[%s] Local %u <- phone %u: received RST", kTag, localPort_, remotePort_);
        }
        return -1;
    }
    if (eof_) return -1;
    return 0;  // timeout
}

bool UsbMuxChannel::writeAll(const uint8_t* buf, size_t n)
{
    if (n == 0) return true;

    std::lock_guard<std::mutex> lk(mu_);
    if (owner_ == nullptr || closed_ || reset_) return false;

    // The layer below is USB bulk, no packet loss; but a frame has a cap, so split long writes into segments
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
            connected_ = true;  // don't let SYN-ACK waiters hang until timeout
        }
        cv_.notify_all();
        return;
    }

    if ((flags & kTcpSyn) != 0 && (flags & kTcpAck) != 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextSeq_ = 1;       // our SYN consumed sequence 0
            nextAck_ = seq + 1; // the peer's SYN consumed its own sequence 0
        }
        sendControl(kTcpAck);
        {
            std::lock_guard<std::mutex> lk(mu_);
            connected_ = true;
        }
        cv_.notify_all();
        return;  // SYN segments carry no data
    }

    const size_t payloadLen = total > headerBytes ? total - headerBytes : 0;
    if (payloadLen > 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextAck_ += uint32_t(payloadLen);
            rx_.insert(rx_.end(), tcp + headerBytes, tcp + total);
        }
        sendControl(kTcpAck);  // ACK exactly what was received, no delayed ACK
        cv_.notify_all();
    }

    if ((flags & kTcpFin) != 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            nextAck_ += 1;  // FIN consumes one sequence number too
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
    if (reader_.joinable()) reader_.join();  // the reader thread blocks for at most one kReadPollMs

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
            if (err != nullptr) *err = "USBMUX pipe write failed (version message was not sent)";
            return false;
        }
    }
    LOG_DEBUG("[%s] Version message sent; waiting for reply", kTag);

    {
        std::unique_lock<std::mutex> lk(hsMu_);
        hsCv_.wait_for(lk, std::chrono::milliseconds(kHandshakeTimeoutMs),
                       [this] { return versionReplied_ || handshakeFailed_ || isClosed(); });
        if (handshakeFailed_) {
            if (err != nullptr)
                *err = "USBMUX received too many stale TCP frames during the handshake - a previous process's session is still attached to this interface";
            return false;
        }
        if (!versionReplied_) {
            if (err != nullptr) *err = "Timed out waiting for the USBMUX version reply (60s)";
            return false;
        }
    }

    const uint8_t setup = kSetupValue;
    {
        std::lock_guard<std::mutex> lk(writeMu_);
        // **Version messages do not consume sequence numbers**: measured — the phone replies to a setup with seq=1
        // with "detected duplicate packet. Expected 0 received 1" (a proto=1 text message).
        // After the version exchange, the host's tx_seq restarts from 0.
        nextMuxSeq_ = 0;
        if (!sendFrame(kProtoSetup, kMuxMagic, &setup, 1)) {
            if (err != nullptr) *err = "Failed to send the USBMUX setup frame";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(hsMu_);
        handshakeDone_ = true;
    }
    LOG_INFO("[%s] Handshake complete (v%u); connections can be opened now", kTag, kUsbMuxVersion);
    return true;
}

std::unique_ptr<IByteStream> UsbMux::connectTcp(uint16_t port, int timeoutMs, std::string* err)
{
    {
        std::lock_guard<std::mutex> lk(hsMu_);
        if (!handshakeDone_) {
            if (err != nullptr) *err = "USBMUX handshake has not completed yet; cannot open a connection";
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
            if (err != nullptr) *err = "USBMUX ran out of local ports";
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
        if (err != nullptr) *err = "Failed to send SYN";
        return nullptr;
    }
    LOG_DEBUG("[%s] Local %u -> phone %u: SYN sent", kTag, localPort, port);

    if (!ch->waitConnected(timeoutMs)) {
        removeChannel(localPort);
        if (err != nullptr)
            *err = "Timed out connecting to phone port " + std::to_string(port) + " (no SYN-ACK received)";
        return nullptr;
    }
    LOG_INFO("[%s] Connected to phone %u (local port %u)", kTag, port, localPort);
    return ch;
}

void UsbMux::removeChannel(uint16_t localPort)
{
    std::lock_guard<std::mutex> lk(chanMu_);
    channels_.erase(localPort);
}

bool UsbMux::sendFrame(uint32_t protocol, uint32_t word8, const uint8_t* payload, size_t n)
{
    // Caller must hold writeMu_ (nextMuxSeq_/muxAck_ live inside it)
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
    seg[12] = 0x50;  // data offset 5 (20-byte header), low 4 bits 0
    seg[13] = flags;
    wrU16(seg.data() + 14, kTcpWindow);
    // checksum at offset 16 and urgent at 18 are always 0: this mini TCP does not checksum and has no out-of-band data
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
            // Must not hold rxMu_ while dispatching: onSegment writes ACKs back into the pipe
            for (const Frame& f : frames) handleFrame(f);
            frames.clear();
            continue;
        }

        const int n = pipe_.read(chunk.data(), chunk.size(), kReadPollMs);
        if (n < 0) {
            failAll("USBMUX pipe disconnected");
            break;
        }
        if (n == 0) continue;  // timeout; loop back to check whether we were told to stop

        std::lock_guard<std::mutex> lk(rxMu_);
        rx_.insert(rx_.end(), chunk.begin(), chunk.begin() + n);
    }
}

bool UsbMux::takeFrame(Frame* out)
{
    // Quirk: some replies are written 4 bytes longer than declared. Skipping is allowed only after a "version reply" or
    // a "payload-less SYN-ACK", and only after confirming the next 4 bytes are followed by a plausible TCP frame —
    // better to stall here waiting for more data than eat a real frame header as padding.
    if (prevAllowsPadding_ && rx_.size() >= kTcpHeader + 2 * sizeof(uint32_t) &&
        rdU32(rx_.data()) != kProtoTcp && rdU32(rx_.data() + 4) == kProtoTcp) {
        const uint32_t length = rdU32(rx_.data() + 8);
        const uint32_t word8 = rdU32(rx_.data() + 12);
        if (length >= kMuxHeader + kTcpHeader && length <= kMaxFrame && word8 == kReplyMagic) {
            const uint8_t* tcp = rx_.data() + 4 + kMuxHeader;
            const size_t   hdr = size_t((tcp[12] >> 4) & 0x0f) * 4;
            if (hdr >= kTcpHeader && hdr <= length - kMuxHeader && rdU16(tcp) != 0 &&
                rdU16(tcp + 2) != 0) {
                LOG_DEBUG("[%s] Skipping 4 bytes of optional padding (the peer wrote 4 extra bytes)", kTag);
                rx_.erase(rx_.begin(), rx_.begin() + 4);
                prevAllowsPadding_ = false;
            }
        }
    }

    if (rx_.size() < kMuxHeader) return false;

    const uint32_t protocol = rdU32(rx_.data());
    const uint32_t length = rdU32(rx_.data() + 4);
    if (length < kMuxHeader || length > kMaxFrame) {
        // The normal path should never reach here; if it does, drop 1 byte and resync
        LOG_WARN("[%s] Invalid frame length %u; dropping 1 byte to resynchronize", kTag, length);
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
        muxAck_ = f.sequence;  // we never send standalone MUX ACKs; rely on piggybacking
    }

    if (f.protocol == kProtoVersion) {
        if (f.length == kVersionBytes && f.word8 == kUsbMuxVersion) {
            {
                std::lock_guard<std::mutex> lk(hsMu_);
                versionReplied_ = true;
            }
            hsCv_.notify_all();
        } else {
            LOG_WARN("[%s] Unrecognized version reply: len=%u word8=%u", kTag, f.length, f.word8);
        }
        return;
    }

    if (f.protocol == kProtoSetup) return;  // setup has no reply

    if (f.protocol == kProtoControl) {
        // Control messages the phone sends proactively. Payload = 1 type byte + text; it uses this to report errors:
        //   "\x04detected duplicate packet. Expected 0 received 1"
        // It **always** appears when sequence numbers mismatch, so log it verbatim — these "phone complaints" are far easier to diagnose than a timeout.
        const std::string text(f.payload.begin() + (f.payload.empty() ? 0 : 1), f.payload.end());
        LOG_WARN("[%s] Control message from the phone (type=0x%02x): %s", kTag,
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

    LOG_DEBUG("[%s] Ignoring unknown protocol frame proto=%u len=%u; payload follows:\n%s", kTag, f.protocol, f.length,
              hexDump(f.payload, 64).c_str());
}

void UsbMux::dispatchTcp(const Frame& f)
{
    if (f.payload.size() < kTcpHeader) return;
    const uint8_t* t = f.payload.data();
    const size_t headerBytes = size_t((t[12] >> 4) & 0x0f) * 4;
    if (headerBytes < kTcpHeader || headerBytes > f.payload.size()) {
        LOG_WARN("[%s] Unreasonable TCP data offset %zu; dropping frame", kTag, headerBytes);
        return;
    }

    // The peer echoes our local **source port** in the destination port field — this is the only connection identifier
    const uint16_t dstPort = rdU16(t + 2);
    LOG_DEBUG("[%s] TCP segment src=%u dst=%u seq=%u ack=%u flags=0x%02x payload=%zu", kTag, rdU16(t),
              dstPort, rdU32(t + 4), rdU32(t + 8), t[13], f.payload.size() - headerBytes);

    UsbMuxChannel* ch = nullptr;
    {
        std::lock_guard<std::mutex> lk(chanMu_);
        auto it = channels_.find(dstPort);
        if (it != channels_.end()) ch = it->second;
    }
    if (ch == nullptr) {
        LOG_DEBUG("[%s] No connection on local port %u; dropping segment", kTag, dstPort);
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

    LOG_WARN("[%s] %s - all connections on this interface are invalidated", kTag, why);
}

}  // namespace core::usb
