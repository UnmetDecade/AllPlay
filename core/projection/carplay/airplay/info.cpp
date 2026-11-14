#include "airplay/info.h"

#include "bplist/bplist.h"

#include <cstdio>

namespace core::airplay {

const char* const kMainDisplayUuid = "b7e6c5a0-1111-4000-8000-000000000001";
const char* const kSourceVersion   = "950.7.1";
const char* const kProtoVersion    = "1.1";

using bplist::array;
using bplist::boolean;
using bplist::data;
using bplist::dict;
using bplist::i;
using bplist::Ptr;
using bplist::str;
using bplist::u;

namespace {

// ---------------------------------------------------------------------------
// HID devices (touch/knob/media/telephony). Per the AirPlay protocol's HID descriptor and
// hidDeviceEntry fields. Without HID device declarations, the iPhone tears down immediately after SETUP.
// ---------------------------------------------------------------------------

std::vector<uint8_t> multitouchDescriptor(int xMax, int yMax)
{
    auto finger = [&](std::vector<uint8_t>& b) {
        const uint8_t f[] = {
            0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,
            0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
            0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
            0x95, 0x07, 0x81, 0x03,
            0x05, 0x01, 0x26, uint8_t(xMax & 0xff), uint8_t((xMax >> 8) & 0xff),
            0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
            0x26, uint8_t(yMax & 0xff), uint8_t((yMax >> 8) & 0xff),
            0x09, 0x31, 0x81, 0x02, 0xc0,
        };
        b.insert(b.end(), f, f + sizeof f);
    };
    std::vector<uint8_t> b = {0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01};
    finger(b);   // touch 0
    finger(b);   // touch 1
    b.push_back(0xc0);
    return b;
}

std::vector<uint8_t> knobDescriptor()
{
    const uint8_t d[] = {
        0x05, 0x01, 0x09, 0x08, 0xa1, 0x01,
        0x05, 0x09, 0x09, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
        0x05, 0x0c, 0x0a, 0x23, 0x02, 0x0a, 0x24, 0x02, 0x95, 0x02, 0x81, 0x02,
        0x95, 0x05, 0x81, 0x01,
        0x05, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0,
        0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0,
    };
    return std::vector<uint8_t>(d, d + sizeof d);
}

std::vector<uint8_t> mediaDescriptor()
{
    const uint8_t d[] = {
        0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
        0x15, 0x00, 0x25, 0x06, 0x05, 0x0c,
        0x0a, 0x00, 0x00, 0x0a, 0xb0, 0x00, 0x0a, 0xb1, 0x00, 0x0a, 0xcd, 0x00,
        0x0a, 0xb5, 0x00, 0x0a, 0xb6, 0x00, 0x0a, 0x9e, 0x02,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
    };
    return std::vector<uint8_t>(d, d + sizeof d);
}

std::vector<uint8_t> telephonyDescriptor()
{
    const uint8_t d[] = {
        0x05, 0x0b, 0x09, 0x07, 0xa1, 0x01,
        0x15, 0x00, 0x25, 0x11, 0x05, 0x0b,
        0x09, 0x00, 0x09, 0x20, 0x09, 0x21, 0x09, 0x26, 0x09, 0x2f,
        0x09, 0xb0, 0x09, 0xb1, 0x09, 0xb2, 0x09, 0xb3, 0x09, 0xb4, 0x09, 0xb5,
        0x09, 0xb6, 0x09, 0xb7, 0x09, 0xb8, 0x09, 0xb9, 0x09, 0xba, 0x09, 0xbb,
        0x05, 0x07, 0x09, 0x2a,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x00, 0xc0,
    };
    return std::vector<uint8_t>(d, d + sizeof d);
}

Ptr hidDevice(uint64_t uid, const std::string& name,
              const std::vector<uint8_t>& descriptor, const std::string& displayUuid)
{
    char uidHex[16];
    std::snprintf(uidHex, sizeof uidHex, "%llx", (unsigned long long)uid);
    return dict({
        {"hidProductID",  u(1)},
        {"hidVendorID",   u(2)},
        {"hidCountryCode",u(0)},
        {"uuid",          str(uidHex)},
        {"name",          str(name)},
        {"displayUUID",   str(displayUuid)},
        {"hidDescriptor", data(descriptor)},
    });
}

/// CarPlay resource modes (the /info modes field)
Ptr resourceMode(uint64_t id)
{
    return dict({
        {"resourceID",       u(id)},
        {"transferType",     u(1)},
        {"transferPriority", u(100)},
        {"takeConstraint",   u(100)},
        {"borrowConstraint", u(100)},
        {"unborrowConstraint", u(100)},
    });
}

}  // namespace

std::vector<uint8_t> buildInfoPlist(const DeviceInfo& info)
{
    const int width = info.width, height = info.height;

    // displays[]: main screen (110). uuid uses the protocol's lowercase key name; type must be 110.
    const Ptr area = dict({
        {"widthPixels",  u((uint64_t)width)},
        {"heightPixels", u((uint64_t)height)},
        {"originXPixels",u(0)},
        {"originYPixels",u(0)},
        {"safeArea", dict({
            {"widthPixels",  u((uint64_t)width)},
            {"heightPixels", u((uint64_t)height)},
            {"originXPixels",u(0)},
            {"originYPixels",u(0)},
            {"drawUIOutsideSafeArea", boolean(true)},
        })},
    });
    std::vector<Ptr> displays = {
        dict({
            {"uuid",             str(kMainDisplayUuid)},
            {"type",             u(110)},
            {"maxFPS",           u((uint64_t)info.fps)},
            {"widthPixels",      u((uint64_t)width)},
            {"heightPixels",     u((uint64_t)height)},
            {"widthPhysical",    u(0)},
            {"heightPhysical",   u(0)},
            {"features",         u(0x0A)},          // 0x08 high-fidelity touch | 0x02 knob
            {"primaryInputDevice", boolean(true)},
            {"viewAreas",        array({area})},
            {"initialViewArea",  u(0)},
        }),
    };

    std::vector<Ptr> hidDevices = {
        hidDevice(0x2a2a2a2a, "AllPlay Touchscreen", multitouchDescriptor(width, height), kMainDisplayUuid),
        hidDevice(0x2a2a2a2b, "AllPlay Knob",        knobDescriptor(),                   kMainDisplayUuid),
        hidDevice(0x2a2a2a2c, "AllPlay Media",       mediaDescriptor(),                  kMainDisplayUuid),
        hidDevice(0x2a2a2a2d, "AllPlay Telephony",   telephonyDescriptor(),              kMainDisplayUuid),
    };

    Ptr modes = dict({
        {"resources", array({resourceMode(1), resourceMode(2)})},
        {"appStates", array({
            dict({{"appStateID", u(2)}, {"state", boolean(false)}}),
            dict({{"appStateID", u(1)}, {"speechMode", i(-1)}}),
            dict({{"appStateID", u(3)}, {"state", boolean(false)}}),
        })},
    });

    // Audio formats/latencies (per the protocol's audioFormats/audioLatencies; 48kHz)
    const uint64_t pcm48     = 0x3fcULL | 0xc000ULL;      // 48k stereo PCM
    const uint64_t pcmMono48 = 0x154ULL | 0x4000ULL;      // 48k mono PCM
    const uint64_t opus      = 0x70000000ULL;
    const uint64_t aac48     = 0x800000ULL;               // 48k stereo AAC-LC
    auto audioFormat = [](int type, const char* audioType, uint64_t outFmt, uint64_t inFmt = 0) {
        std::vector<std::pair<std::string, Ptr>> d = {
            {"type", u((uint64_t)type)},
            {"audioType", str(audioType)},
            {"audioOutputFormats", u(outFmt)},
        };
        if (inFmt) d.push_back({"audioInputFormats", u(inFmt)});   // microphone (car -> phone)
        return dict(d);
    };
    std::vector<Ptr> audioFormats = {
        audioFormat(100, "compatibility", pcm48, pcmMono48),
        audioFormat(101, "compatibility", pcm48),
        audioFormat(100, "default", pcm48 | opus, pcmMono48 | opus),
        audioFormat(100, "alert", pcm48 | opus),
        audioFormat(100, "media", pcm48),
        audioFormat(100, "telephony", pcmMono48 | opus, pcmMono48 | opus),
        audioFormat(100, "speechRecognition", pcmMono48 | opus, pcmMono48 | opus),
        audioFormat(101, "default", pcm48 | opus),
        audioFormat(102, "media", aac48),
    };
    auto audioLatency = [](int type, const char* audioType) {
        return dict({
            {"type", u((uint64_t)type)},
            {"audioType", str(audioType)},
            {"inputLatencyMicros", u(0)},
            {"outputLatencyMicros", u(0)},
        });
    };
    std::vector<Ptr> audioLatencies = {
        audioLatency(100, "default"), audioLatency(100, "media"),
        audioLatency(100, "telephony"), audioLatency(100, "speechRecognition"),
        audioLatency(100, "alert"), audioLatency(101, "default"),
        audioLatency(102, "default"),
    };

    Ptr root = dict({
        {"sourceVersion", str(kSourceVersion)},
        {"features",      u(info.features)},
        {"statusFlags",   u(4)},
        {"model",         str("AllPlay1,1")},
        {"manufacturer",  str("AllPlay")},
        {"name",          str(info.name)},
        {"deviceID",      str(info.deviceId)},
        {"bluetoothIDs",  array({str(info.btMac)})},
        {"pi",            str(info.pi)},
        {"pk",            str(info.pkHex)},
        {"srcvers",       str(kSourceVersion)},
        {"protovers",     str(kProtoVersion)},
        {"rightHandDrive",      boolean(false)},
        {"keepAliveLowPower",   boolean(false)},
        {"keepAliveSendStatsAsBody", boolean(false)},
        {"extendedFeatures",    array({str("vocoderInfo"), str("enhancedRequestCarUI")})},
        {"modes",         modes},
        {"audioFormats",  array(audioFormats)},
        {"audioLatencies",array(audioLatencies)},
        {"displays",      array(displays)},
        {"hidDevices",    array(hidDevices)},
    });

    return bplist::serialize(root);
}

}  // namespace core::airplay
