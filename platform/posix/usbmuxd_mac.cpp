// ============================================================================
// platform/posix/usbmuxd_mac.cpp —— 有线 CarPlay 的 macOS 传输层（系统 usbmuxd）
// ============================================================================
// macOS 和 Linux 那条路的**根本区别**：这里一行 USB 代码都不写。
//
//   · 控制面：系统本来就有 usbmuxd（/var/run/usbmuxd）在跑，它已经把 iPhone 的
//     USBMUX 口管起来了。我们以普通客户端的身份跟它说 usbmuxd 的 plist 协议
//     （16 字节头 + XML plist），让它给我们开一条到手机端口的隧道 —— 之后
//     lockdown / StartSession / TLS / carkit 服务和 Linux 那条路完全一样。
//   · 媒体面：iPhone 一进 CarPlay 配置，macOS 自带的 AppleUSBNCMData 就会把 NCM
//     功能绑成 enX 网卡（实测一次会出现两块 en，AV 走其中一块）。我们只要在
//     IORegistry 里按手机序列号把这块网卡找出来，取它的 IPv6 链路本地地址即可。
//     不用 cdc_ncm、不用切配置、不用钉地址 —— 那些都是 Apple 自己做的。
//
// 为什么不是 DriverKit/dext：不需要。系统 daemon + 系统驱动已经把这层做完了，
// 我们再写一个驱动只会和它们抢设备。参考实现见 f-io/LIVI 与 Roylyl/MacPlay 的
// iap2-wired/{usbmuxd,mac_network}.rs —— 它们也是这么做的。
//
// 前提：这台 Mac 必须已经和 iPhone 配对过（Finder 里点过「信任」）。配对记录归
// 系统 daemon 管，我们只借壳开会话；没配过的话下面会给出明确的提示。
#include "posix/usb.h"

#include "common/log.h"
#include "usb/mux.h"
#include "usb/plist_xml.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace allplay::posix {

namespace {

constexpr const char* kTag        = "usbmuxd";
constexpr const char* kSocketPath = "/var/run/usbmuxd";

// usbmuxd 的包格式（libusbmuxd/usbmuxd-proto.h）：
//   struct usbmuxd_header { uint32 length; uint32 version; uint32 message; uint32 tag; }
// 全部小端。plist 协议 version=1、message=8（MESSAGE_PLIST），payload 是 XML plist。
constexpr uint32_t kProtoPlist = 1;
constexpr uint32_t kMsgPlist   = 8;
constexpr uint32_t kPlistLibVersion = 3;  // kLibUSBMuxVersion

uint32_t readLe32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

void writeLe32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

/// 序列号的两种写法要能对上：ioreg 里是 "0000802000164D2C0102002E"，
/// usbmuxd 里是 "00008020-00164D2C0102002E"。比较时只留字母数字。
std::string normalizeSerial(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= '0' && c <= '9') out.push_back(c);
        else if (c >= 'a' && c <= 'z') out.push_back(char(c - 'a' + 'A'));
        else if (c >= 'A' && c <= 'Z') out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 字节流：一条连到 usbmuxd 的 unix socket（Connect 成功后它就是一截隧道）
// ---------------------------------------------------------------------------
class UnixFdStream : public core::IByteStream {
public:
    explicit UnixFdStream(int fd) : fd_(fd) {}
    ~UnixFdStream() override { close(); }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        for (;;) {
            pollfd p{fd_, POLLIN, 0};
            const int pr = ::poll(&p, 1, timeoutMs);
            if (pr == 0) return 0;  // 超时
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            const ssize_t r = ::recv(fd_, buf, n, 0);
            if (r == 0) return -1;  // 对端关了
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            return int(r);
        }
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0) return false;
        size_t done = 0;
        while (done < n) {
            const ssize_t w = ::send(fd_, buf + done, n - done, 0);
            if (w < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            done += size_t(w);
        }
        return true;
    }

    void close() override
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// ---------------------------------------------------------------------------
// 打开 socket / 收发 usbmuxd 包
// ---------------------------------------------------------------------------
int openUsbmuxdSocket(std::string* err)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err != nullptr) *err = std::string("socket(AF_UNIX) 失败：") + std::strerror(errno);
        return -1;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", kSocketPath);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err != nullptr) {
            *err = std::string("连不上 ") + kSocketPath + "：" + std::strerror(errno) +
                   "（系统的 usbmuxd 没在跑？）";
        }
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeFd(int fd, const uint8_t* buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t w = ::send(fd, buf + done, n - done, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += size_t(w);
    }
    return true;
}

/// 读满 n 字节。0 = 超时/对端关，-1 = 出错。
int readExact(int fd, uint8_t* buf, size_t n, int timeoutMs)
{
    size_t got = 0;
    while (got < n) {
        pollfd p{fd, POLLIN, 0};
        const int pr = ::poll(&p, 1, timeoutMs);
        if (pr == 0) return 0;
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        const ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += size_t(r);
    }
    return 1;
}

struct Packet {
    uint32_t             version = 0;
    uint32_t             message = 0;
    uint32_t             tag = 0;
    std::vector<uint8_t> body;
};

bool sendPlistPacket(int fd, uint32_t tag, const core::usb::Plist& pl, std::string* err)
{
    const std::string xml = pl.toXml();
    std::vector<uint8_t> pkt(16 + xml.size());
    writeLe32(&pkt[0], uint32_t(pkt.size()));
    writeLe32(&pkt[4], kProtoPlist);
    writeLe32(&pkt[8], kMsgPlist);
    writeLe32(&pkt[12], tag);
    std::memcpy(&pkt[16], xml.data(), xml.size());
    if (!writeFd(fd, pkt.data(), pkt.size())) {
        if (err != nullptr) *err = "写 usbmuxd 失败";
        return false;
    }
    return true;
}

bool recvPacket(int fd, int timeoutMs, Packet* out, std::string* err)
{
    uint8_t hdr[16];
    const int r = readExact(fd, hdr, sizeof hdr, timeoutMs);
    if (r <= 0) {
        if (err != nullptr) *err = r == 0 ? "usbmuxd 没回包（超时）" : "读 usbmuxd 失败";
        return false;
    }
    const uint32_t len = readLe32(hdr);
    if (len < 16 || len > 1u << 20) {
        if (err != nullptr) *err = "usbmuxd 回了个长度离谱的包：" + std::to_string(len);
        return false;
    }
    out->version = readLe32(hdr + 4);
    out->message = readLe32(hdr + 8);
    out->tag     = readLe32(hdr + 12);
    out->body.assign(len - 16, 0);
    if (!out->body.empty()) {
        const int rr = readExact(fd, out->body.data(), out->body.size(), timeoutMs);
        if (rr <= 0) {
            if (err != nullptr) *err = "usbmuxd 包头到了、包体没到";
            return false;
        }
    }
    return true;
}

core::usb::Plist makeRequest(const char* type)
{
    core::usb::Plist p = core::usb::Plist::dict();
    p.set("MessageType", core::usb::Plist::string(type));
    p.set("ClientVersionString", core::usb::Plist::string("AllPlay 1.0"));
    p.set("ProgName", core::usb::Plist::string("allplay"));
    p.set("kLibUSBMuxVersion", core::usb::Plist::integer(kPlistLibVersion));
    return p;
}

/// 发一条 plist 请求、收一条 plist 响应。usbmuxd 报错时（MessageType=Result 且
/// Number != 0）返回 false，err 里带上错误码。
bool plistRequest(int fd, const core::usb::Plist& req, core::usb::Plist* resp, int timeoutMs,
                  std::string* err)
{
    if (!sendPlistPacket(fd, 1, req, err)) return false;

    Packet pkt;
    if (!recvPacket(fd, timeoutMs, &pkt, err)) return false;
    if (pkt.message != kMsgPlist) {
        if (err != nullptr) *err = "usbmuxd 回了非 plist 包（message=" + std::to_string(pkt.message) + "）";
        return false;
    }

    std::string perr;
    core::usb::Plist parsed;
    if (!core::usb::Plist::parse(std::string(pkt.body.begin(), pkt.body.end()), &parsed, &perr)) {
        if (err != nullptr) *err = "usbmuxd 回的 plist 解不开：" + perr;
        return false;
    }
    if (parsed.stringValue("MessageType") == "Result") {
        const int64_t num = parsed.intValue("Number", -1);
        if (num != 0) {
            if (err != nullptr) *err = "usbmuxd 拒绝（错误码 " + std::to_string(num) + "）";
            return false;
        }
    }
    if (resp != nullptr) *resp = std::move(parsed);
    return true;
}

struct DeviceEntry {
    uint32_t    deviceId = 0;
    std::string serial;  ///< usbmuxd 的写法（带横杠）
};

/// 问系统 usbmuxd 要一张设备表，挑出 USB 上连着的那台 iPhone。
bool listUsbPhones(std::vector<DeviceEntry>* out, std::string* err)
{
    const int fd = openUsbmuxdSocket(err);
    if (fd < 0) return false;

    core::usb::Plist resp;
    const bool ok = plistRequest(fd, makeRequest("ListDevices"), &resp, 5000, err);
    ::close(fd);
    if (!ok) return false;

    out->clear();
    const core::usb::Plist* list = resp.find("DeviceList");
    if (list == nullptr) {
        if (err != nullptr) *err = "usbmuxd 的 ListDevices 回包里没有 DeviceList";
        return false;
    }
    for (const core::usb::Plist& d : list->elements()) {
        const core::usb::Plist* props = d.find("Properties");
        if (props == nullptr) continue;
        if (props->stringValue("ConnectionType") != "USB") continue;

        DeviceEntry e;
        e.deviceId = uint32_t(d.intValue("DeviceID", props->intValue("DeviceID", 0)));
        e.serial   = props->stringValue("SerialNumber");
        if (e.serial.empty()) e.serial = props->stringValue("USBSerialNumber");
        if (e.deviceId == 0) continue;
        out->push_back(std::move(e));
    }
    return true;
}

// ---------------------------------------------------------------------------
// IORegistry：手机的 NCM 网卡（enX）与 USBMUX 接口号
// ---------------------------------------------------------------------------
bool quotedValue(const std::string& line, const char* key, std::string* out)
{
    const std::string pat = std::string("\"") + key + "\" = \"";
    const size_t      p   = line.find(pat);
    if (p == std::string::npos) return false;
    const size_t begin = p + pat.size();
    const size_t end   = line.find('"', begin);
    if (end == std::string::npos) return false;
    *out = line.substr(begin, end - begin);
    return true;
}

bool numericValue(const std::string& line, const char* key, int* out)
{
    std::string v;
    if (!quotedValue(line, key, &v)) return false;
    if (v.empty() || v[0] < '0' || v[0] > '9') return false;
    *out = std::atoi(v.c_str());
    return true;
}

std::string runIoreg()
{
    std::string out;
    std::FILE*  f = ::popen("ioreg -r -n iPhone -l -w 0 2>/dev/null", "r");
    if (f == nullptr) return out;
    char   buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::pclose(f);
    return out;
}

struct DarwinPhoneIfaces {
    bool        found = false;
    std::string ncm;          ///< AppleUSBNCMData 挂出来的 enX（AV 走这块）
    int         usbMuxNumber = 1;  ///< 手机侧 USBMUX 口的 bInterfaceNumber
};

/// 在 IORegistry 里按 USB 序列号找到这台 iPhone，再读它挂出来的网卡与接口号。
DarwinPhoneIfaces scanPhoneIoreg(const std::string& phoneSerial)
{
    DarwinPhoneIfaces out;
    const std::string want = normalizeSerial(phoneSerial);
    if (want.empty()) return out;

    const std::string text = runIoreg();
    if (text.empty()) return out;

    const std::string marker = "+-o iPhone@";
    size_t            pos    = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        size_t next = text.find(marker, pos + marker.size());
        if (next == std::string::npos) next = text.size();
        const std::string block = text.substr(pos, next - pos);
        pos = next;

        // 先认人：块里的 USB 序列号必须对得上
        bool serialOk = false;
        {
            const std::string key = "\"USB Serial Number\" = \"";
            const size_t      p   = block.find(key);
            if (p != std::string::npos) {
                const size_t b = p + key.size();
                const size_t e = block.find('"', b);
                if (e != std::string::npos &&
                    normalizeSerial(block.substr(b, e - b)) == want) {
                    serialOk = true;
                }
            }
        }
        if (!serialOk) continue;

        out.found = true;

        // 再认网卡：AppleUSBNCMData 下面紧跟的那个 BSD Name。
        // （同一块网卡下面还有 IONetworkStack 等子节点，它们的 BSD Name 不是我们要的，
        //   所以只在看到 AppleUSBNCMData 之后取**第一个** BSD Name。）
        bool        wantNcm   = false;
        bool        gotNcm    = false;
        int         ifaceNum  = -1;
        int         cls = -1, sub = -1, proto = -1;
        size_t      p = 0;
        while (p < block.size()) {
            size_t e = block.find('\n', p);
            if (e == std::string::npos) e = block.size();
            const std::string line = block.substr(p, e - p);
            p = e + 1;

            if (line.find("IOUSBHostInterface") != std::string::npos) {
                cls = sub = proto = -1;
                ifaceNum = -1;
            }
            numericValue(line, "bInterfaceNumber", &ifaceNum);
            numericValue(line, "bInterfaceClass", &cls);
            numericValue(line, "bInterfaceSubClass", &sub);
            numericValue(line, "bInterfaceProtocol", &proto);
            if (cls == 0xff && sub == 0xfe && proto == 0x02 && ifaceNum >= 0) {
                out.usbMuxNumber = ifaceNum;   // USBMUX：ff/fe/02
            }

            if (line.find("\"IOClass\" = \"AppleUSBNCMData") != std::string::npos) {
                wantNcm = true;
                continue;
            }
            if (wantNcm && !gotNcm) {
                std::string name;
                if (quotedValue(line, "BSD Name", &name) && !name.empty()) {
                    out.ncm = name;
                    gotNcm  = true;
                    wantNcm = false;
                }
            }
        }
        return out;
    }
    return out;
}

/// 系统 usbmuxd 的 mux 实现：每开一条连接就新起一个 unix socket。
class SystemUsbMuxd final : public core::usb::IMux {
public:
    bool begin(std::string* err) override
    {
        if (deviceId_ != 0) return true;  // 已经认过人了

        // 刚插上时 daemon 可能要一小会儿才看见设备；重试 ~5 秒。
        std::string lastErr;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::vector<DeviceEntry> phones;
            if (listUsbPhones(&phones, &lastErr)) {
                if (!phones.empty()) {
                    deviceId_ = phones.front().deviceId;
                    serial_   = phones.front().serial;
                    LOG_INFO("[%s] 系统 usbmuxd：手机 %s（DeviceID %u）", kTag, serial_.c_str(),
                             unsigned(deviceId_));
                    return true;
                }
                lastErr.clear();
            }
            ::usleep(500 * 1000);
        }

        if (err != nullptr) {
            *err = lastErr.empty()
                       ? "系统 usbmuxd 里没有 USB 连接的 iPhone：插上数据线、解锁手机，"
                         "并在 Finder 里点一次「信任」"
                       : lastErr;
        }
        return false;
    }

    std::unique_ptr<core::IByteStream> connectTcp(uint16_t port, int timeoutMs,
                                                  std::string* err) override
    {
        if (deviceId_ == 0) {
            if (!begin(err)) return nullptr;
        }
        std::string oerr;
        const int   fd = openUsbmuxdSocket(err != nullptr ? err : &oerr);
        if (fd < 0) return nullptr;

        core::usb::Plist req = makeRequest("Connect");
        req.set("DeviceID", core::usb::Plist::integer(deviceId_));
        // ⚠️ PortNumber 是**网络序**（libusbmuxd 就是 plist_new_uint(htons(port))）。
        // 写成主机序会连到某个莫名其妙的端口上，然后一直不通。
        req.set("PortNumber", core::usb::Plist::integer(int64_t(uint16_t(htons(port)))));

        core::usb::Plist resp;
        if (!plistRequest(fd, req, &resp, timeoutMs, err != nullptr ? err : &oerr)) {
            ::close(fd);
            return nullptr;
        }
        return std::make_unique<UnixFdStream>(fd);
    }

    const std::string& serial() const { return serial_; }

private:
    uint32_t    deviceId_ = 0;
    std::string serial_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 对外
// ---------------------------------------------------------------------------
std::unique_ptr<core::usb::IMux> openSystemUsbMuxd(WiredUsbInfo* info, std::string* err)
{
    auto mux = std::make_unique<SystemUsbMuxd>();
    if (!mux->begin(err)) return nullptr;

    if (info != nullptr) {
        const DarwinPhoneIfaces p = scanPhoneIoreg(mux->serial());
        *info            = WiredUsbInfo{};
        info->vendorId   = 0x05ac;
        info->productId  = 0;
        info->serial     = mux->serial();
        info->bus        = 0;
        info->address    = 0;
        info->configValue = -1;
        // USBHostTransport 那组要的是手机侧 USBMUX 口的接口号；ioreg 读不到就按 1
        info->usbMuxInterface = uint8_t(p.found ? p.usbMuxNumber : 1);
    }
    return mux;
}

bool prepareWiredNcmDarwin(const std::string& phoneSerial, WiredNcmLink* out, std::string* err)
{
    if (out == nullptr) return false;
    *out = WiredNcmLink{};

    DarwinPhoneIfaces p;
    for (int attempt = 0; attempt < 10; ++attempt) {  // 网卡可能比 carkit 晚一两秒
        p = scanPhoneIoreg(phoneSerial);
        if (p.found && !p.ncm.empty()) break;
        ::usleep(500 * 1000);
    }
    if (!p.found) {
        if (err != nullptr) {
            *err = "IORegistry 里找不到序列号 " + phoneSerial +
                   " 的 iPhone（读 ioreg 失败？手机没进 CarPlay 配置？）";
        }
        return false;
    }
    if (p.ncm.empty()) {
        if (err != nullptr) {
            *err = "这台 iPhone 没有 AppleUSBNCMData 网卡：USB 线不是数据线，或者手机还没进"
                   " CarPlay 配置（先在手机上解锁一次、然后重新插拔）";
        }
        return false;
    }

    out->ifname    = p.ncm;
    out->broughtUp = true;   // macOS 自己会把网卡拉 up

    uint8_t mac[6];
    if (networkInfo().interfaceMAC(out->ifname, mac)) out->mac = core::deviceIdFromMAC(mac);

    out->ipv6 = networkInfo().interfaceIPv6LinkLocal(out->ifname);
    if (out->ipv6.empty()) {
        if (err != nullptr) {
            *err = out->ifname + " 上没有 IPv6 链路本地地址（刚起来？等一两秒再按 W 重试）";
        }
        return false;
    }

    LOG_INFO("[%s] NCM 网卡 %s：MAC %s，IPv6 %s（macOS 自己建的，不用钉）", kTag,
             out->ifname.c_str(), out->mac.c_str(), out->ipv6.front().c_str());
    return true;
}

}  // namespace allplay::posix
