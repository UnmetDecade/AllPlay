/**
 * @file   aap.h
 * @brief  Android Auto 协议（AAP）帧格式：通道号、消息号、分帧与拼帧
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/aap.h —— AAP 帧格式
// ============================================================================
// 每个 USB bulk 传输单元 = 一个 AAP 帧：
//   byte0 = channel id
//   byte1 = flags: bit0 FIRST | bit1 LAST | bit2 CONTROL | bit3 ENCRYPTED
//   之后是长度（大端）：普通帧 2 字节；FIRST 帧 6 字节（u16 本帧长度 + u32 明文总长）
//   payload = [2 字节大端消息号] + body（加密时整段都在 TLS 里）
//
// 一条消息 > 0x4000 字节时拆成 FIRST/MIDDLE.../LAST；每个 chunk 单独过一次 TLS，
// 所以加密帧的长度字段是**密文长度**，只有 FIRST 的 total 是明文总长。
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

// ---- control channel 消息号 ----
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

// ---- AV / 媒体通道消息号 ----
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

// ---- protobuf 枚举值（见 AASDK 的 *Enum.proto）----
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
    uint32_t             totalSize = 0;   ///< FIRST 帧的明文总长；其它帧为 0
    std::vector<uint8_t> payload;
};

/// 把 USB 读到的字节流切成 AAP 帧（允许一次读到多帧 / 半帧）。
class FrameParser {
public:
    void feed(const uint8_t* data, size_t len);
    /// 取下一帧；数据不够返回 false（剩下的留在缓冲里等下次）。
    bool next(Frame* out);
    void reset();

private:
    void compact_();

    std::vector<uint8_t> buf_;
    size_t               off_ = 0;
};

/// 编一个 AAP 帧（不含分帧逻辑；调用方按 chunk 调）。
std::vector<uint8_t> encodeFrame(Channel ch, bool encrypted, bool control,
                                 const uint8_t* payload, size_t len,
                                 bool first, bool last, uint32_t totalPlain);

}  // namespace core::projection::androidauto
