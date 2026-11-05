#include "airplay/rtsp.h"

#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace core::airplay {

namespace {

// 断开检测：iPhone 每 ~2s 发 feedback，5s 无请求即判断开（原来是 10s —— 太钝，
// 手机拿走后画面要冻 10 秒才回启动页；5s 留够一次丢包的余量）。
constexpr int kConnTimeoutMs = 5000;

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

/// 控制通道读端：明文（cipher 为空）或"密文帧→明文缓冲"。
struct Channel {
    core::ITcpStream* stream = nullptr;
    const std::shared_ptr<ControlCipher>* cipher = nullptr;   // 指向 RtspServer::cipher_
    std::vector<uint8_t> plain;                               // 解密后的明文缓冲
    int lastRead = 0;   // 最后一次 read 的返回值：0=超时（手机安静了），<0=断开

    int read(uint8_t* buf, size_t n, int timeoutMs)
    {
        if (!cipher || !*cipher) return lastRead = stream->read(buf, n, timeoutMs);

        while (plain.empty()) {
            uint8_t raw[2048];
            int got = stream->read(raw, sizeof raw, timeoutMs);
            if (got <= 0) return lastRead = got;         // 超时/断开
            (*cipher)->feed(raw, size_t(got));
            std::vector<uint8_t> pt;
            while ((*cipher)->next(&pt)) {
                if (!pt.empty()) plain.insert(plain.end(), pt.begin(), pt.end());
                pt.clear();
            }
            if ((*cipher)->failed()) return lastRead = -1;   // 鉴权失败
            // 没凑齐一整帧（TCP 分片）→ 外层 while 继续读；空明文帧同理继续拉下一帧
        }
        size_t take = std::min(n, plain.size());
        std::memcpy(buf, plain.data(), take);
        plain.erase(plain.begin(), plain.begin() + take);
        return int(take);
    }
};

/// 读一行（到 \n，含）；EOF/超时/超长返回 false。行尾 \r\n 会去掉。
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

/// 解析完头之后判断 keep-alive
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
    LOG_INFO("RTSP 服务器就绪 :%u", port());
}

void RtspServer::stop()
{
    if (stop_.exchange(true)) return;
    if (listener_) listener_->stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : connStreams_) s->close();   // 唤醒阻塞在 read 的连接线程
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
        auto stream = listener_->accept(200);   // 200ms 轮询，stop 及时退出
        if (!stream) continue;
        if (gate_ && !gate_()) {
            // 另一条通道正在用（双通道"先连上的赢"）：直接关掉，连线程都不起。
            // 手机那边表现为连接被重置，它会重试或退回另一条路。
            stream->close();
            LOG_INFO("连接被拒：另一条通道正在使用");
            continue;
        }

        auto sh = std::shared_ptr<core::ITcpStream>(std::move(stream));
        {
            std::lock_guard<std::mutex> lk(mu_);
            connStreams_.push_back(sh);
        }
        LOG_INFO("连接进入");
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
        // ---- 请求行 ----
        std::string line;
        if (!readLine(ch, &line) || line.empty()) break;

        RtspRequest req;
        {
            std::istringstream iss(line);
            if (!(iss >> req.method >> req.uri >> req.version)) break;
        }

        // ---- 头 ----
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

        // ---- 体（Content-Length）----
        const std::string cl = req.header("Content-Length");
        if (!cl.empty()) {
            long len = std::strtol(cl.c_str(), nullptr, 10);
            if (len > 0) {
                if (len > 8 * 1024 * 1024) break;   // 防御：异常大直接断
                req.body.resize(size_t(len));
                if (!readExact(ch, req.body.data(), req.body.size())) break;
            }
        }

        // ---- 分发 ----
        RtspResponse resp;
        try {
            resp = handler_ ? handler_(req) : RtspResponse::notImplemented();
        } catch (const std::exception& e) {
            LOG_ERROR("处理器抛异常：%s", e.what());
            resp = {500, "Internal Server Error", {}, {}};
        }

        // ---- 组包 ----
        std::string head = "RTSP/1.0 " + std::to_string(resp.status) + " " + resp.reason + "\r\n";
        const std::string cseq = req.header("CSeq");
        if (!cseq.empty()) head += "CSeq: " + cseq + "\r\n";
        for (const auto& kv : resp.headers) head += kv.first + ": " + kv.second + "\r\n";
        head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        head += "\r\n";

        std::vector<uint8_t> wire(head.begin(), head.end());
        wire.insert(wire.end(), resp.body.begin(), resp.body.end());

        // ---- 写（pair-verify 后加密；M4 响应此刻 cipher 仍为空 → 明文）----
        auto c = cipher_;
        if (c) {
            auto framed = c->encrypt(wire.data(), wire.size());
            if (!sendAll(*stream, framed.data(), framed.size())) break;
        } else {
            if (!sendAll(*stream, wire.data(), wire.size())) break;
        }

        // 写完这条响应后才可能启用加密（M4 明文写出，下一字节起加密）
        if (afterResponse_) afterResponse_();

        keep = isKeepAlive(req);
        ++reqCount;
    }
    LOG_INFO("控制连接结束：共 %zu 条请求，最后一次读%s", reqCount,
             ch.lastRead == 0 ? "超时（手机不再发包）" : "返回断开（对端关闭/出错）");
    // 密文只属于这一场会话：pair-verify 装进来的控制密钥，下一场要重新协商。
    // 不在这里清掉的话，下一条连接的第一句（明文 OPTIONS）会被当成密文去解，
    // 手机表现为"连上就被对端断开"—— 进程不重启就再也连不上。
    cipher_.reset();
    if (onDisconnect_) onDisconnect_();   // 控制连接断开（手机断开/退出）→ 复位会话状态
}

}  // namespace core::airplay
