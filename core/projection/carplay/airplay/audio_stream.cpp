#include "airplay/audio_stream.h"

#include "common/log.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/hkdf.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace core::airplay {

namespace {
constexpr size_t kRtpHeader  = 12;
constexpr size_t kNonceTail  = 8;
constexpr size_t kMaxDatagram = 4096;
constexpr int    kTimeoutMs  = 1000;
}  // namespace

AudioStream::AudioStream(std::function<std::unique_ptr<core::IUdpSocket>()> makeUdp,
                         const std::vector<uint8_t>& sharedSecret,
                         const std::string& streamConnectionIdDecimal,
                         AudioCodec codec, int sampleRate, int channels,
                         core::IAudioOutput* output)
    : codec_(codec), sampleRate_(sampleRate), channels_(channels), output_(output)
{
    const auto key = crypto::hkdfSha512(sharedSecret,
                                        "DataStream-Salt" + streamConnectionIdDecimal,
                                        "DataStream-Output-Encryption-Key", 32);
    std::copy(key.begin(), key.end(), key_.begin());

    std::string err;
    dataSock_ = makeUdp();
    if (!dataSock_ || !dataSock_->bind(0, &err)) {
        LOG_WARN("Audio data UDP bind failed: %s", err.c_str());
        return;
    }
    dataPort_ = dataSock_->port();

    controlSock_ = makeUdp();
    if (controlSock_) controlSock_->bind(0, &err);
    controlPort_ = controlSock_ ? controlSock_->port() : 0;

    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("Audio stream ready dataPort=%u controlPort=%u (codec=%d %dHz %dch)",
             dataPort_, controlPort_, int(codec_), sampleRate_, channels_);
}

AudioStream::~AudioStream() { stop(); }

void AudioStream::stop()
{
    if (stop_.exchange(true)) return;
    if (dataSock_) dataSock_->close();
    if (controlSock_) controlSock_->close();
    if (thread_.joinable()) thread_.join();
}

void AudioStream::run()
{
    uint8_t buf[kMaxDatagram];
    // Receive gap watchdog: an AAC frame is ~21ms, so >80ms between two frames means the phone cannot keep up
    // (or our thread got preempted). This is the most direct evidence of "stuttering" — log the gap and sequence delta
    // to distinguish "network arrived late" from "decoding/sound card cannot keep up".
    auto lastRx = std::chrono::steady_clock::now();
    uint16_t lastSeq = 0;
    bool haveLast = false;
    int gapLogs = 0;

    while (!stop_.load()) {
        const int n = dataSock_->recv(buf, sizeof buf, kTimeoutMs);
        if (n <= 0) continue;   // timeout/closed

        const auto now = std::chrono::steady_clock::now();
        const auto gapMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRx).count();
        lastRx = now;

        // wire = [12B RTP header][ciphertext][16B tag][8B nonce]
        if (size_t(n) < kRtpHeader + 16 + kNonceTail) continue;

        std::array<uint8_t, 12> nonce{};
        std::memcpy(nonce.data() + 4, buf + n - kNonceTail, kNonceTail);   // 4 zero + trailing 8B

        const size_t ctLen = size_t(n) - kRtpHeader - kNonceTail;          // ciphertext + tag
        std::vector<uint8_t> pt;
        if (!crypto::chachaOpen(key_.data(), nonce,
                                buf + 4, 8,             // AAD = header[4..12] (timestamp+SSRC)
                                buf + kRtpHeader, ctLen,
                                &pt))
            continue;   // authentication failed (drop packet, non-fatal)

        const uint32_t sample = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) |
                                (uint32_t(buf[6]) << 8) | uint32_t(buf[7]);
        const uint16_t seq = uint16_t((buf[2] << 8) | buf[3]);   // RTP sequence number (for reordering)
        if (gapMs > 80 && gapLogs < 40) {
            ++gapLogs;
            LOG_WARN("Audio receive gap %lldms (seq %u->%u, dropped %d)", (long long)gapMs,
                     unsigned(haveLast ? lastSeq : seq), unsigned(seq),
                     haveLast ? int(int16_t(uint16_t(seq - lastSeq)) - 1) : 0);
        }
        lastSeq = seq;
        haveLast = true;

        if (pt.size() > 0) {
            static uint64_t audioPkts = 0;
            if (++audioPkts == 1) {
                char hx[64] = "";
                for (size_t k = 0; k < pt.size() && k < 16; ++k)
                    std::snprintf(hx + std::strlen(hx), sizeof hx - std::strlen(hx), "%02x", pt[k]);
                LOG_INFO("Audio packet #1 payload=%zuB head=%s", pt.size(), hx);
            } else if (audioPkts % 500 == 0) {
                LOG_INFO("Audio packet #%llu payload=%zuB sample=%u", (unsigned long long)audioPkts,
                         pt.size(), sample);
            }
        }
        deliver(pt.data(), pt.size(), sample, seq);
    }
}

void AudioStream::deliver(const uint8_t* payload, size_t len, uint32_t sample, uint16_t seq)
{
    // 16-bit RTP sequence number -> 64-bit extended sequence number (signed delta handles both wraparound and reordering)
    if (!haveSeq_) {
        haveSeq_ = true;
        nextSeq_ = seq;
        lastRaw_ = seq;
        lastExt_ = seq;
    } else {
        lastExt_ += int16_t(uint16_t(seq - lastRaw_));
        lastRaw_ = seq;
    }
    if (lastExt_ < nextSeq_) return;   // arrived too late (already played), drop

    pending_[lastExt_] = Pkt{std::vector<uint8_t>(payload, payload + len), sample};

    // emit consecutive packets in order
    while (!pending_.empty() && pending_.begin()->first == nextSeq_) {
        auto it = pending_.begin();
        if (output_) output_->feed(it->second.payload.data(), it->second.payload.size(), it->second.sample);
        pending_.erase(it);
        ++nextSeq_;
    }

    // Prevent unbounded buffering (otherwise a single missing packet blocks forever): force-emit the oldest beyond the threshold, accepting one small jump
    constexpr size_t kMaxJitter = 8;   // ~170ms (21.3ms per packet for 48kHz AAC)
    while (pending_.size() > kMaxJitter) {
        auto it = pending_.begin();
        if (output_) output_->feed(it->second.payload.data(), it->second.payload.size(), it->second.sample);
        nextSeq_ = it->first + 1;
        pending_.erase(it);
    }
}

}  // namespace core::airplay
