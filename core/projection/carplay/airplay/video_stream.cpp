#include "airplay/video_stream.h"

#include "common/log.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/hkdf.h"

#include <cstring>

namespace core::airplay {

namespace {
constexpr size_t kHeaderLen = 128;
constexpr size_t kMaxBody = 8 * 1024 * 1024;
constexpr int    kTimeoutMs = 10000;
}  // namespace

ScreenStream::ScreenStream(std::unique_ptr<core::ITcpListener> listener,
                           const std::vector<uint8_t>& sharedSecret,
                           const std::string& streamConnectionIdDecimal,
                           int width, int height, core::IVideoOutput* video,
                           std::function<void()> onNeedKeyFrame)
    : listener_(std::move(listener)), video_(video),
      onNeedKeyFrame_(std::move(onNeedKeyFrame)), width_(width), height_(height)
{
    port_ = listener_->port();
    // Stream key: salt = "DataStream-Salt" + streamConnectionID (unsigned decimal)
    const std::string salt = "DataStream-Salt" + streamConnectionIdDecimal;
    key_ = crypto::hkdfSha512(sharedSecret, salt, "DataStream-Output-Encryption-Key", 32);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("Screen stream ready :%u (streamConnectionID=%s)", port_, streamConnectionIdDecimal.c_str());
}

ScreenStream::~ScreenStream() { stop(); }

void ScreenStream::stop()
{
    if (stop_.exchange(true)) return;
    if (listener_) listener_->stop();
    if (thread_.joinable()) thread_.join();
}

void ScreenStream::requestKeyFrame()
{
    needsKeyFrame_ = true;
    if (video_) video_->reset();
}

void ScreenStream::run()
{
    while (!stop_.load()) {
        auto stream = listener_->accept(200);
        if (!stream) continue;
        LOG_INFO("Screen stream: phone connected to data port :%u", port_);
        handleConnection(*stream);   // single connection, blocks until disconnect
    }
}

void ScreenStream::handleConnection(core::ITcpStream& stream)
{
    while (!stop_.load()) {
        uint8_t header[kHeaderLen];
        size_t got = 0;
        while (got < kHeaderLen) {
            int n = stream.read(header + got, kHeaderLen - got, kTimeoutMs);
            if (n <= 0) return;
            got += size_t(n);
        }

        const uint32_t bodyLen = uint32_t(header[0]) | (uint32_t(header[1]) << 8) |
                                 (uint32_t(header[2]) << 16) | (uint32_t(header[3]) << 24);
        const uint8_t opcode = header[4];
        if (bodyLen > kMaxBody) return;

        std::vector<uint8_t> body(bodyLen);
        got = 0;
        while (got < bodyLen) {
            int n = stream.read(body.data() + got, bodyLen - got, kTimeoutMs);
            if (n <= 0) return;
            got += size_t(n);
        }

        if (opcode == 1) {
            // plaintext config frame: avcC/hvcC
            core::VideoCodec codec;
            std::vector<uint8_t> codecData;
            if (detectConfig(body, &codec, &codecData)) {
                codec_ = codec;
                if (video_) video_->configure(codec, width_, height_, codecData);
                configured_ = true;
                needsKeyFrame_ = true;   // wait for a new keyframe after config change
                char hx[160] = "";
                for (size_t k = 0; k < codecData.size() && k < 48; ++k)
                    std::snprintf(hx + strlen(hx), sizeof hx - strlen(hx), "%02x", codecData[k]);
                LOG_INFO("Screen stream: received config frame (%s, %zu bytes) codecData=%s",
                         codec == core::VideoCodec::HEVC ? "hvcC" : "avcC", codecData.size(), hx);
            }
            continue;
        }

        // opcode 0: encrypted video frame
        std::vector<uint8_t> pt;
        if (body.size() >= 16) {
            if (!crypto::chachaOpen(key_.data(), crypto::nonce64(counter_),
                                    header, kHeaderLen, body.data(), body.size(), &pt))
                return;   // authentication failed -> end stream
            ++counter_;
        } else {
            pt = std::move(body);   // defensive: treat a short body as plaintext (fallback)
        }

        if (frames_ == 0) {
            char hx[128] = "";
            for (size_t k = 0; k < pt.size() && k < 24; ++k)
                std::snprintf(hx + strlen(hx), sizeof hx - strlen(hx), "%02x", pt[k]);
            LOG_INFO("Screen stream: first decrypted frame plaintext head=%s", hx);
        }

        auto annexb = avccToAnnexB(pt);
        if (annexb.empty() || !configured_) continue;   // drop frames until a config frame arrives

        const bool ra = isRandomAccess(codec_, annexb.data(), annexb.size());
        ++frames_;
        ++winFrames_;
        winBytes_ += annexb.size();
        if (frames_ <= 5 || frames_ % 300 == 0)
            LOG_INFO("Screen stream: video frame %zuB %s%s", annexb.size(), ra ? "IDR" : "non-IDR",
                     (needsKeyFrame_ && !ra) ? "(gated)" : "->decode");

        // IDR is self-contained (instant decoder refresh); the decoder resets the reference chain on IDR, so no explicit flush is needed.
        // NOTE: do not flush before the first frame is output — MediaCodec.flush() drops the CSD set at configure time, causing a permanent black screen.
        if (needsKeyFrame_ && !ra) {                    // gate: wait for a keyframe
            ++winGated_;
            reportWindow();
            continue;
        }
        if (ra) needsKeyFrame_ = false;
        if (video_ && !video_->feedAnnexB(annexb.data(), annexb.size())) {
            needsKeyFrame_ = true;            // enqueue failed (dropped frame) -> re-arm the gate, wait for the next keyframe
            ++winDrop_;
            if (onNeedKeyFrame_) onNeedKeyFrame_();   // request a keyframe immediately instead of waiting 5s
        }
        reportWindow();
    }
}

void ScreenStream::reportWindow()
{
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - winStart_).count();
    if (ms < 5000) return;
    const double sec = double(ms) / 1000.0;
    LOG_INFO("Screen stream: %.1fs, %llu frames (%.1f fps), %.0f KB/s, gated %llu, failed to deliver %llu",
             sec, (unsigned long long)winFrames_, double(winFrames_) / sec,
             double(winBytes_) / 1024.0 / sec, (unsigned long long)winGated_,
             (unsigned long long)winDrop_);
    winFrames_ = winBytes_ = winGated_ = winDrop_ = 0;
    winStart_ = now;
}

bool ScreenStream::detectConfig(const std::vector<uint8_t>& body, core::VideoCodec* codec,
                                std::vector<uint8_t>* codecData)
{
    // Scan for ASCII "hvcC"/"avcC" (bytes after the fourcc = avcC/hvcC box body)
    for (size_t i = 0; i + 4 <= body.size(); ++i) {
        if (std::memcmp(body.data() + i, "hvcC", 4) == 0) {
            *codec = core::VideoCodec::HEVC;
            codecData->assign(body.begin() + i + 4, body.end());
            return true;
        }
        if (std::memcmp(body.data() + i, "avcC", 4) == 0) {
            *codec = core::VideoCodec::H264;
            codecData->assign(body.begin() + i + 4, body.end());
            return true;
        }
    }
    // Fallback: looksLikeAvcC (bare avcC, no fourcc) — decided per the protocol
    if (body.size() >= 9 && (body[5] & 0x1f) >= 1) {
        const uint16_t spsLen = uint16_t((body[6] << 8) | body[7]);
        if (spsLen > 0 && (body[8] & 0x1f) == 7) {
            *codec = core::VideoCodec::H264;
            *codecData = body;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> ScreenStream::avccToAnnexB(const std::vector<uint8_t>& p)
{
    // already AnnexB -> short-circuit
    if (p.size() >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return p;

    // Validate: the whole buffer is 4B big-endian length-prefixed NALUs and exactly filled
    size_t off = 0;
    while (off + 4 <= p.size()) {
        const uint32_t n = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
                           (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
        if (n == 0 || off + 4 + n > p.size()) return p;   // invalid -> return as-is
        off += 4 + n;
    }
    if (off != p.size()) return p;

    std::vector<uint8_t> out;
    out.reserve(p.size());
    off = 0;
    while (off + 4 <= p.size()) {
        const uint32_t n = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
                           (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), p.begin() + off + 4, p.begin() + off + 4 + n);
        off += 4 + n;
    }
    return out;
}

bool ScreenStream::isRandomAccess(core::VideoCodec codec, const uint8_t* d, size_t n)
{
    // Must scan the **entire access unit**, not just the first NAL:
    // A keyframe AU is commonly laid out as [SPS][PPS][IDR] (the phone inlines parameter sets at keyframes),
    // so looking only at the first NAL reads SPS(7) and classifies it as a "non-keyframe" — the gate then never opens,
    // with the symptom "config frame received, decoder created, but the screen stays black". Both real and fake phones hit this.
    //
    // Scanning this way is safe: H.264 payloads use emulation prevention bytes,
    // so 00 00 01 cannot appear inside a NAL payload and payload data is never mistaken for a start code.
    size_t i = 0;
    while (i + 3 <= n) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) sc = 3;
            else if (d[i + 2] == 0 && i + 3 < n && d[i + 3] == 1) sc = 4;
        }
        if (sc == 0) { ++i; continue; }

        const size_t h = i + sc;            // NAL header byte
        if (h >= n) break;
        if (codec == core::VideoCodec::H264) {
            if ((d[h] & 0x1f) == 5) return true;            // IDR
        } else {
            const int type = (d[h] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) return true;      // HEVC IRAP 16..21
        }
        i = h + 1;                          // continue to the next NAL
    }
    return false;
}

}  // namespace core::airplay
