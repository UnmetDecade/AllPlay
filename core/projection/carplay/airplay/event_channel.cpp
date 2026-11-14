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
constexpr int kTimeoutMs = 100;   // short read timeout so we do not hold the lock long and stall sendTouch

/// HID report: 2 contacts x 6 bytes (slot | down | x u16le | y u16le), per the AirPlay HID touchReport format.
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

/// Extract complete RTSP requests from the plaintext buffer; complete ones go to out and are removed, an incomplete tail stays in buf (wait for more bytes).
/// RTSP message parsing: find the \r\n\r\n header terminator, then take the body per Content-Length.
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
        if (total > buf.size()) break;   // incomplete body, wait for more bytes
        req.body.assign(buf.begin() + bodyStart, buf.begin() + total);
        out->push_back(std::move(req));
        buf.erase(buf.begin(), buf.begin() + total);
        off = 0;
    }
}

/// Build a minimal 200 response (echoing CSeq), per RTSP 200 response construction.
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
    writer_ = std::thread([this] { writeLoop(); });
    LOG_INFO("Event channel ready :%u", port_);
}

EventChannel::~EventChannel() { stop(); }

void EventChannel::rekey(const std::vector<uint8_t>& sharedSecret)
{
    std::lock_guard<std::mutex> lk(mu_);
    writeKey_ = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Write-Encryption-Key", 32);
    readKey_  = crypto::hkdfSha512(sharedSecret, "Events-Salt", "Events-Read-Encryption-Key", 32);
    cipher_.reset();
    plain_.clear();
    if (stream_) stream_->close();   // the previous session's connection is invalid; its read thread will exit the inner loop on its own
    stream_.reset();
    // packets queued from the previous session would only confuse the phone if sent with the new key; drop them
    {
        std::lock_guard<std::mutex> ok(outMu_);
        out_.clear();
        outBytes_ = 0;
    }
}

void EventChannel::stop()
{
    if (stop_.exchange(true)) return;
    if (listener_) listener_->stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stream_) stream_->close();
    }
    outCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (writer_.joinable()) writer_.join();
}

bool EventChannel::submit(std::vector<uint8_t> plain, bool droppable)
{
    constexpr size_t kMaxQueuedBytes = 128 * 1024;
    std::lock_guard<std::mutex> lk(outMu_);

    if (droppable) {
        // Touch is a "position snapshot", so a new one directly replaces old ones not yet sent — during fast swipes
        // there are dozens to hundreds of packets per second, and queuing them only accumulates latency while the cursor cannot keep up.
        for (auto it = out_.begin(); it != out_.end();) {
            if (it->droppable) {
                outBytes_ -= it->plain.size();
                it = out_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (outBytes_ + plain.size() > kMaxQueuedBytes) {
        // A queue full of ACKs means the peer is not reading at all and the connection is dead; drop the oldest to free space
        if (!out_.empty()) {
            outBytes_ -= out_.front().plain.size();
            out_.pop_front();
        }
    }
    outBytes_ += plain.size();
    out_.push_back(OutMsg{std::move(plain), droppable});
    outCv_.notify_one();
    return true;
}

void EventChannel::writeLoop()
{
    for (;;) {
        OutMsg msg;
        {
            std::unique_lock<std::mutex> lk(outMu_);
            outCv_.wait(lk, [&] { return stop_.load() || !out_.empty(); });
            if (out_.empty()) return;   // stopped and nothing left to send
            msg = std::move(out_.front());
            out_.pop_front();
            outBytes_ -= msg.plain.size();
        }

        // Copy the two connection shared_ptrs and release the lock: the send below may block for a long time if the phone is not reading,
        // and must never block while holding mu_ (the read thread needs it to send ACKs, and the UI thread is waiting for this call to return).
        std::shared_ptr<core::ITcpStream> stream;
        std::shared_ptr<ControlCipher> cipher;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stream = stream_;
            cipher = cipher_;
        }
        if (!stream || !cipher) continue;

        auto framed = cipher->encrypt(msg.plain.data(), msg.plain.size());
        if (!stream->writeAll(framed.data(), framed.size())) {
            const int n = writeFails_.fetch_add(1) + 1;
            if (n <= 5 || n % 50 == 0)
                LOG_WARN("Event channel write failed (attempt %d, %zu bytes, %s)", n, framed.size(),
                         msg.droppable ? "touch" : "command/ACK");
        }
    }
}

void EventChannel::run()
{
    while (!stop_.load()) {
        auto s = listener_->accept(200);
        if (!s) continue;
        LOG_INFO("Event channel: phone connected");

        {
            std::lock_guard<std::mutex> lk(mu_);
            stream_ = std::move(s);
            cipher_ = std::make_shared<ControlCipher>(readKey_.data(), writeKey_.data());
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

            // read raw ciphertext (no lock, otherwise sendTouch/forceKeyFrame would be slowed by the 100ms read timeout)
            const int n = local->read(buf, sizeof buf, kTimeoutMs);
            if (n < 0) break;          // disconnected
            if (n == 0) continue;      // timeout

            // decrypt + parse + send ACK (cipher is stateful, so lock for the whole block; writes are small packets and will not block reads for long)
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
                // Skip responses ("RTSP/1.0 200" / "HTTP/1.1 ..."): the iPhone's replies to our commands must not be ACKed,
                // otherwise it becomes an infinite mutual-ACK loop (RTSP request/response detection: method starts with "RTSP/")
                if (m.method.rfind("RTSP/", 0) == 0 || m.method.rfind("HTTP/", 0) == 0) continue;

                if (m.method == "POST" && (m.uri.find("/command") != std::string::npos)) {
                    auto root = bplist::parse(m.body.data(), m.body.size());
                    const std::string type = root ? bplist::strOf(bplist::dictGet(root, "type")) : "";
                    LOG_INFO("Event channel received command type=%s", type.c_str());
                } else {
                    LOG_INFO("Event channel received %s %s body=%zu", m.method.c_str(), m.uri.c_str(),
                             m.body.size());
                }
                // Even ACKs are only enqueued: the read thread must never stall here because the peer is not reading,
                // otherwise the entire channel (including touch) stops.
                submit(ackFor(m), false);
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
    return sendCommandImpl(cmd, false);
}

bool EventChannel::sendTouch(const std::vector<TouchPoint>& contacts)
{
    const auto report = touchReport(contacts, width_, height_);
    const auto cmd = bplist::dict({
        {"type",      bplist::str("hidSendReport")},
        {"uuid",      bplist::str("2a2a2a2a")},
        {"hidReport", bplist::data(report)},
    });
    // Touch uses the "droppable" path: it is called from the UI thread and must never wait on the socket,
    // and newer touches may supersede older ones (position snapshots; nobody wants the intermediate points).
    return sendCommandImpl(cmd, true);
}

bool EventChannel::sendCommandImpl(const std::shared_ptr<core::bplist::Value>& cmd, bool droppable)
{
    // Only "build plaintext + enqueue" here; the actual encryption and send happen in the write thread,
    // so the UI thread never blocks in send() (this was the root cause of "freezes while swiping").
    std::vector<uint8_t> wire;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stream_ || !cipher_) return false;   // the phone has not connected to the event channel yet

        const std::vector<uint8_t> body = bplist::serialize(cmd);
        char head[256];
        std::snprintf(head, sizeof head,
                      "POST /command RTSP/1.0\r\n"
                      "Content-Type: application/x-apple-binary-plist\r\n"
                      "Content-Length: %zu\r\n"
                      "CSeq: %llu\r\n\r\n",
                      body.size(), (unsigned long long)++cseq_);
        wire.assign(head, head + std::strlen(head));
        wire.insert(wire.end(), body.begin(), body.end());
    }
    return submit(std::move(wire), droppable);
}

}  // namespace core::airplay
