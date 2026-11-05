// ============================================================================
// tools/fake_phone.cpp —— 假 iPhone：真 socket 跑完 RTSP + 加密屏幕流 + 触摸回读
// ============================================================================
// 这是"Linux 宿主真的能渲染出 CarPlay 画面"这句话的**唯一证据**。没有 iPhone 的时候，
// 剩下能证明的方式就是自己当手机：真 TCP、真 TLV8、真 SRP、真 ChaCha20-Poly1305，
// 一个环节都不省。宿主的解码链路由 tools/video_probe 单独负责，两者合起来覆盖全链路。
//
// 流程（与 core/airplay/session.cpp 的路由一一对应）：
//   1. OPTIONS                 —— 探活，验证 RTSP 端口 + keep-alive
//   2. GET  /info              —— 读 bplist，确认对面是本项目的设备（打印名字/features）
//      跳过 /auth-setup       —— 要 MFi 身份文件（identity/），本机开发故意不装；
//                                真实 iPhone 这一步是必须的，所以本工具**不能**替代真机验收
//   3. POST /pair-setup  M1→M6 —— SRP-6a（PIN 3939）+ 交换 controller 长期公钥
//   4. POST /pair-verify PV1→PV4 —— X25519 + 签名，拿到 shared secret
//      **PV-M4 的响应仍是明文**，之后所有控制通道字节都走 ControlCipher 帧
//   5. SETUP 两批              —— 第一批拿 eventPort，第二批拿屏幕流 dataPort
//   6. RECORD                  —— 宿主据此把启动页切成视频面
//   7. 连 dataPort 推流        —— 先发配置帧（opcode 1，明文），再发加密视频帧（opcode 0）
//   8. 连 eventPort 读触摸     —— 解密宿主发来的 hidSendReport，把触点解出来打印
//
// 第 8 步是触摸功能的端到端验收：宿主把 SDL 的鼠标/手指按 letterbox 矩形归一化后
// 编成 HID report，经事件通道加密发来。这里能还原出坐标，才说明整条触摸链路是通的。
//
// 造测试流（任意带 H.264 的容器都行；--aac 用 48kHz 立体声的 ADTS）：
//   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=60 -t 10 -c:v libx264
//     -preset veryfast -pix_fmt yuv420p -g 60 -bf 0 -bsf:v h264_mp4toannexb -f h264 out.h264
//   ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=10" -ac 2
//     -c:a aac -b:a 128k -f adts tone.aac
// （注意注释行别以反斜杠结尾：那在 C++ 里会把下一行也吞进注释。）
//
// 用法：
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

// 假手机的"身份"：像真机一样有个稳定的 controller 标识和一对长期 Ed25519 密钥。
// 固定住 UUID 是有意的 —— 宿主会把它存进 PairingStore，重跑时走的是"已配对"分支。
constexpr const char* kControllerId = "11111111-2222-3333-4444-555555555555";

// ---------------------------------------------------------------------------
// 裸 TCP（tools/ 不依赖 core 的 port 层，这里直接用 POSIX socket）
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
            *err = std::string("建 socket 失败：") + std::strerror(errno);
            return false;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
            *err = "host 不是合法的 IPv4 地址：" + host;
            closeFd();
            return false;
        }
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            *err = std::string(what) + " 连不上 " + host + ":" + std::to_string(port) +
                   " —— " + std::strerror(errno);
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

    /// 返回 >0 = 字节数；0 = 超时；<0 = 断开/出错
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
// 裸 UDP 发送（音频流走 RTP over UDP，不能像屏幕流那样开 TCP）
// ---------------------------------------------------------------------------
class UdpSender {
public:
    UdpSender() = default;   // 声明了拷贝构造就会抑制隐式默认构造，得补回来
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
            *err = "解析不了音频目的地址 " + host;
            return false;
        }
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ >= 0) {
            std::memcpy(&addr_, res->ai_addr, size_t(res->ai_addrlen));
            addrLen_ = socklen_t(res->ai_addrlen);
        }
        ::freeaddrinfo(res);
        if (fd_ < 0) { *err = std::string("建 UDP socket 失败：") + std::strerror(errno); return false; }
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
// ADTS 裸流（.aac）→ 裸 AAC 帧
// ---------------------------------------------------------------------------
// CarPlay 的 AAC 载荷是**无 ADTS 头的裸帧**，而手头现成的素材基本都是 ADTS，
// 所以在这里剥一层。顺带把采样率/声道读出来 —— 协商值必须和素材对得上，
// 对不上就是"能出声但全是噪声"，不如早点报错。
struct AacFrames {
    std::vector<std::vector<uint8_t>> frames;
    int freqIndex = -1;   ///< ADTS 采样率索引（3 = 48000，4 = 44100）
    int channels  = 0;
};

bool readFile(const std::string& path, std::vector<uint8_t>* out);   // 定义在下面

bool readAdts(const std::string& path, AacFrames* out)
{
    std::vector<uint8_t> buf;
    if (!readFile(path, &buf)) return false;

    size_t i = 0;
    bool   sawHeader = false;
    while (i + 7 <= buf.size()) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xF0) != 0xF0) { ++i; continue; }   // 找同步字
        const size_t hdr = (buf[i + 1] & 0x01) ? 7 : 9;   // protection_absent=0 时多 2 字节 CRC
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
// RTSP 客户端：请求行 + 头 + Content-Length 体；可切到 ControlCipher 加密
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

    /// pair-verify 之后调用：之后的收发都走加密帧。
    /// 注意入参顺序 —— ControlCipher(readKey=解密入站, writeKey=加密出站)，
    /// 而服务端的 read/write 命名与手机侧**刚好相反**，见 controlKeys() 的注释。
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
            *err = method + " " + uri + " 发送失败（对端可能已断开）";
            return false;
        }
        return readResponse(method + " " + uri, out, err);
    }

private:
    /// 把链路里的一段字节收进 plain_（加密时先解密）。false = 断开。
    bool pull(std::string* err)
    {
        uint8_t buf[4096];
        const int n = link_.readSome(buf, sizeof buf, 10000);
        if (n == 0) return true;                     // 超时：没有新字节，交给外层再判
        if (n < 0) { *err = "读响应时对端断开"; return false; }

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
        if (cipher_->failed()) { *err = "控制通道解密鉴权失败（密钥方向搞反了？）"; return false; }
        return true;
    }

    bool readResponse(const std::string& what, Response* out, std::string* err)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            // 找头结束（\r\n\r\n），再按 Content-Length 取体
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
                *err = what + " 等响应超时（宿主没回？）";
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
// 选项
// ---------------------------------------------------------------------------
struct Options {
    std::string host = "127.0.0.1";
    uint16_t    port = 7000;
    std::string h264;
    std::string aac;               ///< --aac：ADTS 裸流，建音频流并推（不验音频就不给）
    int         fps    = 30;
    bool        loop   = false;
    int         frames = 0;        // >0：推满 N 帧就收工
    std::string pin    = "3939";
    int         seconds = 0;       // >0：推流最多跑 N 秒
    int         videoDelayMs = 0;  // --video-delay：RECORD 之后、推第一帧之前干等多久
                                   //   用来复现"切到视频面但还没新画面"的空窗（查残留帧）
    bool        noTouch = false;
    bool        verbose = false;
    bool        help    = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "用法: %s --h264 FILE [选项]\n"
        "  --h264 PATH     裸 AnnexB H.264 基本流（推给宿主当屏幕流）\n"
        "  --host IP       宿主地址（默认 127.0.0.1）\n"
        "  --port N        宿主 RTSP 端口（默认 7000）\n"
        "  --fps N         推流帧率（默认 30）\n"
        "  --loop          放完从头再来\n"
        "  --frames N      推满 N 帧就收工（默认 0 = 整个文件过一遍）\n"
        "  --seconds N     推流最多跑 N 秒\n"
        "  --video-delay MS  RECORD 之后先干等 MS 毫秒再推第一帧（复现切视频面后的空窗）\n"
        "  --pin S         pair-setup 的 PIN（默认 3939）\n"
        "  --aac PATH       48kHz 立体声 ADTS 裸流；给了就建音频流并推（验音频链路）\n"
        "  --no-touch      不连事件通道（不验触摸）\n"
        "  -v, --verbose   打印每条 RTSP 收发\n"
        "  -h, --help      显示本帮助\n"
        "\n"
        "退出码：0 = 走完全流程并推了帧；非 0 = 某一步失败（看 stderr）。\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
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
            std::fprintf(stderr, "未知参数：%s（-h 看帮助）\n", a);
            return false;
        }
    }
    if (o->help) return true;
    if (o->h264.empty()) {
        std::fprintf(stderr, "必须给 --h264 FILE（-h 看帮助）\n");
        return false;
    }
    if (o->fps <= 0 || o->fps > 240) {
        std::fprintf(stderr, "--fps 超范围（1..240）\n");
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "打不开 %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fprintf(stderr, "%s 是空文件\n", path.c_str());
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = std::fread(out->data(), 1, size_t(n), f);
    std::fclose(f);
    return got == size_t(n);
}

// ---------------------------------------------------------------------------
// 屏幕流推帧
// ---------------------------------------------------------------------------
struct ScreenFrame {
    std::vector<uint8_t> avcc;   ///< 已转 AVCC（4 字节长度前缀），可直接加密
    bool                 idr = false;
};

/// 128 字节数据流头：bodyLen(LE32) + opcode + 其余填零。
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
// 触摸回读：连事件通道，解密宿主发来的 POST /command，还原触点
// ---------------------------------------------------------------------------
void touchReader(std::string host, uint16_t port, std::vector<uint8_t> shared,
                 int width, int height, std::atomic<bool>* stop,
                 std::atomic<int>* got)
{
    Link lk;
    std::string err;
    if (!lk.connectTo(host, port, "事件通道", &err)) {
        std::fprintf(stderr, "[phone] 事件通道：%s\n", err.c_str());
        return;
    }

    // 服务端 event_channel.cpp:112-114 把 writeKey_ 用在**出站**、readKey_ 用在**入站**；
    // 手机侧对调。这一对和屏幕流/控制通道都不是同一组 label，不能类推。
    auto evWrite = core::crypto::hkdfSha512(shared, "Events-Salt", "Events-Write-Encryption-Key", 32);
    auto evRead  = core::crypto::hkdfSha512(shared, "Events-Salt", "Events-Read-Encryption-Key", 32);
    core::airplay::ControlCipher cipher(evWrite.data(),   // 解密入站（宿主发出的命令）
                                        evRead.data());   // 加密出站（我方 ACK）

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

        // 从明文里切出完整请求（\r\n\r\n + Content-Length）
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

            // 是请求才解析（响应形如 "RTSP/1.0 200"，跳过）
            if (head.rfind("RTSP/", 0) == 0 || head.rfind("HTTP/", 0) == 0) continue;

            auto root = core::bplist::parse(body.data(), body.size());
            const std::string type =
                root ? core::bplist::strOf(core::bplist::dictGet(root, "type")) : "";
            if (type != "hidSendReport") {
                std::printf("[phone] 事件通道收到 type=%s\n", type.c_str());
                continue;
            }

            const auto rep = core::bplist::dictGet(root, "hidReport");
            if (!rep || rep->data.size() < 12) continue;

            // HID report：2 触点 × 6 字节（slot | down | x u16le | y u16le）
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
                              down ? "按下" : "抬起", x, y);
                desc += one;
            }
            std::printf("[phone] 触摸 ← %s（归一化 %.3f,%.3f 左右）\n",
                        desc.c_str(),
                        double(rep->data[2] | (rep->data[3] << 8)) / double(width),
                        double(rep->data[4] | (rep->data[5] << 8)) / double(height));
            got->fetch_add(downs > 0 ? 1 : 0);

            // 回 ACK（服务端会等它；不回也不致命，但真机会回）
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
        std::fprintf(stderr, "在 %s 里没找到访问单元 —— 不是 AnnexB 流？\n", opt.h264.c_str());
        return 1;
    }

    // 整个文件先转成 AVCC（推流方向就是要 AVCC；core 收到后会自己转回 AnnexB）
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
        std::fprintf(stderr, "%s 里没有可推的帧\n", opt.h264.c_str());
        return 1;
    }
    std::printf("[phone] %s：%zu 个访问单元，avcC %zu 字节，首帧%s\n",
                opt.h264.c_str(), frames.size(), codecData.size(),
                frames[0].idr ? "是关键帧" : "**不是关键帧**（宿主会一直等到下一个 IDR）");
    if (!frames[0].idr)
        std::printf("[phone] ⚠️ 首帧不是 IDR：宿主的闸门会丢弃它之后的非关键帧，"
                    "请用 -g 较小的参数重编（如 -g 30）\n");

    const std::string baseUri = "rtsp://" + opt.host + ":" + std::to_string(opt.port);

    // ---- 1) OPTIONS ----
    Link ctrl;
    if (!ctrl.connectTo(opt.host, opt.port, "RTSP 控制通道", &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }
    Rtsp rtsp(ctrl);
    Response resp;
    if (!rtsp.request("OPTIONS", baseUri + "/", "", {}, &resp, &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }
    std::printf("[phone] OPTIONS → %d %s（Public: %s）\n", resp.status, resp.reason.c_str(),
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
        std::printf("[phone] GET /info → %d，设备名 \"%s\"，%zu 字节 bplist\n",
                    resp.status, name.c_str(), resp.body.size());
    }

    // ---- 3) POST /pair-setup（SRP-6a，PIN 3939）----
    std::printf("[phone] —— pair-setup（PIN %s）——\n", opt.pin.c_str());
    const std::string kTlv = "application/pairing+tlv8";

    uint8_t cpub[32] = {};
    EVP_PKEY* cpriv = allplay::phone::genEd25519(cpub);
    if (!cpriv) {
        std::fprintf(stderr, "[phone] 生成 Ed25519 密钥失败\n");
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
            std::fprintf(stderr, "[phone] M2 异常：status=%d B=%p salt=%p\n",
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
            std::fprintf(stderr, "[phone] SRP 失败：M4 校验不过（PIN 错？）\n");
            return 1;
        }
        std::printf("[phone] SRP 通过（M4 校验 OK）\n");
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
            std::fprintf(stderr, "[phone] M6 解密失败（宿主没有 MFi 身份？identity/ 装了吗）\n");
            return 1;
        }
        auto accInner = core::crypto::tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
        const auto* aid = core::crypto::tlv8Get(accInner, T_IDENTIFIER);
        const auto* apk = core::crypto::tlv8Get(accInner, T_PUBLIC_KEY);
        const auto* asg = core::crypto::tlv8Get(accInner, T_SIGNATURE);
        if (!aid || !apk || !asg) {
            std::fprintf(stderr, "[phone] M6 内层缺字段\n");
            return 1;
        }
        // 验宿主的长期签名 —— 这一步是在验"对面真的是那个刚生成的身份"，不是走过场
        auto aSignKey = hkdf(pairSessionK, "Pair-Setup-Accessory-Sign-Salt",
                             "Pair-Setup-Accessory-Sign-Info");
        std::vector<uint8_t> aData = aSignKey;
        aData.insert(aData.end(), aid->begin(), aid->end());
        aData.insert(aData.end(), apk->begin(), apk->end());
        if (!core::crypto::ed25519Verify(apk->data(), aData.data(), aData.size(), asg->data())) {
            std::fprintf(stderr, "[phone] M6 宿主签名验不过\n");
            return 1;
        }
        accId  = std::string(aid->begin(), aid->end());
        accPub = *apk;
        std::printf("[phone] M6 OK：宿主 \"%s\"，公钥已验签\n", accId.c_str());
    }

    // ---- 4) POST /pair-verify ----
    std::printf("[phone] —— pair-verify ——\n");
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
            std::fprintf(stderr, "[phone] PV-M2 异常（status=%d）\n", resp.status);
            return 1;
        }
        shared = core::crypto::x25519Shared(eph.priv.data(), apub->data());
        if (shared.empty()) {
            std::fprintf(stderr, "[phone] X25519 共享密钥算不出来\n");
            return 1;
        }
        auto pvKey = hkdf(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
        std::vector<uint8_t> innerBytes;
        if (!core::crypto::chachaOpen(pvKey.data(), core::crypto::nonceLabel("PV-Msg02"),
                                      nullptr, 0, enc->data(), enc->size(), &innerBytes)) {
            std::fprintf(stderr, "[phone] PV-M2 解密失败\n");
            return 1;
        }
        auto inner = core::crypto::tlv8Decode(innerBytes.data(), innerBytes.size());
        const auto* aid = core::crypto::tlv8Get(inner, T_IDENTIFIER);
        const auto* asg = core::crypto::tlv8Get(inner, T_SIGNATURE);
        if (!aid || !asg) {
            std::fprintf(stderr, "[phone] PV-M2 内层缺字段\n");
            return 1;
        }
        // 签名覆盖：宿主临时公钥 ‖ 宿主 pi ‖ 我方临时公钥
        std::vector<uint8_t> sigData(apub->begin(), apub->end());
        sigData.insert(sigData.end(), aid->begin(), aid->end());
        sigData.insert(sigData.end(), eph.pub.begin(), eph.pub.end());
        if (!core::crypto::ed25519Verify(accPub.data(), sigData.data(), sigData.size(),
                                         asg->data())) {
            std::fprintf(stderr, "[phone] PV-M2 宿主临时签名验不过\n");
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
            std::fprintf(stderr, "[phone] PV-M4 不是 STATE 4 —— 宿主不认这个 controller"
                                 "（PairingStore 里没有？）\n");
            return 1;
        }
        std::printf("[phone] PV-M4 OK —— 共享密钥已建立，控制通道转入加密\n");
    }

    // ---- 切换到加密控制通道 ----
    // 服务端（core/airplay/session.cpp:329）：readKey = "Control-Write-…"（解密入站）、
    // writeKey = "Control-Read-…"（加密出站）——**命名与"谁读谁写"是反的**。
    // 所以手机侧：解入站用 "Control-Read-…"（宿主写出来的），加出站用 "Control-Write-…"。
    // 这一对很容易搞反，且搞反的症状是"PV-M4 之后所有请求都没响应"。
    const auto ctrlIn  = hkdf(shared, "Control-Salt", "Control-Read-Encryption-Key");
    const auto ctrlOut = hkdf(shared, "Control-Salt", "Control-Write-Encryption-Key");
    rtsp.enableCipher(std::make_unique<core::airplay::ControlCipher>(
        ctrlIn.data(),    // 解密入站（宿主写出来的）
        ctrlOut.data())); // 加密出站（宿主读的）
    {
        // 立刻发一条加密请求验证方向对不对（错了会卡到超时）
        Response probe;
        if (!rtsp.request("OPTIONS", baseUri + "/", "", {}, &probe, &err)) {
            std::fprintf(stderr, "[phone] 加密通道试探失败：%s\n", err.c_str());
            return 1;
        }
        std::printf("[phone] 加密控制通道可用（OPTIONS → %d）\n", probe.status);
    }

    // ---- 5) SETUP 两批 ----
    // 音频素材先读好：读不出来就别在 SETUP 里建音频流了（免得宿主白开一路 UDP）
    AacFrames aac;
    if (!opt.aac.empty()) {
        if (!readAdts(opt.aac, &aac)) {
            std::fprintf(stderr, "[phone] %s 里没找到 ADTS 帧（用 ffmpeg -i x.wav -c:a aac -f adts x.aac 造）\n",
                         opt.aac.c_str());
            return 1;
        }
        if (aac.freqIndex != 3 || aac.channels != 2) {
            std::fprintf(stderr, "[phone] %s 是 %dHz/%dch，得是 48000Hz 立体声"
                                 "（宿主按 0x800000 协商，对不上会变噪声）\n",
                         opt.aac.c_str(),
                         aac.freqIndex == 3 ? 48000 : (aac.freqIndex == 4 ? 44100 : -1),
                         aac.channels);
            return 1;
        }
        std::printf("[phone] 音频：%zu 个 AAC 帧（48kHz 立体声）\n", aac.frames.size());
    }
    const bool wantAudio = !aac.frames.empty();

    uint16_t eventPort = 0, dataPort = 0, audioPort = 0;
    const uint64_t scid = 1234;
    // 音频用**另一个** scid：密钥是从 scid 派生的，复用就等于两路流同一把钥匙
    const uint64_t audioScid = 5678;
    {
        // 第一批：参数协商（无 streams）。真实 iPhone 会带 deviceID/features 等，
        // 宿主只关心 keepAliveLowPower。
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
            std::fprintf(stderr, "[phone] 宿主没给 eventPort\n");
            return 1;
        }

        // 第二批：建屏幕流（type 110）+ 可选音频流（type 100）。
        // audioFormat 0x800000 = AAC-LC 48kHz 立体声（core/airplay/session.cpp:56 的
        // kAac48）。给了 --aac 才带，不带就不建 —— 宿主那边 audio_ 为空也不会建。
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
        // 按 **type** 认，别按 streamConnectionID：宿主只给音频流回 streamConnectionID，
        // 屏幕流的响应里没有这个键（见 core/airplay/session.cpp:383-402）。
        for (size_t i = 0;; ++i) {
            auto s = core::bplist::arrGet(streams, i);
            if (!s) break;
            const uint16_t dp   = uint16_t(core::bplist::uintOf(core::bplist::dictGet(s, "dataPort")));
            const uint64_t type = core::bplist::uintOf(core::bplist::dictGet(s, "type"));
            if (type == 110)                        dataPort  = dp;
            else if (type >= 100 && type <= 102)    audioPort = dp;
        }
        std::printf("[phone] SETUP#2 → 屏幕流 dataPort=%u（scid=%llu）\n", dataPort,
                    (unsigned long long)scid);
        if (!dataPort) {
            std::fprintf(stderr, "[phone] 宿主没给屏幕流 dataPort\n");
            return 1;
        }
        if (wantAudio) {
            std::printf("[phone] SETUP#2 → 音频流 dataPort=%u（scid=%llu）\n", audioPort,
                        (unsigned long long)audioScid);
            if (!audioPort) {
                std::fprintf(stderr, "[phone] 宿主没给音频流 dataPort（宿主是不是 --no-audio？）\n");
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
        std::printf("[phone] RECORD → %d（宿主此刻应切到视频面）\n", resp.status);
    }

    // 故意空等：让宿主停在"已切视频面、还没有任何新画面"的窗口里。
    // 这时如果有上一场会话的残留帧被画出来，就是 sdl_video_sink 的尾巴帧保护没生效。
    if (opt.videoDelayMs > 0) {
        std::printf("[phone] 推流前空等 %d ms（查视频面空窗）\n", opt.videoDelayMs);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.videoDelayMs));
    }

    // ---- 7) 连 dataPort 推流 ----
    std::atomic<bool> touchStop{false};
    std::atomic<int>  touchDowns{0};
    std::thread touchThread;
    if (!opt.noTouch && eventPort) {
        // 事件通道要共享密钥 + 分辨率（宿主用它把归一化坐标换成像素）。
        // 分辨率用宿主协商的那套（--screen），这里按 1280x720 报 —— 只是打印时的换算基准。
        touchThread = std::thread(touchReader, opt.host, eventPort, shared, 1280, 720,
                                 &touchStop, &touchDowns);
    }

    Link screen;
    if (!screen.connectTo(opt.host, dataPort, "屏幕流", &err)) {
        std::fprintf(stderr, "[phone] %s\n", err.c_str());
        return 1;
    }

    auto streamKey = core::crypto::hkdfSha512(shared, "DataStream-Salt" + std::to_string(scid),
                                              "DataStream-Output-Encryption-Key", 32);

    // ---- 音频线程：RTP over UDP，与屏幕流**同时**跑 ----
    // wire = [12B RTP 头][密文][16B tag][8B nonce 尾]，AAD = 头[4..12]（timestamp+SSRC）。
    // 与 core/airplay/audio_stream.cpp 的解析严格对侧。
    std::atomic<bool>   audioStop{false};
    std::atomic<size_t> audioSent{0};
    std::thread audioThread;
    if (wantAudio) {
        audioThread = std::thread([&] {
            UdpSender udp;
            std::string aerr;
            if (!udp.openTo(opt.host, audioPort, &aerr)) {
                std::fprintf(stderr, "[phone] 音频：%s\n", aerr.c_str());
                return;
            }
            const auto akey = core::crypto::hkdfSha512(
                shared, "DataStream-Salt" + std::to_string(audioScid),
                "DataStream-Output-Encryption-Key", 32);

            constexpr uint32_t kRate = 48000;
            constexpr uint32_t kFrameSamples = 1024;   // AAC-LC 一帧固定 1024 样本
            const auto     t0a = std::chrono::steady_clock::now();
            uint64_t       counter = 0;    // 加密包计数器（nonce）
            uint32_t       ts      = 0;    // RTP 时间戳（按 48kHz 采样数走）
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
                wire.insert(wire.end(), nonce.begin() + 4, nonce.end());   // 尾部 8B nonce

                if (!udp.send(wire.data(), wire.size())) break;

                ++counter; ++seq; ++n; ++idx;
                ts += kFrameSamples;
                audioSent.store(n);

                // 素材放完就从头来，一直放到主线程喊停（视频推完了）或 --seconds 到点。
                // 短素材（几秒）配长视频时这样才不会中途没声。
                if (idx >= aac.frames.size()) idx = 0;
                if (audioStop.load()) break;
                if (opt.seconds > 0 &&
                    std::chrono::steady_clock::now() - t0a > std::chrono::seconds(opt.seconds))
                    break;
            }
        });
    }

    // 配置帧：明文，opcode 1，body = "avcC" + codecData。
    // **必须在任何视频帧之前**：core 的 configured_ 在此之前为假，视频帧会被整段丢掉
    // （症状就是"一切正常但黑屏"）。
    {
        std::vector<uint8_t> body = {'a', 'v', 'c', 'C'};
        body.insert(body.end(), codecData.begin(), codecData.end());
        auto header = makeHeader(uint32_t(body.size()), 1);
        std::vector<uint8_t> wire = header;
        wire.insert(wire.end(), body.begin(), body.end());
        if (!screen.writeAll(wire.data(), wire.size())) {
            std::fprintf(stderr, "[phone] 发配置帧失败\n");
            return 1;
        }
        std::printf("[phone] 已发配置帧（%zu 字节 codecData，明文）\n", codecData.size());
    }

    std::printf("[phone] 开始推流：%d fps%s\n", opt.fps, opt.loop ? "，循环" : "");
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t counter = 0;      // 加密帧的 nonce 计数器：**只对加密帧递增**
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
            std::fprintf(stderr, "[phone] 推流中断（第 %zu 帧）：宿主断开了\n", sent);
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
            // 循环时重新推一遍：配置帧不必重发（同名同内容，宿主会去重）
        }
        if (sent % 120 == 0) {
            std::printf("[phone] 已推 %zu 帧（%llu 加密帧）\n", sent,
                        (unsigned long long)counter);
            std::fflush(stdout);
        }
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[phone] 推流结束：%zu 帧 / %lld ms（%.1f fps）\n", sent,
                (long long)elapsed,
                elapsed > 0 ? double(sent) * 1000.0 / double(elapsed) : 0.0);

    if (audioThread.joinable()) {
        audioStop.store(true);
        audioThread.join();
        std::printf("[phone] 音频推流结束：%zu 个 AAC 帧（约 %.1f 秒）\n",
                    audioSent.load(), double(audioSent.load()) * 1024.0 / 48000.0);
    }

    // ---- 触摸观察窗口：宿主此刻还在把 SDL 的输入发过来 ----
    if (touchThread.joinable()) {
        std::printf("[phone] 触摸观察中（%.0f 秒）—— 去宿主的窗口里点几下\n", 8.0);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(8000));
        touchStop.store(true);
        touchThread.join();
        const int d = touchDowns.load();
        if (d > 0)
            std::printf("[phone] ✅ 收到 %d 次按下事件 —— 宿主 → HID → 加密 → 事件通道 全通\n", d);
        else
            std::printf("[phone] ⚠️ 没收到按下事件（没点，或者触摸链路断了）\n");
    }

    if (cpriv) EVP_PKEY_free(cpriv);
    return sent > 0 ? 0 : 1;
}
