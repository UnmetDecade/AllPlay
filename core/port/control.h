/**
 * @file   control.h
 * @brief  Head-unit input port: steering-wheel controls / media keys
 * @author blt
 */

// ============================================================================
// core/port/control.h — head-unit input port: steering-wheel controls / media keys
// ============================================================================
// Direction is **head unit → phone**: steering-wheel "previous/next/play-pause" and the microphone key.
// The opposite of media.h (that one pushes video and audio from phone to head unit).
//
// This layer only defines "what a key is", not "how a key is sent":
//   · the upper layer (HarmonyOS Index.ets, desktop host key handling, future hardware key drivers) translates
//     physical events into MediaKey and pushes them into MediaKeyQueue;
//   · the protocol layer (core::iap2::WirelessTrigger / WiredLink) encodes keys from the queue and
//     sends them to the phone — the wireless one drains its queue in run(), the wired one is drained by the caller between pumps.
//
// Why no single "send" callback here: the iAP2 link is **single-threaded** (the link layer's seq/ack
// and encrypted write counter must not be touched by two threads), so sending directly from the UI thread would race. The queue is the only
// safe gate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace core {

/// Steering-wheel keys. The values themselves have no protocol meaning; mapping is done in iap2/hid.h.
enum class MediaKey : uint8_t {
    Play = 0,
    Pause,
    PlayPause,      ///< Play/pause share one physical key (most common on steering wheels)
    Stop,
    NextTrack,
    PreviousTrack,
    FastForward,
    Rewind,
    VolumeUp,       ///< Volume: CarPlay generally expects the head unit to adjust local volume itself,
    VolumeDown,     ///< these two are kept for "unified volume" vehicle models; leave unconnected by default
    Mute,
    Siri,           ///< Long-press the voice key to wake Siri
    Voice,          ///< Short-press the voice key (system voice input)
    PhoneAccept,    ///< Accept call
    PhoneEnd,       ///< End call
    Home,           ///< Back to home screen
};

/// For logging: returns a human-readable display name for the key
const char* mediaKeyName(MediaKey key);

/// One key event: press or release. The upper layer decides when to produce paired press/release
/// (most keys send once on press and once on release, using usage 0 to mean released).
struct MediaKeyEvent {
    MediaKey key  = MediaKey::PlayPause;
    bool     down = true;
};

/// Platform-independent key output. If a "steering-wheel channel not over iAP2" ever appears (e.g. a model using proprietary
/// CAN forwarding), just write another implementation of this interface.
class IMediaKeySink {
public:
    virtual ~IMediaKeySink() = default;
    virtual bool send(const MediaKeyEvent& ev) = 0;
};

/// Thread-safe event queue: upper layer pushes, protocol thread pops. Drops the oldest when full (steering-wheel keys
/// piling up is pointless; a release matters more than a "next track" from three seconds ago).
class MediaKeyQueue {
public:
    static constexpr size_t kCapacity = 32;

    void   push(MediaKey key, bool down);
    void   push(const MediaKeyEvent& ev);
    bool   pop(MediaKeyEvent* out);          ///< Returns false if the queue is empty
    void   clear();
    size_t size() const;

private:
    mutable std::mutex        mu_;
    std::deque<MediaKeyEvent> q_;
};

}  // namespace core
