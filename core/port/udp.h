/**
 * @file   udp.h
 * @brief  UDP socket abstraction (CarPlay audio uses RTP/RTCP over UDP)
 * @author blt
 */

// ============================================================================
// core/port/udp.h — UDP socket abstraction (CarPlay audio uses RTP/RTCP over UDP)
// ============================================================================
// Symmetric with ITcpListener: the protocol layer consumes only this interface; posix/Android each provide the backend.
#pragma once

#include <cstdint>
#include <string>
#include <cstddef>

namespace core {

/// A bound UDP socket (datagrams).
class IUdpSocket {
public:
    virtual ~IUdpSocket() = default;

    /// Bind a local port (port=0 lets the kernel choose; read it back with port()). Returns false and sets err on failure.
    virtual bool bind(uint16_t port, std::string* err) = 0;
    /// Receive one datagram. >0 = byte count; 0 = timeout; <0 = closed/error.
    virtual int recv(uint8_t* buf, size_t n, int timeoutMs) = 0;
    /// Send a datagram back to the source of the last recv (for NTP/RTCP responses).
    virtual bool send(const uint8_t* buf, size_t n) = 0;
    virtual uint16_t port() const = 0;
    virtual void close() = 0;
};

}  // namespace core
