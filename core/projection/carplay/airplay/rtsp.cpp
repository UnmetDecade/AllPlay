#include "airplay/rtsp.h"

#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace core::airplay {

namespace {

// Disconnect detection: the iPhone sends feedback every ~2s, so 5s without a request means disconnected (used to be 10s — too blunt:
// after the phone was taken away the screen froze for 10 seconds before returning to the splash screen; 5s leaves room for one lost packet).
constexpr int kConnTimeoutMs = 5000;

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// Control channel read side: plaintext (cipher empty) or "ciphertext frame -> plaintext buffer".
struct Channel {
    core::ITcpStream* stream = nullptr;
    const std::shared_ptr<ControlCipher>* cipher = nullptr;   // points to RtspServer::cipher_
    std::vector<uint8_t> plain;                               // decrypted plaintext buffer
    int lastRead = 0;   // last read() return value: 0 = timeout (phone went quiet), <0 = disconnected

    int read(uint8_t* buf, size_t n, int timeoutMs)
    {
        if (!cipher || !*cipher) return lastRead = stream->read(buf, n, timeoutMs);

        while (plain.empty()) {
            uint8_t raw[2048];
            int got = stream->read(raw, sizeof raw, timeoutMs);
            if (got <= 0) return lastRead = got;         // timeout/disconnect
            (*cipher)->feed(raw, size_t(got));
            std::vector<uint8_t> pt;
            while ((*cipher)->next(&pt)) {
                if (!pt.empty()) plain.insert(plain.end(), pt.begin(), pt.end());
                pt.clear();
            }
            if ((*cipher)->failed()) return lastRead = -1;   // authentication failed
            // less than a full frame (TCP fragmentation) -> outer while keeps reading; ditto for empty plaintext frames, pull the next one
        }
        size_t take = std::min(n, plain.size());
        std::memcpy(buf, plain.data(), take);
        plain.erase(plain.begin(), plain.begin() + take);
        return int(take);
    }
};

/// Read one line (up to and including \n); returns false on EOF/timeout/overflow. Trailing \r\n is stripped.
bool readLine(Channel& ch, std::string* line, size_t maxLen = 8192)
{
    line->clear();
    char c;
    while (line->size() < maxLen) {
        int n = ch.read(reinterpret_cast<uint8_t*>(&c), 1, kConnTimeoutMs);
        if (n <= 0) return false;
        if (c == '\n') {
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return true;
        }
        line->push_back(c);
    }
    return false;
}

bool readExact(Channel& ch, uint8_t* buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = ch.read(buf + got, len - got, kConnTimeoutMs);
        if (n <= 0) return false;
        got += size_t(n);
    }
    return true;
}

bool sendAll(core::ITcpStream& s, const uint8_t* buf, size_t len)
{
    return s.writeAll(buf, len);
}

/// Decide keep-alive after parsing the headers
bool isKeepAlive(const RtspRequest& req)
{
    const std::string conn = toLower(req.header("Connection"));
    return conn.find("close") == std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// RtspRequest / RtspResponse
// ---------------------------------------------------------------------------

std::string RtspRequest::header(const std::string& name) const
{
    const std::string want = toLower(name);
    for (const auto& kv : headers)
        if (toLower(kv.first) == want) return kv.second;
    return {};
}

void RtspResponse::setHeader(const std::string& k, const std::string& v)
{
    for (auto& kv : headers) {
        if (kv.first == k) { kv.second = v; return; }
    }
    headers.emplace_back(k, v);
}

std::string RtspResponse::header(const std::string& name) const
{
    const std::string want = toLower(name);
    for (const auto& kv : headers)
        if (toLower(kv.first) == want) return kv.second;
    return {};
}

// ---------------------------------------------------------------------------
// RtspServer
// ---------------------------------------------------------------------------

RtspServer::~RtspServer() { stop(); }

void RtspServer::start(std::unique_ptr<core::ITcpListener> listener, Handler handler)
{
    listener_ = std::move(listener);
    handler_  = std::move(handler);
    stop_.store(false);
    acceptThread_ = std::thread([this] { acceptLoop(); });
    LOG_INFO("RTSP server ready :%u", port());
}

void RtspServer::stop()
{
    if (stop_.exchange(true)) return;
    if (listener_) listener_->stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : connStreams_) s->close();   // wake connection threads blocked in read
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    for (auto& t : connThreads_)
        if (t.joinable()) t.join();
    connThreads_.clear();
    connStreams_.clear();
}

void RtspServer::acceptLoop()
{
    while (!stop_.load()) {
        auto stream = listener_->accept(200);   // 200ms poll so stop() exits promptly
        if (!stream) continue;
        if (gate_ && !gate_()) {
            // Another channel is in use ("first to connect wins"): close it directly without even starting a thread.
            // On the phone side this looks like a connection reset; it will retry or fall back to the other path.
            stream->close();
            LOG_INFO("Connection rejected: another channel is in use");
            continue;
        }

        auto sh = std::shared_ptr<core::ITcpStream>(std::move(stream));
        {
            std::lock_guard<std::mutex> lk(mu_);
            connStreams_.push_back(sh);
        }
        LOG_INFO("Connection accepted");
        connThreads_.emplace_back([this, sh] {
            handleConnection(sh);
            std::lock_guard<std::mutex> lk(mu_);
            auto it = std::find(connStreams_.begin(), connStreams_.end(), sh);
            if (it != connStreams_.end()) connStreams_.erase(it);
        });
    }
}

void RtspServer::handleConnection(std::shared_ptr<core::ITcpStream> stream)
{
    Channel ch{stream.get(), &cipher_, {}};
    bool keep = true;
    size_t reqCount = 0;
    while (keep && !stop_.load()) {
        // ---- request line ----
        std::string line;
        if (!readLine(ch, &line) || line.empty()) break;

        RtspRequest req;
        {
            std::istringstream iss(line);
            if (!(iss >> req.method >> req.uri >> req.version)) break;
        }

        // ---- headers ----
        bool headerOk = true;
        while (true) {
            std::string h;
            if (!readLine(ch, &h)) { headerOk = false; break; }
            if (h.empty()) break;
            auto colon = h.find(':');
            if (colon == std::string::npos) continue;
            std::string k = h.substr(0, colon);
            std::string v = h.substr(colon + 1);
            size_t s = v.find_first_not_of(" \t");
            v = (s == std::string::npos) ? "" : v.substr(s);
            req.headers.emplace_back(k, v);
        }
        if (!headerOk) break;

        // ---- body (Content-Length) ----
        const std::string cl = req.header("Content-Length");
        if (!cl.empty()) {
            long len = std::strtol(cl.c_str(), nullptr, 10);
            if (len > 0) {
                if (len > 8 * 1024 * 1024) break;   // defensive: abnormally large -> disconnect
                req.body.resize(size_t(len));
                if (!readExact(ch, req.body.data(), req.body.size())) break;
            }
        }

        // ---- dispatch ----
        RtspResponse resp;
        try {
            resp = handler_ ? handler_(req) : RtspResponse::notImplemented();
        } catch (const std::exception& e) {
            LOG_ERROR("Handler threw an exception: %s", e.what());
            resp = {500, "Internal Server Error", {}, {}};
        }

        // ---- build response ----
        std::string head = "RTSP/1.0 " + std::to_string(resp.status) + " " + resp.reason + "\r\n";
        const std::string cseq = req.header("CSeq");
        if (!cseq.empty()) head += "CSeq: " + cseq + "\r\n";
        for (const auto& kv : resp.headers) head += kv.first + ": " + kv.second + "\r\n";
        head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        head += "\r\n";

        std::vector<uint8_t> wire(head.begin(), head.end());
        wire.insert(wire.end(), resp.body.begin(), resp.body.end());

        // ---- write (encrypted after pair-verify; with cipher still null at M4 the response is plaintext) ----
        auto c = cipher_;
        if (c) {
            auto framed = c->encrypt(wire.data(), wire.size());
            if (!sendAll(*stream, framed.data(), framed.size())) break;
        } else {
            if (!sendAll(*stream, wire.data(), wire.size())) break;
        }

        // Encryption may be enabled only after this response is written (M4 is written in plaintext; encryption starts with the next byte)
        if (afterResponse_) afterResponse_();

        keep = isKeepAlive(req);
        ++reqCount;
    }
    LOG_INFO("Control connection finished: %zu requests total, last read %s", reqCount,
             ch.lastRead == 0 ? "timed out (phone stopped sending)" : "returned disconnect (peer closed/error)");
    // The cipher belongs to this session only: the control key installed by pair-verify must be renegotiated next session.
    // If not cleared here, the first message on the next connection (a plaintext OPTIONS) would be decrypted as ciphertext,
    // and the phone would see "connected then disconnected by peer" — unable to reconnect until the process restarts.
    cipher_.reset();
    if (onDisconnect_) onDisconnect_();   // control connection dropped (phone disconnected/exited) -> reset session state
}

}  // namespace core::airplay
