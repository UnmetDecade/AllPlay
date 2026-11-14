/**
 * @file   link.h
 * @brief  iAP2 link layer (marker / 9-byte frame / checksum / seq-ack / sync handshake)
 * @author blt
 */

// ============================================================================
// core/iap2/link.h - iAP2 link layer (marker / 9-byte frame / checksum / seq-ack / sync handshake)
// ============================================================================
// Protocol facts (compiled from iAP2 link-layer behavior):
//   · start marker: FF 55 02 00 EE 10; resent every 1s during DETECTING until the peer's marker is received
//   · data frame 9-byte header: FF 5A | u16 frame length | control | seq | ack | session | checksum
//     frame length = 9 + payload length + 1 (payload checksum); header checksum = negated sum of the first 8 bytes (whole frame sums to 0 mod 256)
//     payload checksum = negated sum of the payload, appended after the payload
//   · control bits: SYN 0x80 / ACK 0x40 / EAK 0x20 / RST 0x10; data frame control = 0x40 and carries session
//   · the ack field of every outgoing frame = the last seq received in order (cumulative ACK)
//   · initial seq = 99 (u8 wraparound); session 10 = control session (CSM goes here)
//   · SYN payload v1: ver(1B)=1 | maxOutgoing | u16 maxLength | u16 retransmit timeout (ms) |
//     u16 ack timeout (ms) | maxRetransmissions | maxAcknowledgements | 3B per session (id,kind,ver)
//   · NEGOTIATING: receive peer SYN -> record params + send ACK -> receive peer ACK -> NORMAL
//   · data retransmission: resend on timeout, declare dead after maxRetransmissions
//
// This implementation is a "sufficient and compatible" subset: window/retransmission/cumulative ACK are implemented, but
// the EAK out-of-order fill details are omitted (on a gap, just send ACK and wait for retransmission). See tools/iap2_test.cpp for behavior parity.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <cstddef>

namespace core::iap2 {

/// Local link parameters (at namespace scope: nested types with NSDMI cannot be used as default arguments)
struct LinkConfig {
    uint8_t  maxOutgoing            = 30;
    uint16_t ackTimeoutMs           = 500;
    uint16_t retransmissionTimeoutMs = 4000;
    uint8_t  maxRetransmissions     = 4;
    uint8_t  maxAcknowledgements    = 3;
    uint8_t  controlSessionVersion  = 1;

    /// Host initiates negotiation (wired = true; wireless = false).
    /// With wireless the phone sends the marker first and we reply with SYN; wired is the reverse — after sending the marker the host **immediately sends its own SYN**,
    /// without waiting for the peer's marker. Getting this backwards is easy to spot: after the marker goes out the peer stays silent and nothing comes back.
    bool     initiator              = false;
};

class LinkEngine {
public:
    explicit LinkEngine(LinkConfig cfg = LinkConfig());

    // ---- driving (single-threaded; caller supplies monotonic milliseconds) ----
    void start(uint64_t nowMs);
    void feed(const uint8_t* p, size_t n, uint64_t nowMs);
    void tick(uint64_t nowMs);

    // ---- output ----
    bool takeOutput(std::vector<uint8_t>* out);          ///< take pending output bytes (write to the transport)
    bool nextControl(std::vector<uint8_t>* payload);     ///< take one complete control-session (CSM) payload

    /// Send one control-session payload (queued if not ready; declares death above 64 entries)
    bool sendControl(const std::vector<uint8_t>& payload, uint64_t nowMs);

    // ---- state ----
    bool ready() const { return state_ == State::Normal; }
    bool dead() const { return state_ == State::Dead; }
    const std::string& deadReason() const { return deadReason_; }

    /// Maximum control payload the peer can carry (peer maxLength - 10); conservative value before negotiation
    size_t peerMaxPayload() const;

    /// Parameters reported in the peer SYN, as a one-line summary. This is the first thing to check when negotiation stalls.
    std::string peerSummary() const;

private:
    enum class State { Idle, Detecting, Negotiating, Normal, Dead };

    struct Frame {
        uint16_t length = 0;
        uint8_t  control = 0;
        uint8_t  seq = 0;
        uint8_t  ack = 0;
        uint8_t  session = 0;
        std::vector<uint8_t> payload;   // no payload = empty
        bool     hasPayload = false;
    };

    void die(const std::string& why);
    void enterNegotiating(uint64_t nowMs);
    void appendOutput(const std::vector<uint8_t>& bytes);
    void writeFrame(uint8_t control, uint8_t seq, uint8_t session,
                    const std::vector<uint8_t>* payload);
    void sendSynchronization();
    void sendAckOnly();
    void sendData(uint8_t seq, uint8_t session, const std::vector<uint8_t>& payload);

    bool parseOne(uint64_t nowMs);
    void handleFrame(const Frame& f, uint64_t nowMs);
    void handleSync(const Frame& f);
    void handleAckField(uint8_t ack, uint64_t nowMs);
    void handleData(const Frame& f, uint64_t nowMs);
    void flushQueued(uint64_t nowMs);

    struct OutPacket {
        uint8_t seq = 0;
        uint8_t session = 0;
        std::vector<uint8_t> payload;
        int     retransmissions = 0;
        uint64_t deadlineMs = 0;
    };

    LinkConfig cfg_;
    State      state_ = State::Idle;

    // receive
    std::vector<uint8_t> rx_;

    // peer sync parameters (valid after SYN is received)
    bool     peerSyncOk_ = false;
    uint8_t  peerMaxOutgoing_ = 30;
    uint16_t peerMaxLength_ = 0xffff;
    uint16_t peerRetransTimeoutMs_ = 4000;
    uint16_t peerAckTimeoutMs_ = 500;
    uint8_t  peerMaxRetransmissions_ = 4;
    uint8_t  peerMaxAcks_ = 3;

    // sequence/ACK
    uint8_t  sentSeq_ = 99;
    uint8_t  lastSentAcked_ = 99;
    uint8_t  lastReceivedInOrder_ = 0;
    bool     haveReceived_ = false;
    int      pendingAcks_ = 0;

    // out-of-order cache (seq -> payload)
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> outOfOrder_;

    std::deque<std::vector<uint8_t>> controlQueue_;   // complete control payloads pending delivery
    std::vector<OutPacket> unacked_;
    std::deque<OutPacket>  queued_;
    std::vector<uint8_t>   output_;

    uint64_t markerDeadlineMs_ = 0;
    uint64_t syncDeadlineMs_ = 0;
    std::string deadReason_;
};

}  // namespace core::iap2
