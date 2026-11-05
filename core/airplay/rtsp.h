/**
 * @file   rtsp.h
 * @brief  极简 RTSP 服务器（平台无关，吃 core::ITcpListener）
 * @author blt
 */

// ============================================================================
// core/airplay/rtsp.h —— 极简 RTSP 服务器（平台无关，吃 core::ITcpListener）
// ============================================================================
// AirPlay 的控制通道就是 "RTSP over TCP"：请求行 + 头 + Content-Length 体，
// 语义和 HTTP 几乎一致（CSeq 头、keep-alive）。这里不引任何 HTTP 库，手写解析。
//
// 线程模型：一条 accept 线程 + 每连接一条线程（连接数很少：手机 1~2 条）。
// 底层 TCP 来自注入的 core::ITcpListener（POSIX / Android 各自实现）。
#pragma once

#include "airplay/control_cipher.h"
#include "port/tcp.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace core::airplay {

struct RtspRequest {
    std::string method;    ///< OPTIONS / GET / POST / SETUP / RECORD / ...
    std::string uri;       ///< "/info" 等
    std::string version;   ///< "RTSP/1.0"
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;

    /// 头查找（大小写不敏感）；没有返回空串
    std::string header(const std::string& name) const;
};

struct RtspResponse {
    int         status = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;

    void setHeader(const std::string& k, const std::string& v);
    std::string header(const std::string& name) const;

    static RtspResponse ok() { return {200, "OK", {}, {}}; }
    static RtspResponse notFound() { return {404, "Not Found", {}, {}}; }
    static RtspResponse notImplemented() { return {501, "Not Implemented", {}, {}}; }
    static RtspResponse badRequest() { return {400, "Bad Request", {}, {}}; }
};

class RtspServer {
public:
    using Handler = std::function<RtspResponse(const RtspRequest&)>;

    ~RtspServer();

    /// listener 需已绑定成功（start() 返回 true）；这里接管所有权并开始 accept。
    void start(std::unique_ptr<core::ITcpListener> listener, Handler handler);
    void stop();
    uint16_t port() const { return listener_ ? listener_->port() : 0; }

    /// pair-verify 完成后由组合根装入控制通道加密（null=明文）。
    void setCipher(std::shared_ptr<ControlCipher> c) { cipher_ = std::move(c); }
    /// 每写完一条响应回调一次（组合根用它做"M4 写完后才启加密"）。
    void setAfterResponse(std::function<void()> f) { afterResponse_ = std::move(f); }
    /// 控制连接断开时回调（组合根用它把会话状态复位，UI 回到启动页）。
    void setOnDisconnect(std::function<void()> f) { onDisconnect_ = std::move(f); }

    /// 连接闸门（双通道互斥用）：accept 到新连接时先问一句，返回 false 就直接关掉。
    /// 组合根用它实现"先连上的赢" —— 有线正在用就把 Wi-Fi 来的连接挡住，反之亦然。
    /// 不设闸门（默认）时一切照旧。
    void setConnectionGate(std::function<bool()> g) { gate_ = std::move(g); }

private:
    void acceptLoop();
    void handleConnection(std::shared_ptr<core::ITcpStream> stream);

    std::unique_ptr<core::ITcpListener> listener_;
    Handler handler_;

    // 单手机场景：一条 keep-alive 控制连接。cipher_ 由该连接线程在 pair-verify 后装入，
    // 同一线程随后读写——故无需跨线程同步。
    std::shared_ptr<ControlCipher> cipher_;
    std::function<void()>          afterResponse_;
    std::function<void()>          onDisconnect_;
    std::function<bool()>          gate_;          ///< 双通道互斥：false = 不接这条连接

    std::atomic<bool>       stop_{false};
    std::thread             acceptThread_;
    std::mutex              mu_;
    std::vector<std::shared_ptr<core::ITcpStream>> connStreams_;
    std::vector<std::thread> connThreads_;
};

}  // namespace core::airplay
