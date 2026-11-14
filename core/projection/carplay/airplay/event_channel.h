/**
 * @file   event_channel.h
 * @brief  Event channel (M4: HID/touch feedback + commands)
 * @author blt
 */

// ============================================================================
// core/airplay/event_channel.h - Event channel (M4: HID/touch feedback + commands)
// ============================================================================
// The iPhone connects to the eventPort from the SETUP response. The channel is encrypted with ControlCipher:
//   key = HKDF-SHA512(sharedSecret, "Events-Salt", "Events-*", 32)
//   write direction = "Events-Write-Encryption-Key", read direction = "Events-Read-Encryption-Key"
// Touch: encode contacts as an HID report and send it as the bplist body of POST /command
//   {"type":"hidSendReport","uuid":"2a2a2a2a","hidReport":<12B report>}
// Per the AirPlay event channel (sendCommand/sendTouch + HID touchReport).
#pragma once

#include "airplay/control_cipher.h"
#include "airplay/rtsp.h"
#include "port/tcp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core::bplist { class Value; }

namespace core::airplay {

/// One touch contact (coordinates normalized 0..1 relative to the main screen resolution).
struct TouchPoint {
    float x = 0;
    float y = 0;
    bool  down = false;
};

class EventChannel {
public:
    /// listener must already be bound (port known). sharedSecret = the shared secret from pair-verify.
    EventChannel(std::unique_ptr<core::ITcpListener> listener,
                 const std::vector<uint8_t>& sharedSecret,
                 int width, int height);
    ~EventChannel();
    EventChannel(const EventChannel&) = delete;

    uint16_t port() const { return port_; }
    void stop();

    /// Recompute the read/write keys from a new pair-verify sharedSecret and kick the old connection.
    /// The channel object is reused across sessions (the listening port stays the same), but the keys are **per session** — otherwise
    /// the phone cannot decrypt packets the host sends, and iOS terminates the entire CarPlay session.
    void rekey(const std::vector<uint8_t>& sharedSecret);

    /// Send a touch (normalized coordinates -> scaled to pixels -> HID report -> encrypted POST /command).
    bool sendTouch(const std::vector<TouchPoint>& contacts);

    /// Send an arbitrary command (bplist dict).
    bool sendCommand(const std::shared_ptr<core::bplist::Value>& cmd);

private:
    void run();   // accept + read side (decrypt + parse + send 200 ACK)
    /// Separate write thread: encrypt + send. See the .cpp for "why callers must not write directly".
    void writeLoop();
    /// Enqueue plaintext to send. Non-blocking — touch is called from the UI thread and must never wait on a socket here.
    bool submit(std::vector<uint8_t> plain, bool droppable);
    bool sendCommandImpl(const std::shared_ptr<core::bplist::Value>& cmd, bool droppable);

    struct OutMsg {
        std::vector<uint8_t> plain;
        bool droppable = false;   ///< true = a "newer supersedes older" packet like touch, may be dropped when the queue is full
    };

    std::unique_ptr<core::ITcpListener> listener_;
    int  width_ = 1280, height_ = 720;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::thread writer_;

    std::mutex mu_;                          // protects stream_/cipher_/cseq_/plain_
    std::shared_ptr<core::ITcpStream> stream_;
    std::shared_ptr<ControlCipher> cipher_;
    uint64_t cseq_ = 0;
    std::vector<uint8_t> writeKey_, readKey_;
    std::vector<uint8_t> plain_;             // decrypted plaintext buffer on the read side (partial frame remainder)

    std::mutex outMu_;                       // protects the send queue
    std::condition_variable outCv_;
    std::deque<OutMsg> out_;
    size_t outBytes_ = 0;
    std::atomic<int> writeFails_{0};
};

}  // namespace core::airplay
