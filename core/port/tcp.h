/**
 * @file   tcp.h
 * @brief  TCP listener/connection abstraction
 * @author blt
 */

// ============================================================================
// core/port/tcp.h — TCP listener/connection abstraction
// ============================================================================
// The RTSP control port, screen-stream data port and event port all build on it. Read timeouts are guaranteed by the platform layer
// (POSIX=SO_RCVTIMEO/poll, Windows=SO_RCVTIMEO, Android=Socket timeout).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <cstddef>

namespace core {

/// An established TCP connection (byte stream).
class ITcpStream {
public:
    virtual ~ITcpStream() = default;
    virtual int  read(uint8_t* buf, size_t n, int timeoutMs) = 0;  // >0 n; 0 timeout; <0 disconnected
    virtual bool writeAll(const uint8_t* buf, size_t n) = 0;
    virtual void close() = 0;
};

/// TCP listener.
class ITcpListener {
public:
    virtual ~ITcpListener() = default;

    /// Bind and listen. port=0 lets the kernel choose (read it back with port()). Returns false and sets err on failure.
    virtual bool start(const std::string& bindAddr, uint16_t port, std::string* err) = 0;
    /// Blocking accept of one connection (timeoutMs<=0 never times out). Returns nullptr on timeout/stopped.
    virtual std::unique_ptr<ITcpStream> accept(int timeoutMs) = 0;
    virtual uint16_t port() const = 0;
    /// Stop and wake up accept().
    virtual void stop() = 0;
};

}  // namespace core
