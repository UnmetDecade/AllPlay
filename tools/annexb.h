/**
 * @file   annexb.h
 * @brief  Raw H.264 elementary stream parsing and avcC/AVCC conversion (shared by video_probe and fake_phone)
 * @author blt
 */

// ============================================================================
// tools/annexb.h -- AnnexB parsing + avcC synthesis + AnnexB->AVCC
// ============================================================================
// **Why both tools share one copy**: all of video_probe's value is in the "bisection":
//   Picture appears -> problem is on the protocol side (RTSP / encryption / framing / config-frame order)
//   No picture -> problem is on the decode side (extradata / pixel format / textures)
// The bisection only holds if both sides **interpret the same stream identically**: same AU splitting,
// same avcC. Separate copies will inevitably diverge, and then "probe shows a picture, real device is black" proves nothing.
//
// Parsing only, no ffmpeg -- so fake_phone needs no decoder library linked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace allplay::annexb {

struct NalRef {
    size_t scStart;   ///< index of the first start-code byte
    size_t payload;   ///< first NAL byte (start code skipped)
    size_t end;       ///< NAL end (trailing 0x00 stripped)
    int    type;      ///< nal_unit_type (low 5 bits for H.264)
};

inline bool isVcl(int t) { return t >= 1 && t <= 5; }

/// Scan out all NALs. Both 3- and 4-byte start codes are recognized: a 4-byte code's leading 0 counts as the previous segment's
/// trailing 0x00 and is stripped (per spec, a trailing 0x00 on a NAL is trailing_zero_8bits anyway).
inline std::vector<NalRef> scanNals(const std::vector<uint8_t>& b)
{
    std::vector<NalRef> out;
    const size_t n = b.size();

    auto findSc = [&](size_t from, size_t* sc, size_t* payload) -> bool {
        for (size_t k = from; k + 3 <= n; ++k) {
            if (b[k] == 0 && b[k + 1] == 0 && b[k + 2] == 1) {
                *sc = k;
                *payload = k + 3;
                return true;
            }
        }
        return false;
    };

    size_t sc = 0, pl = 0;
    if (!findSc(0, &sc, &pl)) return out;
    for (;;) {
        size_t nsc = 0, npl = 0;
        const bool more = findSc(pl, &nsc, &npl);
        size_t end = more ? nsc : n;
        while (end > pl && b[end - 1] == 0) --end;
        if (end > pl) out.push_back(NalRef{sc, pl, end, int(b[pl] & 0x1f)});
        if (!more) break;
        sc = nsc;
        pl = npl;
    }
    return out;
}

/// Split access units: any NAL after a VCL NAL starts a new AU.
/// Returns each AU's [begin, end) in the original buffer, including the start code.
inline std::vector<std::pair<size_t, size_t>> splitAus(const std::vector<uint8_t>& b,
                                                       const std::vector<NalRef>& nals)
{
    std::vector<std::pair<size_t, size_t>> aus;
    if (nals.empty()) return aus;

    size_t auStart = nals[0].scStart;
    bool   auHasVcl = isVcl(nals[0].type);
    for (size_t i = 1; i < nals.size(); ++i) {
        const bool vcl = isVcl(nals[i].type);
        if (auHasVcl) {   // this AU already has a VCL slice, so any following NAL belongs to the next AU
            aus.push_back({auStart, nals[i].scStart});
            auStart = nals[i].scStart;
        }
        auHasVcl = vcl;
    }
    aus.push_back({auStart, b.size()});
    return aus;
}

/// Build an avcC record body from the file's SPS/PPS (first byte 0x01).
/// **Deliberately done the long way**: core hands the decoder avcC, and a local smoke test must take the same path,
/// otherwise the extradata pitfall stays untested (see the comment in host/media/ffmpeg_decoder.h).
inline bool buildAvcC(const std::vector<uint8_t>& b, const std::vector<NalRef>& nals,
                      std::vector<uint8_t>* out, std::string* err)
{
    const NalRef* sps = nullptr;
    std::vector<const NalRef*> ppss;
    for (const auto& r : nals) {
        if (r.type == 7 && !sps) sps = &r;
        else if (r.type == 8 && ppss.size() < 255) ppss.push_back(&r);
    }
    if (!sps || ppss.empty()) {
        *err = "No SPS/PPS in the file -- probably not a raw AnnexB stream; "
               "re-encode it with ffmpeg -i in.mp4 -c:v libx264 -bsf:v h264_mp4toannexb -an out.h264";
        return false;
    }
    if (sps->end - sps->payload < 4) {
        *err = "SPS is implausibly short";
        return false;
    }

    auto put16 = [&](size_t v) {
        out->push_back(uint8_t(v >> 8));
        out->push_back(uint8_t(v));
    };

    out->clear();
    out->push_back(1);                              // configurationVersion
    out->push_back(b[sps->payload + 1]);            // profile_idc
    out->push_back(b[sps->payload + 2]);            // constraint flags
    out->push_back(b[sps->payload + 3]);            // level_idc
    out->push_back(0xff);                           // length prefix is always 4 bytes
    out->push_back(0xe0 | uint8_t(1));              // reserved bits | SPS count = 1
    put16(sps->end - sps->payload);
    out->insert(out->end(), b.begin() + sps->payload, b.begin() + sps->end);
    out->push_back(uint8_t(ppss.size()));
    for (const NalRef* p : ppss) {
        put16(p->end - p->payload);
        out->insert(out->end(), b.begin() + p->payload, b.begin() + p->end);
    }
    return true;
}

/// One AnnexB access unit -> AVCC (4-byte big-endian length before each NALU).
/// This is the **streaming-direction** conversion: CarPlay's screen stream carries AVCC (because the config frame gives avcC),
/// and core converts back with avccToAnnexB (core/airplay/video_stream.cpp:170).
/// Empty return = not valid AnnexB.
inline std::vector<uint8_t> toAvcc(const std::vector<uint8_t>& b)
{
    const auto nals = scanNals(b);
    if (nals.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(b.size() + 4 * nals.size());
    for (const auto& r : nals) {
        const size_t len = r.end - r.payload;
        if (len == 0 || len > 0xFFFFFF) return {};
        out.push_back(uint8_t(len >> 24));
        out.push_back(uint8_t(len >> 16));
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len));
        out.insert(out.end(), b.begin() + r.payload, b.begin() + r.end);
    }
    return out;
}

}  // namespace allplay::annexb
