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
        LOG_WARN("音频数据 UDP 绑定失败：%s", err.c_str());
        return;
    }
    dataPort_ = dataSock_->port();

    controlSock_ = makeUdp();
    if (controlSock_) controlSock_->bind(0, &err);
    controlPort_ = controlSock_ ? controlSock_->port() : 0;

    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("音频流就绪 dataPort=%u controlPort=%u（codec=%d %dHz %dch）",
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
    // 收包间隔看门狗：AAC 一帧 ~21ms，连续两帧之间超过 80ms 就是手机那边送不过来
    // （或者我们这条线程被抢了）。这是"卡顿"最直接的物证 —— 把间隔和序号差打出来，
    // 就能区分"网络晚到"和"解码/声卡跟不上"。
    auto lastRx = std::chrono::steady_clock::now();
    uint16_t lastSeq = 0;
    bool haveLast = false;
    int gapLogs = 0;

    while (!stop_.load()) {
        const int n = dataSock_->recv(buf, sizeof buf, kTimeoutMs);
        if (n <= 0) continue;   // 超时/关闭

        const auto now = std::chrono::steady_clock::now();
        const auto gapMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRx).count();
        lastRx = now;

        // wire = [12B RTP 头][密文][16B tag][8B nonce]
        if (size_t(n) < kRtpHeader + 16 + kNonceTail) continue;

        std::array<uint8_t, 12> nonce{};
        std::memcpy(nonce.data() + 4, buf + n - kNonceTail, kNonceTail);   // 4 零 + 尾部 8B

        const size_t ctLen = size_t(n) - kRtpHeader - kNonceTail;          // 密文 + tag
        std::vector<uint8_t> pt;
        if (!crypto::chachaOpen(key_.data(), nonce,
                                buf + 4, 8,             // AAD = 头[4..12]（timestamp+SSRC）
                                buf + kRtpHeader, ctLen,
                                &pt))
            continue;   // 鉴权失败（丢包，非致命）

        const uint32_t sample = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) |
                                (uint32_t(buf[6]) << 8) | uint32_t(buf[7]);
        const uint16_t seq = uint16_t((buf[2] << 8) | buf[3]);   // RTP 序号（乱序重排用）
        if (gapMs > 80 && gapLogs < 40) {
            ++gapLogs;
            LOG_WARN("音频收包空档 %lldms（seq %u→%u，丢了 %d 个）", (long long)gapMs,
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
                LOG_INFO("音频包 #1 payload=%zuB head=%s", pt.size(), hx);
            } else if (audioPkts % 500 == 0) {
                LOG_INFO("音频包 #%llu payload=%zuB sample=%u", (unsigned long long)audioPkts,
                         pt.size(), sample);
            }
        }
        deliver(pt.data(), pt.size(), sample, seq);
    }
}

void AudioStream::deliver(const uint8_t* payload, size_t len, uint32_t sample, uint16_t seq)
{
    // 16-bit RTP 序号 → 64-bit 扩展序号（有符号增量同时处理回绕与乱序）
    if (!haveSeq_) {
        haveSeq_ = true;
        nextSeq_ = seq;
        lastRaw_ = seq;
        lastExt_ = seq;
    } else {
        lastExt_ += int16_t(uint16_t(seq - lastRaw_));
        lastRaw_ = seq;
    }
    if (lastExt_ < nextSeq_) return;   // 太晚到达（已播放过），丢弃

    pending_[lastExt_] = Pkt{std::vector<uint8_t>(payload, payload + len), sample};

    // 按序吐出连续的包
    while (!pending_.empty() && pending_.begin()->first == nextSeq_) {
        auto it = pending_.begin();
        if (output_) output_->feed(it->second.payload.data(), it->second.payload.size(), it->second.sample);
        pending_.erase(it);
        ++nextSeq_;
    }

    // 防缓冲无限增长（缺一个包就一直等）：超过阈值强制吐最旧的，接受一次小跳变
    constexpr size_t kMaxJitter = 8;   // ~170ms（48kHz AAC 每包 21.3ms）
    while (pending_.size() > kMaxJitter) {
        auto it = pending_.begin();
        if (output_) output_->feed(it->second.payload.data(), it->second.payload.size(), it->second.sample);
        nextSeq_ = it->first + 1;
        pending_.erase(it);
    }
}

}  // namespace core::airplay
