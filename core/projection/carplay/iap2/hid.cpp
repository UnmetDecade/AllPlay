#include "iap2/hid.h"

#include "iap2/wire.h"

namespace core::iap2 {

namespace {
constexpr uint16_t kPageConsumer  = 0x000c;
constexpr uint16_t kPageTelephony = 0x000b;

// HID Usage Tables: common Consumer page keys
constexpr uint16_t kPlay          = 0x00b0;
constexpr uint16_t kPause         = 0x00b1;
constexpr uint16_t kFastForward   = 0x00b3;
constexpr uint16_t kRewind        = 0x00b4;
constexpr uint16_t kScanNext      = 0x00b5;
constexpr uint16_t kScanPrev      = 0x00b6;
constexpr uint16_t kStop          = 0x00b7;
constexpr uint16_t kPlayPause     = 0x00cd;
constexpr uint16_t kVolumeDown    = 0x00ea;
constexpr uint16_t kVolumeUp      = 0x00e9;
constexpr uint16_t kMute          = 0x00e2;
constexpr uint16_t kVoiceCommand  = 0x00cf;
constexpr uint16_t kAcHome        = 0x0223;

// Telephony page: phone keys (the Consumer page has no answer/hang-up)
constexpr uint16_t kHookSwitch    = 0x0020;   // off-hook = answer, on-hook = hang up
constexpr uint16_t kPhoneDrop      = 0x0026;
}  // namespace

HidUsage mediaKeyUsage(core::MediaKey key)
{
    switch (key) {
    case core::MediaKey::Play:          return {kPageConsumer, kPlay};
    case core::MediaKey::Pause:         return {kPageConsumer, kPause};
    case core::MediaKey::PlayPause:     return {kPageConsumer, kPlayPause};
    case core::MediaKey::Stop:          return {kPageConsumer, kStop};
    case core::MediaKey::NextTrack:     return {kPageConsumer, kScanNext};
    case core::MediaKey::PreviousTrack: return {kPageConsumer, kScanPrev};
    case core::MediaKey::FastForward:   return {kPageConsumer, kFastForward};
    case core::MediaKey::Rewind:        return {kPageConsumer, kRewind};
    case core::MediaKey::VolumeUp:      return {kPageConsumer, kVolumeUp};
    case core::MediaKey::VolumeDown:    return {kPageConsumer, kVolumeDown};
    case core::MediaKey::Mute:          return {kPageConsumer, kMute};
    // Siri has no dedicated HID usage; the common industry practice is to report "voice command",
    // and the phone decides between Siri and system dictation per its own policy.
    case core::MediaKey::Siri:          return {kPageConsumer, kVoiceCommand};
    case core::MediaKey::Voice:         return {kPageConsumer, kVoiceCommand};
    case core::MediaKey::PhoneAccept:   return {kPageTelephony, kHookSwitch};
    case core::MediaKey::PhoneEnd:      return {kPageTelephony, kPhoneDrop};
    case core::MediaKey::Home:          return {kPageConsumer, kAcHome};
    }
    return {};
}

std::vector<uint8_t> hidReport(core::MediaKey key, bool down, uint8_t reportId)
{
    const HidUsage u = down ? mediaKeyUsage(key) : HidUsage{};
    std::vector<uint8_t> out;
    if (reportId != 0) out.push_back(reportId);
    out.push_back(uint8_t(u.page & 0xff));
    out.push_back(uint8_t(u.page >> 8));
    out.push_back(uint8_t(u.id & 0xff));
    out.push_back(uint8_t(u.id >> 8));
    return out;
}

std::vector<uint8_t> hidReportMessage(const std::vector<uint8_t>& report)
{
    // Parameter 0 = report bytes. If a real peer requires a different parameter id, change only this line.
    Params p;
    p.bytes(0, report);
    return csmFrame(kAccessoryHidReport, p.encode());
}

std::vector<uint8_t> hidReportMessage(core::MediaKey key, bool down, uint8_t reportId)
{
    return hidReportMessage(hidReport(key, down, reportId));
}

}  // namespace core::iap2
