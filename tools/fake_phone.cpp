// ============================================================================
// tools/fake_phone.cpp -- fake iPhone: real sockets run RTSP + encrypted screen stream + touch readback
// ============================================================================
// This is the **only evidence** that "a Linux host really can render a CarPlay picture". With no iPhone,
// the remaining way to prove it is to act as the phone ourselves: real TCP, real TLV8, real SRP, real ChaCha20-Poly1305,
// skipping no step. The host's decode path is covered separately by tools/video_probe; together they span the whole chain.
//
// Flow (one-to-one with the routes in core/airplay/session.cpp):
//   1. OPTIONS                 -- liveness check, verifies the RTSP port + keep-alive
//   2. GET  /info              -- read bplist, confirm the peer is this project's device (print name/features)
//      skip /auth-setup       -- requires MFi identity files (identity/), deliberately not installed for local dev;
//                                a real iPhone requires this step, so this tool **cannot** replace real-device acceptance
//   3. POST /pair-setup  M1->M6 -- SRP-6a (PIN 3939) + exchange the controller's long-term public key
//   4. POST /pair-verify PV1->PV4 -- X25519 + signatures, obtaining the shared secret
//      **The PV-M4 response is still plaintext**; after that all control-channel bytes use ControlCipher frames
//   5. SETUP in two batches    -- first gets eventPort, second gets the screen stream's dataPort
//   6. RECORD                  -- the host switches the startup screen to the video surface
//   7. Connect to dataPort and stream -- send the config frame first (opcode 1, plaintext), then encrypted video frames (opcode 0)
//   8. Connect to eventPort and read touches -- decrypt the host's hidSendReport and decode/print the touch points
//
// Step 8 is the end-to-end acceptance test for touch: the host normalizes SDL mouse/finger input into the letterbox rect and
// encodes it as an HID report sent encrypted over the event channel. Recovering the coordinates here is what proves the whole touch chain works.
//
// Generate a test stream (any container with H.264 works; --aac uses 48kHz stereo ADTS):
//   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=60 -t 10 -c:v libx264
//     -preset veryfast -pix_fmt yuv420p -g 60 -bf 0 -bsf:v h264_mp4toannexb -f h264 out.h264
//   ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" -ac 2
//     -c:a aac -b:a 128k -f adts tone.aac
// (Note: don't end comment lines with a backslash: in C++ that swallows the next line into the comment.)
//
// Usage:
//   build/allplay_host &
//   build/fake_phone --h264 out.h264 --fps 60
#include "airplay/control_cipher.h"
#include "bplist/bplist.h"
#include "crypto/hkdf.h"
#include "crypto/tlv8.h"
#include "crypto/x25519.h"

#include "annexb.h"
#include "phone_crypto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using allplay::phone::sb;
using allplay::phone::T_ENCRYPTED_DATA;
using allplay::phone::T_IDENTIFIER;
using allplay::phone::T_PROOF;
using allplay::phone::T_PUBLIC_KEY;
using allplay::phone::T_SALT;
using allplay::phone::T_SIGNATURE;
using allplay::phone::T_STATE;

namespace {

// The fake phone's "identity": like a real device, a stable controller id and a long-term Ed25519 key pair.
// Pinning the UUID is intentional -- the host stores it in PairingStore, so reruns take the "already paired" branch.
constexpr const char* kControllerId = "11111111-2222-3333-4444-555555555555";

// ---------------------------------------------------------------------------
// Raw TCP (tools/ doesn't depend on core's port layer, so use POSIX sockets directly)
// ---------------------------------------------------------------------------
class Link {
public:
    ~Link() { closeFd(); }
    Link() = default;
    Link(const Link&) = delete;

    bool connectTo(const std::string& host, uint16_t port, const char* what, std::string* err)
    {
        closeFd();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            *err = std::string("socket creation failed: ") + std::strerror(errno);
            return false;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
            *err = "host is not a valid IPv4 address: " + host;
            closeFd();
            return false;
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            *err = std::string(what) + " cannot connect to " + host + ":" + std::to_string(port) +
                   " -- " + std::strerror(errno);
            closeFd();
            return false;
        }
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return true;
    }

    bool writeAll(const uint8_t* d, size_t n)
    {
        size_t off = 0;
        while (off < n) {
            const ssize_t w = ::send(fd_, d + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) return false;
            off += size_t(w);
        }
        return true;
    }

    /// Returns >0 = byte count; 0 = timeout; <0 = closed/error
    int readSome(uint8_t* d, size_t n, int timeoutMs)
    {
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const ssize_t r = ::recv(fd_, d, n, 0);
        if (r > 0) return int(r);
        if (r == 0) return -1;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }

    void closeFd()
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
    bool ok() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// Raw UDP send (audio goes over RTP/UDP and can't use TCP like the screen stream)
// ---------------------------------------------------------------------------
class UdpSender {
public:
    UdpSender() = default;   // declaring a copy constructor suppresses the implicit default constructor, so add it back
    ~UdpSender() { if (fd_ >= 0) ::close(fd_); }
    UdpSender(const UdpSender&) = delete;

    bool openTo(const std::string& host, uint16_t port, std::string* err)
    {
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        const std::string p = std::to_string(port);
        if (::getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0 || !res) {
            *err = "cannot resolve audio destination address " + host;
            return false;
        }
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ >= 0) {
            std::memcpy(&addr_, res->ai_addr, size_t(res->ai_addrlen));
            addrLen_ = socklen_t(res->ai_addrlen);
        }
        ::freeaddrinfo(res);
        if (fd_ < 0) { *err = std::string("UDP socket creation failed: ") + std::strerror(errno); return false; }
        return true;
    }

    bool send(const uint8_t* d, size_t n)
    {
        return ::sendto(fd_, d, n, 0, reinterpret_cast<sockaddr*>(&addr_), addrLen_) == ssize_t(n);
    }

private:
    int            fd_ = -1;
    sockaddr_in    addr_{};
    socklen_t      addrLen_ = 0;
};

// ---------------------------------------------------------------------------
// ADTS elementary stream (.aac) -> raw AAC frames
// ---------------------------------------------------------------------------
// CarPlay's AAC payload is **raw frames with no ADTS header**, while the material on hand is basically all ADTS,
// so strip a layer here. Also read out the sample rate/channels -- negotiated values must match the material;
// a mismatch means "sound comes out but it's all noise", so failing fast is better.
struct AacFrames {
    std::vector<std::vector<uint8_t>> frames;
    int freqIndex = -1;   ///< ADTS sample-rate index (3 = 48000, 4 = 44100)
    int channels  = 0;
};

bool readFile(const std::string& path, std::vector<uint8_t>* out);   // defined below

bool readAdts(const std::string& path, AacFrames* out)
{
    std::vector<uint8_t> buf;
    if (!readFile(path, &buf)) return false;

    size_t i = 0;
    bool   sawHeader = false;
    while (i + 7 <= buf.size()) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xF0) != 0xF0) { ++i; continue; }   // look for the sync word
        const size_t hdr = (buf[i + 1] & 0x01) ? 7 : 9;   // 2 extra CRC bytes when protection_absent=0
        const size_t len = (size_t(buf[i + 3] & 0x03) << 11) |
                           (size_t(buf[i + 4]) << 3) |
                           (size_t(buf[i + 5]) >> 5);
        if (len < hdr || i + len > buf.size()) { ++i; continue; }

        if (!sawHeader) {
            sawHeader     = true;
            out->freqIndex = int((buf[i + 2] >> 2) & 0x0F);
            out->channels  = int(((buf[i + 2] & 0x01) << 2) | (buf[i + 3] >> 6));
        }
        out->frames.emplace_back(buf.begin() + i + hdr, buf.begin() + i + len);
        i += len;
    }
    return !out->frames.empty();
}

// ---------------------------------------------------------------------------
// RTSP client: request line + headers + Content-Length body; can switch to ControlCipher encryption
// ---------------------------------------------------------------------------
struct Response {
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;

    std::string header(const std::string& name) const
    {
        for (const auto& kv : headers)
            if (kv.first.size() == name.size() &&
                ::strncasecmp(kv.first.c_str(), name.c_str(), name.size()) == 0)
                return kv.second;
        return {};
    }
};

class Rtsp {
public:
    explicit Rtsp(Link& link) : link_(link) {}

    /// Call after pair-verify: all subsequent send/receive goes through encrypted frames.
    /// Note the argument order -- ControlCipher(readKey=decrypt inbound, writeKey=encrypt outbound),
    /// while the server's read/write naming is **exactly reversed** from the phone side; see the comment on controlKeys().
    void enableCipher(std::unique_ptr<core::airplay::ControlCipher> c)
    {
        cipher_ = std::move(c);
        plain_.clear();
    }

    bool request(const std::string& method, const std::string& uri,
                 const std::string& contentType, const std::vector<uint8_t>& body,
                 Response* out, std::string* err)
    {
        std::string head = method + " " + uri + " RTSP/1.0\r\n";
        head += "CSeq: " + std::to_string(++cseq_) + "\r\n";
        head += "User-Agent: AllPlay-fake-phone/1.0\r\n";
        if (!contentType.empty()) head += "Content-Type: " + contentType + "\r\n";
        head += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

        std::vector<uint8_t> wire(head.begin(), head.end());
        wire.insert(wire.end(), body.begin(), body.end());

        if (cipher_) wire = cipher_->encrypt(wire.data(), wire.size());
        if (!link_.writeAll(wire.data(), wire.size())) {
            *err = method + " " + uri + " send failed (peer may have disconnected)";
            return false;
        }
        return readResponse(method + " " + uri, out, err);
    }

private:
    /// Buffer bytes from the link into plain_ (decrypting first when encrypted). false = disconnected.
    bool pull(std::string* err)
    {
        uint8_t buf[4096];
        const int n = link_.readSome(buf, sizeof buf, 10000);
        if (n == 0) return true;                     // timeout: no new bytes, let the caller decide again
        if (n < 0) { *err = "peer disconnected while reading the response"; return false; }

        if (!cipher_) {
            plain_.insert(plain_.end(), buf, buf + n);
            return true;
        }
        cipher_->feed(buf, size_t(n));
        std::vector<uint8_t> pt;
        while (cipher_->next(&pt)) {
            if (!pt.empty()) plain_.insert(plain_.end(), pt.begin(), pt.end());
            pt.clear();
        }
        if (cipher_->failed()) { *err = "control channel decryption/auth failed (keys swapped?)"; return false; }
        return true;
    }

    bool readResponse(const std::string& what, Response* out, std::string* err)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            // find the end of headers (\r\n\r\n), then take the body per Content-Length
            const std::vector<uint8_t> delim = {'\r', '\n', '\r', '\n'};
            auto it = std::search(plain_.begin(), plain_.end(), delim.begin(), delim.end());
            if (it != plain_.end()) {
                const size_t headLen = size_t(it - plain_.begin());
                const size_t bodyStart = headLen + 4;

                Response r;
                std::string head(reinterpret_cast<const char*>(plain_.data()), headLen);
                size_t ls = 0;
                bool first = true;
                while (ls <= head.size()) {
                    size_t le = head.find("\r\n", ls);
                    const std::string l = head.substr(
                        ls, le == std::string::npos ? std::string::npos : le - ls);
                    if (first) {
                        std::sscanf(l.c_str(), "RTSP/1.0 %d", &r.status);
                        const size_t sp = l.find(' ', l.find(' ') + 1);
                        if (sp != std::string::npos) r.reason = l.substr(sp + 1);
                        first = false;
                    } else if (!l.empty()) {
                        const size_t c = l.find(':');
                        if (c != std::string::npos) {
                            std::string v = l.substr(c + 1);
                            const size_t s = v.find_first_not_of(" \t");
                            r.headers.emplace_back(l.substr(0, c),
                                                   s == std::string::npos ? "" : v.substr(s));
                        }
                    }
                    if (le == std::string::npos) break;
                    ls = le + 2;
                }
                long len = 0;
                const std::string cl = r.header("Content-Length");
                if (!cl.empty()) len = std::strtol(cl.c_str(), nullptr, 10);
                if (len < 0) len = 0;

                const size_t total = bodyStart + size_t(len);
                if (plain_.size() >= total) {
                    r.body.assign(plain_.begin() + bodyStart, plain_.begin() + total);
                    plain_.erase(plain_.begin(), plain_.begin() + total);
                    *out = std::move(r);
                    return true;
                }
            }

            if (!pull(err)) return false;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                *err = what + " timed out waiting for a response (host didn't reply?)";
                return false;
            }
        }
    }

    Link& link_;
    std::unique_ptr<core::airplay::ControlCipher> cipher_;
    std::vector<uint8_t> plain_;
    uint64_t cseq_ = 0;
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    std::string host = "127.0.0.1";
    uint16_t    port = 7000;
    std::string h264;
    std::string aac;               ///< --aac: raw ADTS stream; create an audio stream and push it (omit to skip audio verification)
    int         fps    = 30;
    bool        loop   = false;
    int         frames = 0;        // >0: stop after pushing N frames
    std::string pin    = "3939";
    int         seconds = 0;       // >0: run streaming for at most N seconds
    int         videoDelayMs = 0;  // --video-delay: how long to idle after RECORD, before pushing the first frame
                                   //   used to reproduce the gap where "the video surface is active but no new picture yet" (checking for stale frames)
    bool        noTouch = false;
    bool        verbose = false;
    bool        help    = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "Usage: %s --h264 FILE [options]\n"
        "  --h264 PATH     raw AnnexB H.264 elementary stream (pushed to the host as the screen stream)\n"
        "  --host IP       host address (default 127.0.0.1)\n"
        "  --port N        host RTSP port (default 7000)\n"
        "  --fps N         streaming frame rate (default 30)\n"
        "  --loop          replay from the start when finished\n"
        "  --frames N      stop after N frames (default 0 = go through the whole file once)\n"
        "  --seconds N     stream for at most N seconds\n"
        "  --video-delay MS  idle for MS milliseconds after RECORD before pushing the first frame (reproduces the gap after switching to the video surface)\n"
        "  --pin S         PIN for pair-setup (default 3939)\n"
        "  --aac PATH      48kHz stereo raw ADTS stream; if given, create an audio stream and push it (verifies the audio path)\n"
        "  --no-touch      don't connect the event channel (skip touch verification)\n"
        "  -v, --verbose   print every RTSP send/receive\n"
        "  -h, --help      show this help\n"
        "\n"
        "Exit code: 0 = completed the full flow and pushed frames; non-zero = some step failed (see stderr).\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "argument %s is missing a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--h264"))            o->h264 = need(a);
        else if (!std::strcmp(a, "--aac"))        o->aac = need(a);
        else if (!std::strcmp(a, "--host"))       o->host = need(a);
        else if (!std::strcmp(a, "--port"))       o->port = uint16_t(std::atoi(need(a)));
        else if (!std::strcmp(a, "--fps"))        o->fps = std::atoi(need(a));
        else if (!std::strcmp(a, "--loop"))       o->loop = true;
        else if (!std::strcmp(a, "--frames"))     o->frames = std::atoi(need(a));
        else if (!std::strcmp(a, "--seconds"))    o->seconds = std::atoi(need(a));
        else if (!std::strcmp(a, "--video-delay")) o->videoDelayMs = std::atoi(need(a));
        else if (!std::strcmp(a, "--pin"))        o->pin = need(a);
        else if (!std::strcmp(a, "--no-touch"))   o->noTouch = true;
        else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--verbose")) o->verbose = true;
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help"))    o->help = true;
        else {
            std::fprintf(stderr, "unknown argument: %s (see -h for help)\n", a);
            return false;
        }
    }
    if (o->help) return true;
    if (o->h264.empty()) {
        std::fprintf(stderr, "--h264 FILE is required (see -h for help)\n");
        return false;
    }
    if (o->fps <= 0 || o->fps > 240) {
        std::fprintf(stderr, "--fps out of range (1..240)\n");
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fprintf(stderr, "%s is an empty file\n", path.c_str());
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = std::fread(out->data(), 1, size_t(n), f);
    std::fclose(f);
    return got == size_t(n);
}

// ---------------------------------------------------------------------------
// Screen stream frame pushing
// ---------------------------------------------------------------------------
struct ScreenFrame {
    std::vector<uint8_t> avcc;   ///< already converted to AVCC (4-byte length prefix), can be encrypted directly
    bool                 idr = false;
};

/// 128-byte data stream header: bodyLen(LE32) + opcode + rest zero-filled.
std::vector<uint8_t> makeHeader(uint32_t bodyLen, uint8_t opcode)
{
    std::vector<uint8_t> h(128, 0);
    h[0] = uint8_t(bodyLen);
    h[1] = uint8_t(bodyLen >> 8);
    h[2] = uint8_t(bodyLen >> 16);
    h[3] = uint8_t(bodyLen >> 24);
    h[4] = opcode;
    return h;
}

// ---------------------------------------------------------------------------
// Touch readback: connect to the event channel, decrypt the host's POST /command, recover touch points
// ---------------------------------------------------------------------------
void touchReader(std::string host, uint16_t port, std::vector<uint8_t> shared,
                 int width, int height, std::atomic<bool>* stop,
                 std::atomic<int>* got)
{
    Link lk;
    std::string err;
    if (!lk.connectTo(host, port, "event channel", &err)) {
        std::fprintf(stderr, "[phone] event channel: %s\n", err.c_str());
        return;
    }

    // Server's event_channel.cpp:112-114 uses writeKey_ for **outbound** and readKey_ for **inbound**;
    // the phone side swaps them. This pair also doesn't share labels with the screen/control channels, so don't extrapolate.
    auto evWrite = core::crypto::hkdfSha512(shared, "Events-Salt", "Events-Write-Encryption-Key", 32);
    auto evRead  = core::crypto::hkdfSha512(shared, "Events-Salt", "Events-Read-Encryption-Key", 32);
    core::airplay::ControlCipher cipher(evWrite.data(),   // decrypt inbound (commands the host sends)
                                        evRead.data());   // encrypt outbound (our ACK)

    std::vector<uint8_t> plain;
    uint8_t buf[4096];
    while (!stop->load()) {
        const int n = lk.readSome(buf, sizeof buf, 200);
        if (n < 0) break;
        if (n == 0) continue;
        cipher.feed(buf, size_t(n));
        std::vector<uint8_t> pt;
        while (cipher.next(&pt)) {
            if (!pt.empty()) plain.insert(plain.end(), pt.begin(), pt.end());
            pt.clear();
        }

        // cut a complete request out of the plaintext (\r\n\r\n + Content-Length)
        for (;;) {
            const std::vector<uint8_t> delim = {'\r', '\n', '\r', '\n'};
            auto it = std::search(plain.begin(), plain.end(), delim.begin(), delim.end());
            if (it == plain.end()) break;
            const size_t headLen = size_t(it - plain.begin());
            const size_t bodyStart = headLen + 4;

            std::string head(reinterpret_cast<const char*>(plain.data()), headLen);
            long len = 0;
            const size_t cl = head.find("Content-Length:");
            if (cl != std::string::npos) len = std::strtol(head.c_str() + cl + 15, nullptr, 10);
            if (len < 0) len = 0;
            if (plain.size() < bodyStart + size_t(len)) break;

            const std::string cseqH = "CSeq:";
            std::string cseq = "0";
            const size_t cs = head.find(cseqH);
            if (cs != std::string::npos) {
                const size_t e = head.find("\r\n", cs);
                cseq = head.substr(cs + cseqH.size(), e - cs - cseqH.size());
                const size_t s = cseq.find_first_not_of(" \t");
                if (s != std::string::npos) cseq = cseq.substr(s);
            }

            std::vector<uint8_t> body(plain.begin() + bodyStart,
                                      plain.begin() + bodyStart + size_t(len));
            plain.erase(plain.begin(), plain.begin() + bodyStart + size_t(len));

            // only parse requests (responses look like "RTSP/1.0 200", skip them)
            if (head.rfind("RTSP/", 0) == 0 || head.rfind("HTTP/", 0) == 0) continue;

            auto root = core::bplist::parse(body.data(), body.size());
            const std::string type =
                root ? core::bplist::strOf(core::bplist::dictGet(root, "type")) : "";
            if (type != "hidSendReport") {
                std::printf("[phone] event channel received type=%s\n", type.c_str());
                continue;
            }

            const auto rep = core::bplist::dictGet(root, "hidReport");
            if (!rep || rep->data.size() < 12) continue;

            // HID report: 2 touch points × 6 bytes (slot | down | x u16le | y u16le)
            std::string desc;
            int downs = 0;
            for (int slot = 0; slot < 2; ++slot) {
                const size_t off = size_t(slot) * 6;
                const bool down = rep->data[off + 1] != 0;
                const int x = rep->data[off + 2] | (rep->data[off + 3] << 8);
                const int y = rep->data[off + 4] | (rep->data[off + 5] << 8);
                if (down) ++downs;
                char one[96];
                std::snprintf(one, sizeof one, "slot%d %s(%d,%d) ", slot,
                              down ? "down" : "up", x, y);
                desc += one;
            }
            std::printf("[phone] touch ← %s (normalized %.3f,%.3f approx.)\n",
                        desc.c_str(),
                        double(rep->data[2] | (rep->data[3] << 8)) / double(width),
                        double(rep->data[4] | (rep->data[5] << 8)) / double(height));
            got->fetch_add(downs > 0 ? 1 : 0);

            // send the ACK (the server waits for it; not sending isn't fatal, but a real phone would)
            std::string ack = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\nContent-Length: 0\r\n\r\n";
            auto framed = cipher.encrypt(reinterpret_cast<const uint8_t*>(ack.data()), ack.size());
            if (!lk.writeAll(framed.data(), framed.size())) break;
        }
    }
}

std::vector<uint8_t> hkdf(const std::vector<uint8_t>& shared, const char* salt, const char* info)
{
    return core::crypto::hkdfSha512(shared, salt, info, 32);
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 2;
    if (opt.help) { printUsage(argv[0]); return 0; }

    std::vector<uint8_t> file;
    if (!readFile(opt.h264, &file)) return 1;

    const auto nals = allplay::annexb::scanNals(file);
    const auto aus  = allplay::annexb::splitAus(file, nals);
    std::vector<uint8_t> codecData;
    std::string err;
    if (!allplay::annexb::buildAvcC(file, nals, &codecData, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (aus.empty()) {
        std::fprintf(stderr, "no access units found in %s -- not an AnnexB stream?\n", opt.h264.c_str());
        return 1;
    }

    // Convert the whole file to AVCC up front (the streaming direction needs AVCC; core converts it back to AnnexB on receipt)
    std::vector<ScreenFrame> frames;
    frames.reserve(aus.size());
    for (const auto& r : aus) {
        std::vector<uint8_t> au(file.begin() + r.first, file.begin() + r.second);
        ScreenFrame f;
        f.avcc = allplay::annexb::toAvcc(au);
        if (f.avcc.empty()) continue;
        for (const auto& nr : allplay::annexb::scanNals(au))
            if (nr.type == 5) { f.idr = true; break; }
        frames.push_back(std::move(f));
    }
    if (frames.empty()) {
        std::fprintf(stderr, "no frames to push in %s\n", opt.h264.c_str());
        return 1;
    }
    std::printf("[phone] %s: %zu access units, avcC %zu bytes, first frame %s\n",
                opt.h264.c_str(), frames.size(), codecData.size(),
                frames[0].idr ? "is a keyframe" : "**is not a keyframe** (the host will wait for the next IDR)");
    if (!frames[0].idr)
        std::printf("[phone] ⚠️ first frame is not an IDR: the host's gate drops non-keyframes after it, "
                    "re-encode with a smaller -g (e.g. -g 30)\n");

    const std::string baseUri = "rtsp://" + opt.host + ":" + std::to_string(opt.port);

    // ---- 1) OPTIONS ----
    Link ctrl;
    if (!ctrl.connectTo(opt.host, opt.port, "RTSP control channel", &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }
    Rtsp rtsp(ctrl);
    Response resp;
    if (!rtsp.request("OPTIONS", baseUri + "/", "", {}, &resp, &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }
    std::printf("[phone] OPTIONS → %d %s (Public: %s)\n", resp.status, resp.reason.c_str(),
                resp.header("Public").substr(0, 40).c_str());
    if (resp.status != 200) return 1;

    // ---- 2) GET /info ----
    if (!rtsp.request("GET", baseUri + "/info", "", {}, &resp, &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }
    {
        auto root = core::bplist::parse(resp.body.data(), resp.body.size());
        const std::string name = root ? core::bplist::strOf(core::bplist::dictGet(root, "name")) : "";
        std::printf("[phone] GET /info → %d, device name \"%s\", %zu bytes of bplist\n",
                    resp.status, name.c_str(), resp.body.size());
    }

    // ---- 3) POST /pair-setup (SRP-6a, PIN 3939) ----
    std::printf("[phone] -- pair-setup (PIN %s) --\n", opt.pin.c_str());
    const std::string kTlv = "application/pairing+tlv8";

    uint8_t cpub[32] = {};
    EVP_PKEY* cpriv = allplay::phone::genEd25519(cpub);
    if (!cpriv) {
        std::fprintf(stderr, "[phone] failed to generate Ed25519 key\n");
        return 1;
    }

    std::vector<uint8_t> pairEncKey, pairSessionK;
    std::string accId;
    std::vector<uint8_t> accPub;
    {
        // M1 → M2
        if (!rtsp.request("POST", baseUri + "/pair-setup", kTlv,
                          core::crypto::tlv8Encode({{T_STATE, {1}}}), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto items = core::crypto::tlv8Decode(resp.body.data(), resp.body.size());
        const auto* B    = core::crypto::tlv8Get(items, T_PUBLIC_KEY);
        const auto* salt = core::crypto::tlv8Get(items, T_SALT);
        if (resp.status != 200 || !B || !salt) {
            std::fprintf(stderr, "[phone] M2 error: status=%d B=%p salt=%p\n",
                         resp.status, (const void*)B, (const void*)salt);
            return 1;
        }

        // M3 → M4
        allplay::phone::SrpPhoneSide phone(opt.pin);
        std::vector<uint8_t> M1;
        phone.prove(*salt, *B, &M1);
        std::vector<core::crypto::Tlv> m3 = {
            {T_STATE, {3}}, {T_PUBLIC_KEY, phone.Apad}, {T_PROOF, M1}};
        if (!rtsp.request("POST", baseUri + "/pair-setup", kTlv,
                          core::crypto::tlv8Encode(m3), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto it4 = core::crypto::tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st4 = core::crypto::tlv8Get(it4, T_STATE);
        const auto* M2  = core::crypto::tlv8Get(it4, T_PROOF);
        if (!st4 || st4->front() != 4 || !M2 || !phone.checkM2(*M2)) {
            std::fprintf(stderr, "[phone] SRP failed: M4 verification failed (wrong PIN?)\n");
            return 1;
        }
        std::printf("[phone] SRP passed (M4 verification OK)\n");
        pairSessionK = phone.K;
        pairEncKey = hkdf(pairSessionK, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");

        // M5 → M6
        auto cSignKey = hkdf(pairSessionK, "Pair-Setup-Controller-Sign-Salt",
                             "Pair-Setup-Controller-Sign-Info");
        std::vector<uint8_t> signData = cSignKey;
        signData.insert(signData.end(), kControllerId, kControllerId + std::strlen(kControllerId));
        signData.insert(signData.end(), cpub, cpub + 32);
        auto cSig = core::crypto::ed25519Sign(cpriv, signData.data(), signData.size());

        std::vector<core::crypto::Tlv> inner = {
            {T_IDENTIFIER, sb(kControllerId)},
            {T_PUBLIC_KEY, std::vector<uint8_t>(cpub, cpub + 32)},
            {T_SIGNATURE, cSig},
        };
        auto innerBytes = core::crypto::tlv8Encode(inner);
        auto sealed = core::crypto::chachaSeal(pairEncKey.data(),
                                               core::crypto::nonceLabel("PS-Msg05"),
                                               nullptr, 0, innerBytes.data(), innerBytes.size());
        std::vector<core::crypto::Tlv> m5 = {{T_STATE, {5}}, {T_ENCRYPTED_DATA, sealed}};
        if (!rtsp.request("POST", baseUri + "/pair-setup", kTlv,
                          core::crypto::tlv8Encode(m5), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto it6 = core::crypto::tlv8Decode(resp.body.data(), resp.body.size());
        const auto* enc6 = core::crypto::tlv8Get(it6, T_ENCRYPTED_DATA);
        std::vector<uint8_t> accInnerBytes;
        if (!enc6 || !core::crypto::chachaOpen(pairEncKey.data(),
                                               core::crypto::nonceLabel("PS-Msg06"),
                                               nullptr, 0, enc6->data(), enc6->size(),
                                               &accInnerBytes)) {
            std::fprintf(stderr, "[phone] M6 decryption failed (host has no MFi identity? is identity/ installed?)\n");
            return 1;
        }
        auto accInner = core::crypto::tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
        const auto* aid = core::crypto::tlv8Get(accInner, T_IDENTIFIER);
        const auto* apk = core::crypto::tlv8Get(accInner, T_PUBLIC_KEY);
        const auto* asg = core::crypto::tlv8Get(accInner, T_SIGNATURE);
        if (!aid || !apk || !asg) {
            std::fprintf(stderr, "[phone] M6 inner payload is missing fields\n");
            return 1;
        }
        // Verify the host's long-term signature -- this proves "the peer really is the identity just generated", not a formality
        auto aSignKey = hkdf(pairSessionK, "Pair-Setup-Accessory-Sign-Salt",
                             "Pair-Setup-Accessory-Sign-Info");
        std::vector<uint8_t> aData = aSignKey;
        aData.insert(aData.end(), aid->begin(), aid->end());
        aData.insert(aData.end(), apk->begin(), apk->end());
        if (!core::crypto::ed25519Verify(apk->data(), aData.data(), aData.size(), asg->data())) {
            std::fprintf(stderr, "[phone] M6 host signature verification failed\n");
            return 1;
        }
        accId  = std::string(aid->begin(), aid->end());
        accPub = *apk;
        std::printf("[phone] M6 OK: host \"%s\", public key verified\n", accId.c_str());
    }

    // ---- 4) POST /pair-verify ----
    std::printf("[phone] -- pair-verify --\n");
    std::vector<uint8_t> shared;
    {
        auto eph = core::crypto::x25519Generate();
        std::vector<core::crypto::Tlv> pv1 = {
            {T_STATE, {1}},
            {T_PUBLIC_KEY, std::vector<uint8_t>(eph.pub.begin(), eph.pub.end())}};
        if (!rtsp.request("POST", baseUri + "/pair-verify", kTlv,
                          core::crypto::tlv8Encode(pv1), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto items = core::crypto::tlv8Decode(resp.body.data(), resp.body.size());
        const auto* apub = core::crypto::tlv8Get(items, T_PUBLIC_KEY);
        const auto* enc  = core::crypto::tlv8Get(items, T_ENCRYPTED_DATA);
        if (resp.status != 200 || !apub || apub->size() != 32 || !enc) {
            std::fprintf(stderr, "[phone] PV-M2 error (status=%d)\n", resp.status);
            return 1;
        }
        shared = core::crypto::x25519Shared(eph.priv.data(), apub->data());
        if (shared.empty()) {
            std::fprintf(stderr, "[phone] could not compute the X25519 shared secret\n");
            return 1;
        }
        auto pvKey = hkdf(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
        std::vector<uint8_t> innerBytes;
        if (!core::crypto::chachaOpen(pvKey.data(), core::crypto::nonceLabel("PV-Msg02"),
                                      nullptr, 0, enc->data(), enc->size(), &innerBytes)) {
            std::fprintf(stderr, "[phone] PV-M2 decryption failed\n");
            return 1;
        }
        auto inner = core::crypto::tlv8Decode(innerBytes.data(), innerBytes.size());
        const auto* aid = core::crypto::tlv8Get(inner, T_IDENTIFIER);
        const auto* asg = core::crypto::tlv8Get(inner, T_SIGNATURE);
        if (!aid || !asg) {
            std::fprintf(stderr, "[phone] PV-M2 inner payload is missing fields\n");
            return 1;
        }
        // Signature covers: host ephemeral public key ‖ host pi ‖ our ephemeral public key
        std::vector<uint8_t> sigData(apub->begin(), apub->end());
        sigData.insert(sigData.end(), aid->begin(), aid->end());
        sigData.insert(sigData.end(), eph.pub.begin(), eph.pub.end());
        if (!core::crypto::ed25519Verify(accPub.data(), sigData.data(), sigData.size(),
                                         asg->data())) {
            std::fprintf(stderr, "[phone] PV-M2 host ephemeral signature verification failed\n");
            return 1;
        }

        // PV-M3 → PV-M4
        std::vector<uint8_t> sig2(eph.pub.begin(), eph.pub.end());
        sig2.insert(sig2.end(), kControllerId, kControllerId + std::strlen(kControllerId));
        sig2.insert(sig2.end(), apub->begin(), apub->end());
        auto cSig2 = core::crypto::ed25519Sign(cpriv, sig2.data(), sig2.size());
        std::vector<core::crypto::Tlv> inner2 = {
            {T_IDENTIFIER, sb(kControllerId)}, {T_SIGNATURE, cSig2}};
        const auto innerBytes2 = core::crypto::tlv8Encode(inner2);
        auto sealed2 = core::crypto::chachaSeal(pvKey.data(), core::crypto::nonceLabel("PV-Msg03"),
                                                nullptr, 0, innerBytes2.data(), innerBytes2.size());
        std::vector<core::crypto::Tlv> pv3 = {{T_STATE, {3}}, {T_ENCRYPTED_DATA, sealed2}};
        if (!rtsp.request("POST", baseUri + "/pair-verify", kTlv,
                          core::crypto::tlv8Encode(pv3), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto it4 = core::crypto::tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st4 = core::crypto::tlv8Get(it4, T_STATE);
        if (!st4 || st4->front() != 4) {
            std::fprintf(stderr, "[phone] PV-M4 is not STATE 4 -- the host does not recognize this controller "
                                 "(not in PairingStore?)\n");
            return 1;
        }
        std::printf("[phone] PV-M4 OK -- shared secret established, control channel switching to encryption\n");
    }

    // ---- Switch to the encrypted control channel ----
    // Server (core/airplay/session.cpp:329): readKey = "Control-Write-…" (decrypts inbound),
    // writeKey = "Control-Read-…" (encrypts outbound) -- **the naming is the reverse of "who reads/writes"**.
    // So on the phone side: decrypt inbound with "Control-Read-…" (what the host writes), encrypt outbound with "Control-Write-…".
    // This pair is easy to swap, and the symptom is "no response to any request after PV-M4".
    const auto ctrlIn  = hkdf(shared, "Control-Salt", "Control-Read-Encryption-Key");
    const auto ctrlOut = hkdf(shared, "Control-Salt", "Control-Write-Encryption-Key");
    rtsp.enableCipher(std::make_unique<core::airplay::ControlCipher>(
        ctrlIn.data(),    // decrypt inbound (what the host writes)
        ctrlOut.data())); // encrypt outbound (what the host reads)
    {
        // immediately send an encrypted request to verify the direction (a mistake hangs until timeout)
        Response probe;
        if (!rtsp.request("OPTIONS", baseUri + "/", "", {}, &probe, &err)) {
            std::fprintf(stderr, "[phone] encrypted channel probe failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("[phone] encrypted control channel works (OPTIONS → %d)\n", probe.status);
    }

    // ---- 5) SETUP in two batches ----
    // Read the audio material up front: if it can't be read, don't create an audio stream in SETUP (spares the host opening a useless UDP path)
    AacFrames aac;
    if (!opt.aac.empty()) {
        if (!readAdts(opt.aac, &aac)) {
            std::fprintf(stderr, "[phone] no ADTS frames found in %s (create one with ffmpeg -i x.wav -c:a aac -f adts x.aac)\n",
                         opt.aac.c_str());
            return 1;
        }
        if (aac.freqIndex != 3 || aac.channels != 2) {
            std::fprintf(stderr, "[phone] %s is %dHz/%dch but must be 48000Hz stereo "
                                 "(the host negotiates 0x800000; a mismatch turns into noise)\n",
                         opt.aac.c_str(),
                         aac.freqIndex == 3 ? 48000 : (aac.freqIndex == 4 ? 44100 : -1),
                         aac.channels);
            return 1;
        }
        std::printf("[phone] audio: %zu AAC frames (48kHz stereo)\n", aac.frames.size());
    }
    const bool wantAudio = !aac.frames.empty();

    uint16_t eventPort = 0, dataPort = 0, audioPort = 0;
    const uint64_t scid = 1234;
    // Use **another** scid for audio: keys are derived from the scid, so reusing it would give both streams the same key
    const uint64_t audioScid = 5678;
    {
        // First batch: parameter negotiation (no streams). A real iPhone sends deviceID/features etc.,
        // but the host only cares about keepAliveLowPower.
        auto b1 = core::bplist::dict({
            {"deviceID", core::bplist::str("AA:BB:CC:DD:EE:FF")},
            {"sessionID", core::bplist::u(1)},
            {"keepAliveLowPower", core::bplist::boolean(true)},
        });
        if (!rtsp.request("SETUP", baseUri + "/stream", "application/x-apple-binary-plist",
                          core::bplist::serialize(b1), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto root = core::bplist::parse(resp.body.data(), resp.body.size());
        eventPort = uint16_t(core::bplist::uintOf(core::bplist::dictGet(root, "eventPort")));
        std::printf("[phone] SETUP#1 → eventPort=%u timingPort=%llu\n", eventPort,
                    (unsigned long long)core::bplist::uintOf(
                        core::bplist::dictGet(root, "timingPort")));
        if (!eventPort) {
            std::fprintf(stderr, "[phone] host did not provide eventPort\n");
            return 1;
        }

        // Second batch: create the screen stream (type 110) + optional audio stream (type 100).
        // audioFormat 0x800000 = AAC-LC 48kHz stereo (kAac48 in core/airplay/session.cpp:56).
        // Only include it when --aac is given; without it, no stream is created -- and the host won't create one when audio_ is empty either.
        std::vector<core::bplist::Ptr> streamList = {
            core::bplist::dict({
                {"type", core::bplist::u(110)},
                {"streamConnectionID", core::bplist::u(scid)},
            }),
        };
        if (wantAudio) {
            streamList.push_back(core::bplist::dict({
                {"type", core::bplist::u(100)},
                {"audioFormat", core::bplist::u(0x800000)},
                {"streamConnectionID", core::bplist::u(audioScid)},
            }));
        }
        auto b2 = core::bplist::dict({{"streams", core::bplist::array(streamList)}});
        if (!rtsp.request("SETUP", baseUri + "/stream", "application/x-apple-binary-plist",
                          core::bplist::serialize(b2), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        auto root2 = core::bplist::parse(resp.body.data(), resp.body.size());
        auto streams = core::bplist::dictGet(root2, "streams");
        // Identify by **type**, not streamConnectionID: the host only returns streamConnectionID for the audio stream,
        // and the screen stream's response has no such key (see core/airplay/session.cpp:383-402).
        for (size_t i = 0;; ++i) {
            auto s = core::bplist::arrGet(streams, i);
            if (!s) break;
            const uint16_t dp   = uint16_t(core::bplist::uintOf(core::bplist::dictGet(s, "dataPort")));
            const uint64_t type = core::bplist::uintOf(core::bplist::dictGet(s, "type"));
            if (type == 110)                        dataPort  = dp;
            else if (type >= 100 && type <= 102)    audioPort = dp;
        }
        std::printf("[phone] SETUP#2 → screen stream dataPort=%u (scid=%llu)\n", dataPort,
                    (unsigned long long)scid);
        if (!dataPort) {
            std::fprintf(stderr, "[phone] host did not provide a screen-stream dataPort\n");
            return 1;
        }
        if (wantAudio) {
            std::printf("[phone] SETUP#2 → audio stream dataPort=%u (scid=%llu)\n", audioPort,
                        (unsigned long long)audioScid);
            if (!audioPort) {
                std::fprintf(stderr, "[phone] host did not provide an audio-stream dataPort (did the host use --no-audio?)\n");
                return 1;
            }
        }
    }

    // ---- 6) RECORD ----
    {
        auto b = core::bplist::dict({{"streams", core::bplist::array({})}});
        if (!rtsp.request("RECORD", baseUri + "/stream", "application/x-apple-binary-plist",
                          core::bplist::serialize(b), &resp, &err)) {
            std::fprintf(stderr, "[phone] %s\n", err.c_str());
            return 1;
        }
        std::printf("[phone] RECORD → %d (the host should switch to the video surface now)\n", resp.status);
    }

    // Deliberately idle: leave the host in the window where "the video surface is active but no new picture yet".
    // If a stale frame from the previous session gets drawn here, sdl_video_sink's tail-frame guard isn't working.
    if (opt.videoDelayMs > 0) {
        std::printf("[phone] idling %d ms before streaming (checking the video-surface gap)\n", opt.videoDelayMs);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.videoDelayMs));
    }

    // ---- 7) Connect to dataPort and stream ----
    std::atomic<bool> touchStop{false};
    std::atomic<int>  touchDowns{0};
    std::thread touchThread;
    if (!opt.noTouch && eventPort) {
        // The event channel needs the shared secret + resolution (the host uses it to turn normalized coords into pixels).
        // Use the resolution the host negotiated (--screen); report 1280x720 here -- it's only the base for printing conversions.
        touchThread = std::thread(touchReader, opt.host, eventPort, shared, 1280, 720,
                                 &touchStop, &touchDowns);
    }

    Link screen;
    if (!screen.connectTo(opt.host, dataPort, "screen stream", &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }

    auto streamKey = core::crypto::hkdfSha512(shared, "DataStream-Salt" + std::to_string(scid),
                                              "DataStream-Output-Encryption-Key", 32);

    // ---- Audio thread: RTP over UDP, runs **concurrently** with the screen stream ----
    // wire = [12B RTP header][ciphertext][16B tag][8B nonce tail], AAD = header[4..12] (timestamp+SSRC).
    // Strictly the mirror of parsing in core/airplay/audio_stream.cpp.
    std::atomic<bool>   audioStop{false};
    std::atomic<size_t> audioSent{0};
    std::thread audioThread;
    if (wantAudio) {
        audioThread = std::thread([&] {
            UdpSender udp;
            std::string aerr;
            if (!udp.openTo(opt.host, audioPort, &aerr)) {
                std::fprintf(stderr, "[phone] audio: %s\n", aerr.c_str());
                return;
            }
            const auto akey = core::crypto::hkdfSha512(
                shared, "DataStream-Salt" + std::to_string(audioScid),
                "DataStream-Output-Encryption-Key", 32);

            constexpr uint32_t kRate = 48000;
            constexpr uint32_t kFrameSamples = 1024;   // one AAC-LC frame is fixed at 1024 samples
            const auto     t0a = std::chrono::steady_clock::now();
            uint64_t       counter = 0;    // encrypted-packet counter (nonce)
            uint32_t       ts      = 0;    // RTP timestamp (advances by 48kHz sample count)
            uint16_t       seq     = 0;
            size_t         idx     = 0;
            size_t         n       = 0;

            while (!audioStop.load()) {
                const uint32_t wantUs =
                    uint32_t(uint64_t(n) * 1000000ULL * kFrameSamples / kRate);
                std::this_thread::sleep_until(t0a + std::chrono::microseconds(wantUs));

                const auto& f = aac.frames[idx];

                std::array<uint8_t, 12> rtp{};
                rtp[0] = 0x80;                                  // V=2
                rtp[1] = 0x60;                                  // PT=96
                rtp[2] = uint8_t(seq >> 8);
                rtp[3] = uint8_t(seq);
                rtp[4] = uint8_t(ts >> 24); rtp[5] = uint8_t(ts >> 16);
                rtp[6] = uint8_t(ts >> 8);  rtp[7] = uint8_t(ts);
                rtp[8] = 0xAB; rtp[9] = 0xCD; rtp[10] = 0xEF; rtp[11] = 0x01;

                const auto nonce = core::crypto::nonce64(counter);
                auto sealed = core::crypto::chachaSeal(akey.data(), nonce,
                                                       rtp.data() + 4, 8,
                                                       f.data(), f.size());
                std::vector<uint8_t> wire(rtp.begin(), rtp.end());
                wire.insert(wire.end(), sealed.begin(), sealed.end());
                wire.insert(wire.end(), nonce.begin() + 4, nonce.end());   // trailing 8B nonce

                if (!udp.send(wire.data(), wire.size())) break;

                ++counter; ++seq; ++n; ++idx;
                ts += kFrameSamples;
                audioSent.store(n);

                // Loop the material when it runs out, until the main thread stops it (video done) or --seconds elapses.
                // With short material (a few seconds) against a long video, this avoids silence midway.
                if (idx >= aac.frames.size()) idx = 0;
                if (audioStop.load()) break;
                if (opt.seconds > 0 &&
                    std::chrono::steady_clock::now() - t0a > std::chrono::seconds(opt.seconds))
                    break;
            }
        });
    }

    // Config frame: plaintext, opcode 1, body = "avcC" + codecData.
    // **Must come before any video frame**: core's configured_ is false until then and video frames would be dropped entirely
    // (the symptom is "everything looks fine but the screen is black").
    {
        std::vector<uint8_t> body = {'a', 'v', 'c', 'C'};
        body.insert(body.end(), codecData.begin(), codecData.end());
        auto header = makeHeader(uint32_t(body.size()), 1);
        std::vector<uint8_t> wire = header;
        wire.insert(wire.end(), body.begin(), body.end());
        if (!screen.writeAll(wire.data(), wire.size())) {
            std::fprintf(stderr, "[phone] failed to send the config frame\n");
            return 1;
        }
        std::printf("[phone] sent config frame (%zu bytes of codecData, plaintext)\n", codecData.size());
    }

    std::printf("[phone] starting stream: %d fps%s\n", opt.fps, opt.loop ? ", looping" : "");
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t counter = 0;      // nonce counter for encrypted frames: **incremented only for encrypted frames**
    size_t   sent = 0;
    size_t   idx = 0;
    bool     ok = true;

    while (ok) {
        const auto want = t0 + std::chrono::microseconds(
                                    int64_t(sent) * 1000000 / opt.fps);
        std::this_thread::sleep_until(want);

        const auto& f = frames[idx];
        auto header = makeHeader(uint32_t(f.avcc.size() + 16), 0);
        auto sealed = core::crypto::chachaSeal(streamKey.data(), core::crypto::nonce64(counter),
                                               header.data(), header.size(),
                                               f.avcc.data(), f.avcc.size());
        ++counter;

        std::vector<uint8_t> wire = header;
        wire.insert(wire.end(), sealed.begin(), sealed.end());
        if (!screen.writeAll(wire.data(), wire.size())) {
            std::fprintf(stderr, "[phone] streaming interrupted (frame %zu): host disconnected\n", sent);
            ok = false;
            break;
        }
        ++sent;

        if (opt.frames > 0 && sent >= size_t(opt.frames)) break;
        if (opt.seconds > 0 &&
            std::chrono::steady_clock::now() - t0 > std::chrono::seconds(opt.seconds)) break;

        if (++idx >= frames.size()) {
            if (!opt.loop) break;
            idx = 0;
            // On loop, stream the file again: the config frame needn't be resent (same name and content, the host dedups)
        }
        if (sent % 120 == 0) {
            std::printf("[phone] pushed %zu frames (%llu encrypted)\n", sent,
                        (unsigned long long)counter);
            std::fflush(stdout);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[phone] streaming finished: %zu frames / %lld ms (%.1f fps)\n", sent,
                (long long)elapsed,
                elapsed > 0 ? double(sent) * 1000.0 / double(elapsed) : 0.0);

    if (audioThread.joinable()) {
        audioStop.store(true);
        audioThread.join();
        std::printf("[phone] audio streaming finished: %zu AAC frames (~%.1f s)\n",
                    audioSent.load(), double(audioSent.load()) * 1024.0 / 48000.0);
    }

    // ---- Touch observation window: the host is still sending SDL input right now ----
    if (touchThread.joinable()) {
        std::printf("[phone] observing touch (%.0f s) -- click a few times in the host window\n", 8.0);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(8000));
        touchStop.store(true);
        touchThread.join();
        const int d = touchDowns.load();
        if (d > 0)
            std::printf("[phone] ✅ received %d press events -- host → HID → encryption → event channel all working\n", d);
        else
            std::printf("[phone] ⚠️ no press events received (not clicked, or the touch path is broken)\n");
    }

    if (cpriv) EVP_PKEY_free(cpriv);
    return sent > 0 ? 0 : 1;
}
