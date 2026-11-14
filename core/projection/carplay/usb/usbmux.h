/**
 * @file   usbmux.h
 * @brief  usbmux v2 + mini TCP: open multiple "TCP connections" over the USBMUX bulk pipe
 * @author blt
 */

// ============================================================================
// core/usb/usbmux.h — usbmux v2 + mini TCP
// ============================================================================
// The wired CarPlay control plane starts from this pipe:
//
//   USBMUX port (bulk) → usbmux v2 frames → mini TCP → phone:62078 (lockdown)
//                                                → phone:carkit service port
//
// It is a **minimal** TCP: no retransmission, no sliding window, no out-of-order reassembly, checksum always 0,
// window always 512 — because the peer is the same machine speaking the same simplified stack, and the layer below is USB bulk,
// which never drops packets anyway. **Do not use it as a general-purpose TCP.**
//
// Multiplexing is **by "local source port" only**: local ports are allocated from 1; on receive we look up the TCP header's **destination port**
// field (where the peer echoes our local source port) and dispatch, dropping if not found. There is no other connection identifier.
//
// Framing uses the length at MUX header offset 4 (whole frame length, including the 16-byte header, valid range 16..65536).
// There is also a historical quirk: some replies are 4 bytes longer than declared; skipping is allowed only after a "version reply" and
// a "payload-less SYN-ACK", and only when the next frame really is a TCP frame — see takeFrame.
#pragma once

#include "port/stream.h"
#include "usb/mux.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core::usb {

class UsbMuxChannel;

/// A v2 session on one physical USBMUX pipe. **Does not own** the pipe (held by the caller, and must outlive this object).
class UsbMux : public IMux {
public:
    explicit UsbMux(IByteStream& pipe);
    ~UsbMux();

    UsbMux(const UsbMux&) = delete;
    UsbMux& operator=(const UsbMux&) = delete;

    /// Version negotiation + setup. Must be called once before any connectTcp.
    /// Returns false on failure; err is a human-readable reason.
    bool begin(std::string* err) override;

    /// Open a mini TCP to phone port (typical: 62078 = lockdown).
    /// Returns the connection's byte stream on success (owned by the caller); nullptr on failure.
    ///
    /// ⚠️ The returned stream **must be destroyed before this object** — it calls back into this object to send ACKs.
    std::unique_ptr<IByteStream> connectTcp(uint16_t port, int timeoutMs,
                                            std::string* err) override;

private:
    friend class UsbMuxChannel;

    struct Frame {
        uint32_t            protocol = 0;
        uint32_t            length = 0;     ///< whole frame length (including the 16-byte header)
        uint32_t            word8 = 0;      ///< offset 8: version number in a version reply, 0xfaceface in a TCP frame
        uint16_t            sequence = 0;
        uint16_t            ack = 0;
        std::vector<uint8_t> payload;
    };

    void readerLoop();
    void handleFrame(const Frame& f);
    void dispatchTcp(const Frame& f);
    void failAll(const char* why);
    bool isClosed();

    /// Take one frame from rx_ (caller must hold rxMu_). Returns false if less than a frame is available.
    bool takeFrame(Frame* out);

    /// Send a MUX frame. word8 is the version number only for version messages, otherwise kMuxMagic.
    bool sendFrame(uint32_t protocol, uint32_t word8, const uint8_t* payload, size_t n);

    /// Send a mini TCP segment (payload may be empty)
    bool sendTcp(uint16_t srcPort, uint16_t dstPort, uint32_t seq, uint32_t ack,
                 uint8_t flags, const uint8_t* payload, size_t n);

    void removeChannel(uint16_t localPort);

    IByteStream&                       pipe_;
    std::thread                        reader_;

    std::mutex                         rxMu_;
    std::vector<uint8_t>               rx_;             ///< leftover bytes not yet forming a frame
    bool                               prevAllowsPadding_ = false;

    std::mutex                         writeMu_;        ///< serializes writes to the pipe
    uint16_t                           nextMuxSeq_ = 0; ///< MUX-layer sequence number (version message uses 0)
    uint16_t                           muxAck_ = 0;

    std::mutex                         chanMu_;
    std::map<uint16_t, UsbMuxChannel*> channels_;
    uint16_t                           nextSourcePort_ = 1;

    std::mutex                         hsMu_;
    std::condition_variable            hsCv_;
    bool                               versionReplied_ = false;
    bool                               handshakeFailed_ = false;
    int                                staleFrames_ = 0;
    bool                               handshakeDone_ = false;

    std::mutex                         stateMu_;
    std::condition_variable            stateCv_;
    bool                               closed_ = false;
};

}  // namespace core::usb
