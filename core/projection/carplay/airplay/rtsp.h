/**
 * @file   rtsp.h
 * @brief  Minimal RTSP server (platform-independent, consumes core::ITcpListener)
 * @author blt
 */

// ============================================================================
// core/airplay/rtsp.h - Minimal RTSP server (platform-independent, consumes core::ITcpListener)
// ============================================================================
// The AirPlay control channel is just "RTSP over TCP": request line + headers + Content-Length body,
// with semantics almost identical to HTTP (CSeq header, keep-alive). No HTTP library is used; parsing is hand-written.
//
// Threading: one accept thread + one thread per connection (very few connections: 1-2 from the phone).
// The underlying TCP comes from an injected core::ITcpListener (implemented per platform: POSIX / Android).
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
    std::string uri;       ///< "/info" etc.
    std::string version;   ///< "RTSP/1.0"
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;

    /// Header lookup (case-insensitive); returns an empty string if absent
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

    /// listener must already be bound (start() returned true); this takes ownership and starts accepting.
    void start(std::unique_ptr<core::ITcpListener> listener, Handler handler);
    void stop();
    uint16_t port() const { return listener_ ? listener_->port() : 0; }

    /// After pair-verify completes, the composition root installs control channel encryption (null = plaintext).
    void setCipher(std::shared_ptr<ControlCipher> c) { cipher_ = std::move(c); }
    /// Called once after each response is written (the composition root uses it for "enable encryption only after M4 is written").
    void setAfterResponse(std::function<void()> f) { afterResponse_ = std::move(f); }
    /// Called when the control connection drops (the composition root uses it to reset session state so the UI returns to the splash screen).
    void setOnDisconnect(std::function<void()> f) { onDisconnect_ = std::move(f); }

    /// Connection gate (for dual-channel mutual exclusion): asked on each new accepted connection; false means close it immediately.
    /// The composition root uses it to implement "first to connect wins" — block Wi-Fi connections while wired is in use, and vice versa.
    /// With no gate set (the default), behavior is unchanged.
    void setConnectionGate(std::function<bool()> g) { gate_ = std::move(g); }

private:
    void acceptLoop();
    void handleConnection(std::shared_ptr<core::ITcpStream> stream);

    std::unique_ptr<core::ITcpListener> listener_;
    Handler handler_;

    // Single-phone scenario: one keep-alive control connection. cipher_ is installed by that connection thread after pair-verify,
    // and the same thread then reads and writes — so no cross-thread synchronization is needed.
    std::shared_ptr<ControlCipher> cipher_;
    std::function<void()>          afterResponse_;
    std::function<void()>          onDisconnect_;
    std::function<bool()>          gate_;          ///< dual-channel exclusion: false = reject this connection

    std::atomic<bool>       stop_{false};
    std::thread             acceptThread_;
    std::mutex              mu_;
    std::vector<std::shared_ptr<core::ITcpStream>> connStreams_;
    std::vector<std::thread> connThreads_;
};

}  // namespace core::airplay
