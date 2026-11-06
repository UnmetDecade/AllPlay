#include "port/control.h"

namespace core {

const char* mediaKeyName(MediaKey key)
{
    switch (key) {
    case MediaKey::Play:          return "播放";
    case MediaKey::Pause:         return "暂停";
    case MediaKey::PlayPause:     return "播放/暂停";
    case MediaKey::Stop:          return "停止";
    case MediaKey::NextTrack:     return "下一首";
    case MediaKey::PreviousTrack: return "上一首";
    case MediaKey::FastForward:   return "快进";
    case MediaKey::Rewind:        return "快退";
    case MediaKey::VolumeUp:      return "音量+";
    case MediaKey::VolumeDown:    return "音量-";
    case MediaKey::Mute:          return "静音";
    case MediaKey::Siri:          return "Siri";
    case MediaKey::Voice:         return "语音";
    case MediaKey::PhoneAccept:   return "接听";
    case MediaKey::PhoneEnd:      return "挂断";
    case MediaKey::Home:          return "主屏";
    }
    return "未知键";
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
    if (q_.size() >= kCapacity) q_.pop_front();   // 堆久了没意义，丢最老的
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
