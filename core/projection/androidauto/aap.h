/**
 * @file   aap.h
 * @brief  Android Auto (AAP) frame format: channel id, message id, framing and deframing
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/aap.h - AAP frame format
// ============================================================================
// Each USB bulk transfer unit = one AAP frame:
//   byte0 = channel id
//   byte1 = flags: bit0 FIRST | bit1 LAST | bit2 CONTROL | bit3 ENCRYPTED
//   followed by a length (big-endian): 2 bytes for ordinary frames; 6 bytes for FIRST frames (u16 frame length + u32 total plaintext length)
//   payload = [2-byte big-endian message id] + body (encrypted: the whole segment is inside TLS)
//
// A message larger than 0x4000 bytes is split into FIRST/MIDDLE.../LAST; each chunk passes through TLS separately,
// so an encrypted frame's length field is the **ciphertext length**; only the FIRST total is the plaintext total.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace core::projection::androidauto {

enum class Channel : uint8_t {
    Control     = 0,
    Input       = 1,
    Sensor      = 2,
    Video       = 3,
    MediaAudio  = 4,
    SpeechAudio = 5,
    SystemAudio = 6,
    AvInput     = 7,
    Bluetooth   = 8,
    None        = 0xFF,
};

// ---- control channel message ids ----
constexpr uint16_t kMsgVersionRequest       = 0x0001;
constexpr uint16_t kMsgVersionResponse      = 0x0002;
constexpr uint16_t kMsgSslHandshake         = 0x0003;
constexpr uint16_t kMsgAuthComplete         = 0x0004;
constexpr uint16_t kMsgServiceDiscoveryReq  = 0x0005;
constexpr uint16_t kMsgServiceDiscoveryRsp  = 0x0006;
constexpr uint16_t kMsgChannelOpenRequest   = 0x0007;
constexpr uint16_t kMsgChannelOpenResponse  = 0x0008;
constexpr uint16_t kMsgPingRequest          = 0x000B;
constexpr uint16_t kMsgPingResponse         = 0x000C;
constexpr uint16_t kMsgNavigationFocusReq   = 0x000D;
constexpr uint16_t kMsgNavigationFocusRsp   = 0x000E;
constexpr uint16_t kMsgShutdownRequest      = 0x000F;
constexpr uint16_t kMsgShutdownResponse     = 0x0010;
constexpr uint16_t kMsgAudioFocusRequest    = 0x0012;
constexpr uint16_t kMsgAudioFocusResponse   = 0x0013;

// ---- AV / media channel message ids ----
constexpr uint16_t kMsgAvMediaWithTimestamp = 0x0000;
constexpr uint16_t kMsgAvMedia              = 0x0001;
constexpr uint16_t kMsgAvSetupRequest       = 0x8000;
constexpr uint16_t kMsgAvStart              = 0x8001;
constexpr uint16_t kMsgAvStop               = 0x8002;
constexpr uint16_t kMsgAvSetupResponse      = 0x8003;
constexpr uint16_t kMsgAvMediaAck           = 0x8004;
constexpr uint16_t kMsgVideoFocusRequest    = 0x8007;
constexpr uint16_t kMsgVideoFocusIndication = 0x8008;

// ---- input / sensor ----
constexpr uint16_t kMsgInputEvent        = 0x8001;
constexpr uint16_t kMsgBindingRequest    = 0x8002;
constexpr uint16_t kMsgBindingResponse   = 0x8003;
constexpr uint16_t kMsgSensorStartReq    = 0x8001;
constexpr uint16_t kMsgSensorStartRsp    = 0x8002;
constexpr uint16_t kMsgSensorEvent       = 0x8003;

constexpr size_t   kMaxFramePayload = 0x4000;

// ---- protobuf enum values (see *Enum.proto in AASDK) ----
enum : uint32_t {
    kAvStreamAudio = 1,
    kAvStreamVideo = 3,
    kAudioTypeMedia = 3,
    kRes480p = 1, kRes720p = 2, kRes1080p = 3,
    kFps30 = 1, kFps60 = 2,
    kVideoFocusFocused   = 1,
    kVideoFocusUnfocused = 2,
    kAudioFocusGain      = 1,
    kSetupStatusOk       = 2,
    kStatusOk            = 0,
    kTouchPress = 0, kTouchRelease = 1, kTouchDrag = 2,
    kSensorNightMode = 10, kSensorDrivingStatus = 13,
};

struct Frame {
    Channel              channel   = Channel::None;
    bool                 first     = false;
    bool                 last      = false;
    bool                 encrypted = false;
    bool                 control   = false;
    uint32_t             totalSize = 0;   ///< Plaintext total length for FIRST frames; 0 for other frames
    std::vector<uint8_t> payload;
};

/// Split the USB byte stream into AAP frames (a read may contain multiple frames / a partial frame).
class FrameParser {
public:
    void feed(const uint8_t* data, size_t len);
    /// Get the next frame; returns false when there is not enough data (the rest stays buffered for next time).
    bool next(Frame* out);
    void reset();

private:
    void compact_();

    std::vector<uint8_t> buf_;
    size_t               off_ = 0;
};

/// Encode one AAP frame (no framing logic; the caller invokes it per chunk).
std::vector<uint8_t> encodeFrame(Channel ch, bool encrypted, bool control,
                                 const uint8_t* payload, size_t len,
                                 bool first, bool last, uint32_t totalPlain);

}  // namespace core::projection::androidauto
