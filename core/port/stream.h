/**
 * @file   stream.h
 * @brief  Abstract byte stream (the only "stream" shape the protocol layer depends on)
 * @author blt
 */

// ============================================================================
// core/port/stream.h — abstract byte stream (the only "stream" shape the protocol layer depends on)
// ============================================================================
// Platform implementations: POSIX socket / RFCOMM / in-memory pipe (tests) … each provides read/write/close.
// iAP2, RTSP and screen streams in core consume only this interface and do not care whether the transport is a socket or Bluetooth.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

class IByteStream {
public:
    virtual ~IByteStream() = default;

    /// Read n bytes. Returns >0 = bytes actually read; 0 = timeout; <0 = disconnect/error.
    virtual int  read(uint8_t* buf, size_t n, int timeoutMs) = 0;
    /// Write all bytes (returns false on failure).
    virtual bool writeAll(const uint8_t* buf, size_t n) = 0;
    /// Actively close (no-op by default; platform implementations may override).
    virtual void close() {}
};

}  // namespace core
