#include "port/control.h"

namespace core {

const char* mediaKeyName(MediaKey key)
{
    switch (key) {
    case MediaKey::Play:          return "Play";
    case MediaKey::Pause:         return "Pause";
    case MediaKey::PlayPause:     return "Play/Pause";
    case MediaKey::Stop:          return "Stop";
    case MediaKey::NextTrack:     return "Next track";
    case MediaKey::PreviousTrack: return "Previous track";
    case MediaKey::FastForward:   return "Fast-forward";
    case MediaKey::Rewind:        return "Rewind";
    case MediaKey::VolumeUp:      return "Volume up";
    case MediaKey::VolumeDown:    return "Volume down";
    case MediaKey::Mute:          return "Mute";
    case MediaKey::Siri:          return "Siri";
    case MediaKey::Voice:         return "Voice";
    case MediaKey::PhoneAccept:   return "Answer";
    case MediaKey::PhoneEnd:      return "Hang up";
    case MediaKey::Home:          return "Home";
    }
    return "Unknown key";
}

void MediaKeyQueue::push(MediaKey key, bool down)
{
    MediaKeyEvent ev;
    ev.key  = key;
    ev.down = down;
    push(ev);
}

void MediaKeyQueue::push(const MediaKeyEvent& ev)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.size() >= kCapacity) q_.pop_front();   // Stale entries are useless; drop the oldest
    q_.push_back(ev);
}

bool MediaKeyQueue::pop(MediaKeyEvent* out)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    if (out) *out = q_.front();
    q_.pop_front();
    return true;
}

void MediaKeyQueue::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
}

size_t MediaKeyQueue::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

}  // namespace core
