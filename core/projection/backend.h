/**
 * @file   backend.h
 * @brief  Protocol backend abstraction: common facade for CarPlay / Android Auto / CarLife / HiCar
 * @author blt
 */

// ============================================================================
// core/projection/backend.h - Protocol backend abstraction (draft: interfaces only, no implementation)
// ============================================================================
// Goal: the host only knows IProjectionBackend / IProjectionSession, not protocol-
// specific details like iAP2 or RTSP. The first backend is CarPlay - it wraps the
// existing airplay/iap2/mfi/usb code unchanged; Android Auto / CarLife / HiCar follow through the same interface.
//
// Layering:
//   host ──> IProjectionBackend (discovery + connect)
//              └─> IProjectionSession (one projection session)
//                    └─> callbacks IProjectionSink (media frames + state)
//   Media output is still implemented by the host via core::IVideoOutput / core::IAudioOutput; core does not decode.
//
// ⚠️ Threading contract (same as existing core; implementations must obey):
//   · probe() / connect()   -- caller thread; probe may be slow (ioreg / USB enumeration / mDNS);
//   · start() returns immediately; the handshake runs on an internal session thread and reports via onState();
//   · onVideoFormat/onVideoFrame come from the video thread, onAudio* from the audio thread,
//     onState from the control thread -- these three may run concurrently, so Sink implementations must do their own locking;
//   · sendInput() / requestKeyFrame() / stop() may be called from any thread;
//   · stop() is idempotent; after it returns no more callbacks occur (implementations must join internal threads).
#pragma once

#include "port/media.h"
#include "port/platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core::projection {

// ---------------------------------------------------------------------------
// Protocol and targets
// ---------------------------------------------------------------------------

enum class Protocol { CarPlay, AndroidAuto, CarLife, HiCar };

/// A connectable target discovered by probe() (a phone, or a box posing as one).
struct ProjectionTarget {
    Protocol    protocol = Protocol::CarPlay;
    /// Unique within the backend and stable across reconnects: serial / pairing ID / IP; the host treats it as an opaque key.
    std::string id;
    std::string name;       ///< For UI display
    std::string transport;  ///< "usb" / "wifi" / "bt"; display and logging only
    /// Backend-private handle (USB device, address, pairing material...). Only the backend that created it may interpret it;
    /// the host must not dereference it or pass it across backends.
    std::shared_ptr<void> handle;
};

/// Capabilities queryable before connecting; the UI uses them to decide which controls (touch/rotary/keys) to show.
struct Capabilities {
    bool touch  = false;
    bool key    = false;
    bool rotary = false;
    /// 0 = unknown / no limit.
    int  max_width  = 0;
    int  max_height = 0;
    std::vector<VideoCodec> video_codecs;
    std::vector<AudioCodec> audio_codecs;
};

// ---------------------------------------------------------------------------
// Media config (reuses core::VideoCodec / core::AudioCodec, same meaning as IVideoOutput)
// ---------------------------------------------------------------------------

struct VideoConfig {
    VideoCodec           codec = VideoCodec::H264;
    int                  width = 0;
    int                  height = 0;
    /// ISO BMFF avcC/hvcC record body (without fourcc); same meaning as IVideoOutput::configure.
    std::vector<uint8_t> codec_data;
};

struct AudioConfig {
    AudioCodec codec       = AudioCodec::LPCM;
    int        sample_rate = 48000;
    int        channels    = 2;
};

// ---------------------------------------------------------------------------
// Protocol-neutral input events
// ---------------------------------------------------------------------------
// Backends translate their protocol's HID / keys / rotary into these semantic values; the host only sees this form.

enum class InputKind { Touch, Key, Rotary };

enum class TouchPhase { Down, Move, Up, Cancel };

enum class KeyCode { Home, Back, Media, Voice, Phone, Navigation, Custom };

struct InputEvent {
    InputKind kind = InputKind::Touch;

    // -- Touch: normalized coordinates, top-left (0,0), bottom-right (1,1); follows the existing TouchPoint convention --
    TouchPhase phase   = TouchPhase::Down;
    uint8_t    pointer = 0;    ///< Contact index; 0 is the primary touch (CarPlay currently supports at most 2 contacts)
    float      x       = 0.f;
    float      y       = 0.f;

    // —— Key ——
    KeyCode    key  = KeyCode::Home;
    bool       down = true;

    // -- Rotary: 1 per detent, positive = clockwise --
    int8_t     rotary = 0;
};

// ---------------------------------------------------------------------------
// State and session callbacks
// ---------------------------------------------------------------------------

enum class SessionState {
    Idle,          ///< Constructed but not started
    Connecting,    ///< Physical link up, identifying/handshaking
    Negotiating,   ///< Authentication/pairing/service negotiation
    Running,       ///< Audio/video streaming
    Stopping,
    Disconnected,  ///< Peer or link disconnected (reconnectable; host decides)
    Failed,        ///< Unrecoverable error; detail describes the cause
};

/// Host implementation: forwards frames to IVideoOutput / IAudioOutput and state to the UI.
class IProjectionSink {
public:
    virtual ~IProjectionSink() = default;

    virtual void onVideoConfig(const VideoConfig& cfg) = 0;

    /// AnnexB frame. false = could not enqueue; the backend should re-enable the gate and request a key frame
    /// (same contract as IVideoOutput::feedAnnexB).
    virtual bool onVideoFrame(const uint8_t* data, size_t len) = 0;

    /// Clear the reference chain before key frame recovery / a new IDR. Default no-op: hosts that do not decode can ignore it.
    virtual void onVideoReset() {}
    virtual void onVideoFlush() {}

    virtual void onAudioConfig(const AudioConfig& cfg) = 0;

    /// One audio payload frame (raw AAC frame / raw Opus packet / raw LPCM), pts = timestamp.
    virtual void onAudioFrame(const uint8_t* payload, size_t len, uint32_t pts) = 0;

    /// Audio stream ended/rebuilt.
    virtual void onAudioReset() {}

    virtual void onState(SessionState state, const std::string& detail) = 0;
};

// ---------------------------------------------------------------------------
// Sessions and backends
// ---------------------------------------------------------------------------

class IProjectionSession {
public:
    virtual ~IProjectionSession() = default;

    /// Returns immediately; handshake/negotiation runs on an internal thread and reports via onState().
    virtual bool start(std::string* err) = 0;

    /// Idempotent. No callbacks after it returns.
    virtual void stop() = 0;

    /// Send input. Returns false when not yet Running.
    virtual bool sendInput(const InputEvent& ev, std::string* err) = 0;

    /// Request a key frame (used on frame loss/recovery).
    virtual void requestKeyFrame() = 0;

    virtual Capabilities capabilities() const = 0;
};

class IProjectionBackend {
public:
    virtual ~IProjectionBackend() = default;

    virtual Protocol    protocol() const = 0;
    virtual const char* name() const = 0;

    /// Discover connectable targets. May be slow; the host decides which thread to call it on.
    /// services provides platform capabilities such as Bluetooth (wireless CarPlay candidates are paired phones).
    virtual std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                                std::string* err) = 0;

    /// Create a session; returns nullptr and writes err on failure. The caller owns the session lifetime.
    /// services is injected by the composition root (same convention as Receiver::start).
    virtual std::unique_ptr<IProjectionSession> connect(
        const ProjectionTarget&          target,
        const core::PlatformServices&    services,
        IProjectionSink&                 sink,
        std::string*                     err) = 0;
};

/// Protocol registry: the host picks a backend by name/enum without including any backend header.
class BackendRegistry {
public:
    void add(std::unique_ptr<IProjectionBackend> backend);
    IProjectionBackend* find(Protocol p) const;
    const std::vector<std::unique_ptr<IProjectionBackend>>& all() const { return backends_; }

private:
    std::vector<std::unique_ptr<IProjectionBackend>> backends_;
};

}  // namespace core::projection
