#include "airplay/event_channel.h"

#include "airplay/rtsp.h"
#include "bplist/bplist.h"
#include "common/log.h"
#include "crypto/hkdf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace core::airplay {

namespace {
constexpr int kTimeoutMs = 100;   // 读超时短一点，避免长时间占锁拖慢 sendTouch

/// HID report：2 触点 × 6 字节（slot | down | x u16le | y u16le），参照 AirPlay HID 的 touchReport 格式。
std::vector<uint8_t> touchReport(const std::vector<TouchPoint>& contacts, int width, int height)
{
    std::vector<uint8_t> r(12, 0);
    for (int slot = 0; slot < 2; ++slot) {
        const size_t off = size_t(slot) * 6;
        r[off] = uint8_t(slot);
        if (size_t(slot) >= contacts.size()) continue;
        const auto& c = contacts[slot];
        const uint16_t x = uint16_t(c.x * float(width));
        const uint16_t y = uint16_t(c.y * float(height));
        r[off + 1] = c.down ? 0x01 : 0x00;
        r[off + 2] = uint8_t(x & 0xff);
        r[off + 3] = uint8_t((x >> 8) & 0xff);
        r[off + 4] = uint8_t(y & 0xff);
        r[off + 5] = uint8_t((y >> 8) & 0xff);
    }
    return r;
}

/// 从明文缓冲切出完整 RTSP 请求；完整的进 out 并删掉，不完整的 buf 原样保留（等更多字节）。
/// 对照 RTSP 消息解析：找 \r\n\r\n 头结束，再按 Content-Length 取体。
void parseMessages(std::vector<uint8_t>& buf, std::vector<RtspRequest>* out)
{
    const std::vector<uint8_t> delim = {'\r', '\n', '\r', '\n'};
    size_t off = 0;
    while (off < buf.size()) {
        auto it = std::search(buf.begin() + off, buf.end(), delim.begin(), delim.end());
        if (it == buf.end()) break;
        const size_t headLen = size_t(it - buf.begin());
        const size_t bodyStart = headLen + delim.size();

        RtspRequest req;
        std::string head(reinterpret_cast<const char*>(buf.data()), headLen);
        size_t lineStart = 0;
        bool first = true;
        while (lineStart <= head.size()) {
            size_t lineEnd = head.find("\r\n", lineStart);
            const std::string l = head.substr(
                lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
            if (first) {
                std::istringstream iss(l);
                iss >> req.method >> req.uri >> req.version;
                first = false;
            } else if (!l.empty()) {
                const auto colon = l.find(':');
                if (colon != std::string::npos) {
                    std::string v = l.substr(colon + 1);
                    const size_t s = v.find_first_not_of(" \t");
                    req.headers.emplace_back(l.substr(0, colon),
                                             s == std::string::npos ? "" : v.substr(s));
                }
            }
            if (lineEnd == std::string::npos) break;
            lineStart = lineEnd + 2;
        }

        long len = 0;
        const std::string cl = req.header("Content-Length");
        if (!cl.empty()) len = std::strtol(cl.c_str(), nullptr, 10);
        if (len < 0) len = 0;
        const size_t total = bodyStart + size_t(len);
        if (total > buf.size()) break;   // 体不完整，等更多字节
        req.body.assign(buf.begin() + bodyStart, buf.begin() + total);
        out->push_back(std::move(req));
        buf.erase(buf.begin(), buf.begin() + total);
        off = 0;
    }
}

/// 组一条最小 200 响应（echo CSeq），对照 RTSP 200 响应构造。
std::vector<uint8_t> ackFor(const RtspRequest& req)
{
    std::string head = "RTSP/1.0 200 OK\r\n";
    const std::string cseq = req.header("CSeq");
    if (!cseq.empty()) head += "CSeq: " + cseq + "\r\n";
    head += "Content-Length: 0\r\n\r\n";
    return std::vector<uint8_t>(head.begin(), head.end());
}

}  // namespace

EventChannel::EventChannel(std::unique_ptr<core::ITcpListener> listener,
                           const std::vector<uint8_t>& sharedSecret,
                           int width, int height)
    : listener_(std::move(listener)), width_(width), height_(height)
{
    port_ = listener_->port();
    writeKey_ = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Write-Encryption-Key", 32);
    readKey_  = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Read-Encryption-Key", 32);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("事件通道就绪 :%u", port_);
}

EventChannel::~EventChannel() { stop(); }

void EventChannel::rekey(const std::vector<uint8_t>& sharedSecret)
{
    std::lock_guard<std::mutex> lk(mu_);
    writeKey_ = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Write-Encryption-Key", 32);
    readKey_  = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Read-Encryption-Key", 32);
    cipher_.reset();
    plain_.clear();
    if (stream_) stream_->close();   // 上一场的连接已作废，读线程会自己退出内层循环
    stream_.reset();
}

void EventChannel::stop()
{
    if (stop_.exchange(true)) return;
    if (listener_) listener_->stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stream_) stream_->close();
    }
    if (thread_.joinable()) thread_.join();
}

void EventChannel::run()
{
    while (!stop_.load()) {
        auto s = listener_->accept(200);
        if (!s) continue;
        LOG_INFO("事件通道：手机连入");

        {
            std::lock_guard<std::mutex> lk(mu_);
            stream_ = std::move(s);
            cipher_.reset(new ControlCipher(readKey_.data(), writeKey_.data()));
            plain_.clear();
        }

        uint8_t buf[2048];
        while (!stop_.load()) {
            std::shared_ptr<core::ITcpStream> local;
            {
                std::lock_guard<std::mutex> lk(mu_);
                local = stream_;
            }
            if (!local) break;

            // 读原始密文（不持锁，否则 sendTouch/forceKeyFrame 会被 100ms 读超时拖累）
            const int n = local->read(buf, sizeof buf, kTimeoutMs);
            if (n < 0) break;          // 断开
            if (n == 0) continue;      // 超时

            // 解密 + 解析 + 回 ACK（cipher 有状态，整段持锁；写也都是小包，不会长阻塞读）
            std::lock_guard<std::mutex> lk(mu_);
            if (!stream_ || !cipher_) break;
            cipher_->feed(buf, size_t(n));
            std::vector<uint8_t> pt;
            while (cipher_->next(&pt)) {
                if (!pt.empty()) plain_.insert(plain_.end(), pt.begin(), pt.end());
                pt.clear();
            }
            if (cipher_->failed()) break;

            std::vector<RtspRequest> msgs;
            parseMessages(plain_, &msgs);
            for (const auto& m : msgs) {
                // 跳过响应（"RTSP/1.0 200" / "HTTP/1.1 ..."）：iPhone 对我们命令的应答不用再 ACK，
                // 否则会形成无限互 ACK 死循环（对照 RTSP 请求/响应判定（method 以 "RTSP/" 开头））
                if (m.method.rfind("RTSP/", 0) == 0 || m.method.rfind("HTTP/", 0) == 0) continue;

                if (m.method == "POST" && (m.uri.find("/command") != std::string::npos)) {
                    auto root = bplist::parse(m.body.data(), m.body.size());
                    const std::string type = root ? bplist::strOf(bplist::dictGet(root, "type")) : "";
                    LOG_INFO("事件通道收到命令 type=%s", type.c_str());
                } else {
                    LOG_INFO("事件通道收到 %s %s body=%zu", m.method.c_str(), m.uri.c_str(),
                             m.body.size());
                }
                const auto ack = ackFor(m);
                auto framed = cipher_->encrypt(ack.data(), ack.size());
                if (!stream_->writeAll(framed.data(), framed.size())) break;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            stream_.reset();
            cipher_.reset();
            plain_.clear();
        }
    }
}

bool EventChannel::sendCommand(const std::shared_ptr<core::bplist::Value>& cmd)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!stream_ || !cipher_) return false;

    const std::vector<uint8_t> body = bplist::serialize(cmd);
    char head[256];
    std::snprintf(head, sizeof head,
                  "POST /command RTSP/1.0\r\n"
                  "Content-Type: application/x-apple-binary-plist\r\n"
                  "Content-Length: %zu\r\n"
                  "CSeq: %llu\r\n\r\n",
                  body.size(), (unsigned long long)++cseq_);

    std::vector<uint8_t> wire(head, head + std::strlen(head));
    wire.insert(wire.end(), body.begin(), body.end());
    auto framed = cipher_->encrypt(wire.data(), wire.size());   // cipher 有状态，需在锁内
    // 也持锁写：保证写计数器顺序 == 实际发包顺序（否则手机侧 ChaCha 计数错位、解密失败）
    return stream_->writeAll(framed.data(), framed.size());
}

bool EventChannel::sendTouch(const std::vector<TouchPoint>& contacts)
{
    const auto report = touchReport(contacts, width_, height_);
    const auto cmd = bplist::dict({
        {"type",      bplist::str("hidSendReport")},
        {"uuid",      bplist::str("2a2a2a2a")},
        {"hidReport", bplist::data(report)},
    });
    const bool ok = sendCommand(cmd);
    if (!ok)
        LOG_WARN("触摸发送失败：事件通道未就绪");
    return ok;
}

}  // namespace core::airplay
