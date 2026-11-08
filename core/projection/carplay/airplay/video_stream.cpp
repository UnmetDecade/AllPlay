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
    // 流密钥：salt = "DataStream-Salt" + streamConnectionID(无符号十进制)
    const std::string salt = "DataStream-Salt" + streamConnectionIdDecimal;
    key_ = crypto::hkdfSha512(sharedSecret, salt, "DataStream-Output-Encryption-Key", 32);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("屏幕流就绪 :%u（streamConnectionID=%s）", port_, streamConnectionIdDecimal.c_str());
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
        LOG_INFO("屏幕流：手机连入数据端口 :%u", port_);
        handleConnection(*stream);   // 单连接，阻塞到断开
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
            // 明文配置帧：avcC/hvcC
            core::VideoCodec codec;
            std::vector<uint8_t> codecData;
            if (detectConfig(body, &codec, &codecData)) {
                codec_ = codec;
                if (video_) video_->configure(codec, width_, height_, codecData);
                configured_ = true;
                needsKeyFrame_ = true;   // 换配置后等新关键帧
                char hx[160] = "";
                for (size_t k = 0; k < codecData.size() && k < 48; ++k)
                    std::snprintf(hx + strlen(hx), sizeof hx - strlen(hx), "%02x", codecData[k]);
                LOG_INFO("屏幕流：收到配置帧（%s，%zu 字节）codecData=%s",
                         codec == core::VideoCodec::HEVC ? "hvcC" : "avcC", codecData.size(), hx);
            }
            continue;
        }

        // opcode 0：加密视频帧
        std::vector<uint8_t> pt;
        if (body.size() >= 16) {
            if (!crypto::chachaOpen(key_.data(), crypto::nonce64(counter_),
                                    header, kHeaderLen, body.data(), body.size(), &pt))
                return;   // 鉴权失败 → 断流
            ++counter_;
        } else {
            pt = std::move(body);   // 防御：短 body 当明文（防御性兜底）
        }

        if (frames_ == 0) {
            char hx[128] = "";
            for (size_t k = 0; k < pt.size() && k < 24; ++k)
                std::snprintf(hx + strlen(hx), sizeof hx - strlen(hx), "%02x", pt[k]);
            LOG_INFO("屏幕流：首个解密帧明文 head=%s", hx);
        }

        auto annexb = avccToAnnexB(pt);
        if (annexb.empty() || !configured_) continue;   // 没配置帧前丢帧

        const bool ra = isRandomAccess(codec_, annexb.data(), annexb.size());
        ++frames_;
        ++winFrames_;
        winBytes_ += annexb.size();
        if (frames_ <= 5 || frames_ % 300 == 0)
            LOG_INFO("屏幕流：视频帧 %zuB %s%s", annexb.size(), ra ? "IDR" : "非IDR",
                     (needsKeyFrame_ && !ra) ? "（闸门拦）" : "→解码");

        // IDR 自含（instant decoder refresh），解码器解到 IDR 会自动重置参考链，无需显式 flush。
        // 注意：不能在首帧输出前 flush——MediaCodec.flush() 会丢掉 configure 时设的 CSD，导致永久黑屏。
        if (needsKeyFrame_ && !ra) {                    // 闸门：等关键帧
            ++winGated_;
            reportWindow();
            continue;
        }
        if (ra) needsKeyFrame_ = false;
        if (video_ && !video_->feedAnnexB(annexb.data(), annexb.size())) {
            needsKeyFrame_ = true;            // 入队失败（丢帧）→ 重新放闸，等下一个关键帧
            ++winDrop_;
            if (onNeedKeyFrame_) onNeedKeyFrame_();   // 立刻请关键帧，别等 5s 盲发
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
    LOG_INFO("屏幕流：%.1fs 内 %llu 帧（%.1f fps），%.0f KB/s，闸门拦 %llu，交不出 %llu",
             sec, (unsigned long long)winFrames_, double(winFrames_) / sec,
             double(winBytes_) / 1024.0 / sec, (unsigned long long)winGated_,
             (unsigned long long)winDrop_);
    winFrames_ = winBytes_ = winGated_ = winDrop_ = 0;
    winStart_ = now;
}

bool ScreenStream::detectConfig(const std::vector<uint8_t>& body, core::VideoCodec* codec,
                                std::vector<uint8_t>* codecData)
{
    // 扫 ASCII "hvcC"/"avcC"（fourcc 之后的字节 = avcC/hvcC 盒体内容）
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
    // 兜底：looksLikeAvcC（裸 avcC，无 fourcc）——按协议判定
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
    // 已 AnnexB → 短路
    if (p.size() >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return p;

    // 校验：整段都是 4B 大端长度前缀的 NALU，且恰好填满
    size_t off = 0;
    while (off + 4 <= p.size()) {
        const uint32_t n = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
                           (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
        if (n == 0 || off + 4 + n > p.size()) return p;   // 非法 → 原样
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
    // 必须扫**整个访问单元**，不能只看第一个 NAL：
    // 关键帧 AU 的常见排布是 [SPS][PPS][IDR]（手机在关键帧处会内联重发参数集），
    // 只看首个 NAL 就会读到 SPS(7) 而判成"非关键帧"——闸门于是永远打不开，
    // 症状是"配置帧收到了、解码器也建好了，但一直黑屏"。真机与假手机都会踩。
    //
    // 这样扫是安全的：H.264 载荷有防竞争字节（emulation prevention），
    // NAL 载荷内部不可能出现 00 00 01，不会把载荷误当成起始码。
    size_t i = 0;
    while (i + 3 <= n) {
        size_t sc = 0;
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) sc = 3;
            else if (d[i + 2] == 0 && i + 3 < n && d[i + 3] == 1) sc = 4;
        }
        if (sc == 0) { ++i; continue; }

        const size_t h = i + sc;            // NAL 头字节
        if (h >= n) break;
        if (codec == core::VideoCodec::H264) {
            if ((d[h] & 0x1f) == 5) return true;            // IDR
        } else {
            const int type = (d[h] >> 1) & 0x3f;
            if (type >= 16 && type <= 21) return true;      // HEVC IRAP 16..21
        }
        i = h + 1;                          // 继续找下一个 NAL
    }
    return false;
}

}  // namespace core::airplay
