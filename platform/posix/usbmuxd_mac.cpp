// ============================================================================
// platform/posix/usbmuxd_mac.cpp -- wired CarPlay macOS transport (system usbmuxd)
// ============================================================================
// The **fundamental difference** from the Linux path: not a single line of USB code here.
//
//   - Control plane: the system already runs usbmuxd (/var/run/usbmuxd), and it already manages
//     the iPhone's USBMUX interface. We act as an ordinary client speaking usbmuxd's plist protocol
//     (16-byte header + XML plist) and ask it to open a tunnel to a phone-side port -- from there
//     lockdown / StartSession / TLS / the carkit service are exactly the same as on the Linux path.
//   - Media plane: as soon as the iPhone enters the CarPlay configuration, macOS's own AppleUSBNCMData
//     binds the NCM function as an enX interface (observed: two en interfaces appear at once, with AV on one of them). We only need to
//     find that interface in IORegistry by phone serial number and take its IPv6 link-local address.
//     No cdc_ncm, no configuration switching, no address pinning -- Apple does all of that itself.
//
// Why not DriverKit/dext: not needed. The system daemon + system drivers already do this layer,
// and writing another driver would only fight them for the device. Reference implementations: f-io/LIVI and Roylyl/MacPlay's
// iap2-wired/{usbmuxd,mac_network}.rs -- they do the same.
//
// Prerequisite: this Mac must have been paired with the iPhone before (clicked "Trust" in Finder). Pairing records are
// managed by the system daemon; we just borrow a session. If not paired, the code below gives a clear hint.
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

// usbmuxd packet format (libusbmuxd/usbmuxd-proto.h):
//   struct usbmuxd_header { uint32 length; uint32 version; uint32 message; uint32 tag; }
// All little-endian. The plist protocol uses version=1, message=8 (MESSAGE_PLIST); the payload is an XML plist.
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

/// The two serial spellings must match: ioreg uses "0000802000164D2C0102002E",
/// while usbmuxd uses "00008020-00164D2C0102002E". Keep only alphanumerics when comparing.
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
// Byte stream: a unix socket connected to usbmuxd (after a successful Connect it is a tunnel segment)
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
            if (pr == 0) return 0;  // timeout
            if (pr < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            const ssize_t r = ::recv(fd_, buf, n, 0);
            if (r == 0) return -1;  // peer closed
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
// Open the socket / send and receive usbmuxd packets
// ---------------------------------------------------------------------------
int openUsbmuxdSocket(std::string* err)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err != nullptr) *err = std::string("socket(AF_UNIX) failed: ") + std::strerror(errno);
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
            *err = std::string("cannot connect to ") + kSocketPath + ": " + std::strerror(errno) +
                   " (is the system usbmuxd running?)";
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

/// Read exactly n bytes. 0 = timeout/peer closed, -1 = error.
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
        if (err != nullptr) *err = "failed to write to usbmuxd";
        return false;
    }
    return true;
}

bool recvPacket(int fd, int timeoutMs, Packet* out, std::string* err)
{
    uint8_t hdr[16];
    const int r = readExact(fd, hdr, sizeof hdr, timeoutMs);
    if (r <= 0) {
        if (err != nullptr) *err = r == 0 ? "usbmuxd did not respond (timeout)" : "failed to read from usbmuxd";
        return false;
    }
    const uint32_t len = readLe32(hdr);
    if (len < 16 || len > 1u << 20) {
        if (err != nullptr) *err = "usbmuxd returned a packet with an absurd length: " + std::to_string(len);
        return false;
    }
    out->version = readLe32(hdr + 4);
    out->message = readLe32(hdr + 8);
    out->tag     = readLe32(hdr + 12);
    out->body.assign(len - 16, 0);
    if (!out->body.empty()) {
        const int rr = readExact(fd, out->body.data(), out->body.size(), timeoutMs);
        if (rr <= 0) {
            if (err != nullptr) *err = "usbmuxd packet header arrived but the body did not";
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

/// Send one plist request and receive one plist response. When usbmuxd reports an error (MessageType=Result and
/// Number != 0) returns false, with the error code in err.
bool plistRequest(int fd, const core::usb::Plist& req, core::usb::Plist* resp, int timeoutMs,
                  std::string* err)
{
    if (!sendPlistPacket(fd, 1, req, err)) return false;

    Packet pkt;
    if (!recvPacket(fd, timeoutMs, &pkt, err)) return false;
    if (pkt.message != kMsgPlist) {
        if (err != nullptr) *err = "usbmuxd returned a non-plist packet (message=" + std::to_string(pkt.message) + ")";
        return false;
    }

    std::string perr;
    core::usb::Plist parsed;
    if (!core::usb::Plist::parse(std::string(pkt.body.begin(), pkt.body.end()), &parsed, &perr)) {
        if (err != nullptr) *err = "cannot parse the plist returned by usbmuxd: " + perr;
        return false;
    }
    if (parsed.stringValue("MessageType") == "Result") {
        const int64_t num = parsed.intValue("Number", -1);
        if (num != 0) {
            if (err != nullptr) *err = "usbmuxd rejected the request (error code " + std::to_string(num) + ")";
            return false;
        }
    }
    if (resp != nullptr) *resp = std::move(parsed);
    return true;
}

struct DeviceEntry {
    uint32_t    deviceId = 0;
    std::string serial;  ///< usbmuxd spelling (with hyphens)
};

/// Ask the system usbmuxd for a device list and pick out the iPhone connected over USB.
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
        if (err != nullptr) *err = "the ListDevices response from usbmuxd has no DeviceList";
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
// IORegistry: the phone's NCM interface (enX) and USBMUX interface number
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
    std::string ncm;          ///< enX exposed by AppleUSBNCMData (AV uses this one)
    int         usbMuxNumber = 1;  ///< bInterfaceNumber of the phone-side USBMUX interface
};

/// Find this iPhone in IORegistry by USB serial number, then read the interface and interface number it exposes.
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

        // Identify first: the USB serial number in the block must match
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

        // Then find the interface: the BSD Name immediately following AppleUSBNCMData.
        // (The same interface has child nodes like IONetworkStack whose BSD Name is not the one we want,
        //  so take the **first** BSD Name only after seeing AppleUSBNCMData.)
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
                out.usbMuxNumber = ifaceNum;   // USBMUX: ff/fe/02
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

/// System usbmuxd mux implementation: each new connection starts a fresh unix socket.
class SystemUsbMuxd final : public core::usb::IMux {
public:
    bool begin(std::string* err) override
    {
        if (deviceId_ != 0) return true;  // already identified

        // Right after plugging in, the daemon may take a moment to see the device; retry for ~5 seconds.
        std::string lastErr;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::vector<DeviceEntry> phones;
            if (listUsbPhones(&phones, &lastErr)) {
                if (!phones.empty()) {
                    deviceId_ = phones.front().deviceId;
                    serial_   = phones.front().serial;
                    LOG_INFO("[%s] system usbmuxd: phone %s (DeviceID %u)", kTag, serial_.c_str(),
                             unsigned(deviceId_));
                    return true;
                }
                lastErr.clear();
            }
            ::usleep(500 * 1000);
        }

        if (err != nullptr) {
            *err = lastErr.empty()
                       ? "no USB-connected iPhone in the system usbmuxd: plug in the data cable, unlock the phone,"
                         " and tap 'Trust' once in Finder"
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
        // WARNING: PortNumber is in **network byte order** (libusbmuxd does plist_new_uint(htons(port))).
        // Passing host order connects to some arbitrary port that never works.
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
// Public API
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
        // The USBHostTransport group needs the phone-side USBMUX interface number; fall back to 1 if ioreg cannot read it
        info->usbMuxInterface = uint8_t(p.found ? p.usbMuxNumber : 1);
    }
    return mux;
}

bool prepareWiredNcmDarwin(const std::string& phoneSerial, WiredNcmLink* out, std::string* err)
{
    if (out == nullptr) return false;
    *out = WiredNcmLink{};

    DarwinPhoneIfaces p;
    for (int attempt = 0; attempt < 10; ++attempt) {  // the interface can appear a second or two after carkit
        p = scanPhoneIoreg(phoneSerial);
        if (p.found && !p.ncm.empty()) break;
        ::usleep(500 * 1000);
    }
    if (!p.found) {
        if (err != nullptr) {
            *err = "cannot find an iPhone with serial " + phoneSerial +
                   " in IORegistry (failed to read ioreg? phone not in the CarPlay configuration?)";
        }
        return false;
    }
    if (p.ncm.empty()) {
        if (err != nullptr) {
            *err = "this iPhone has no AppleUSBNCMData network interface: the USB cable is not a data cable, or the phone has not entered"
                   " the CarPlay configuration (unlock the phone once, then replug)";
        }
        return false;
    }

    out->ifname    = p.ncm;
    out->broughtUp = true;   // macOS brings the interface up itself

    uint8_t mac[6];
    if (networkInfo().interfaceMAC(out->ifname, mac)) out->mac = core::deviceIdFromMAC(mac);

    out->ipv6 = networkInfo().interfaceIPv6LinkLocal(out->ifname);
    if (out->ipv6.empty()) {
        if (err != nullptr) {
            *err = out->ifname + " has no IPv6 link-local address (just came up? wait a second or two and press W to retry)";
        }
        return false;
    }

    LOG_INFO("[%s] NCM interface %s: MAC %s, IPv6 %s (created by macOS itself, no need to pin)", kTag,
             out->ifname.c_str(), out->mac.c_str(), out->ipv6.front().c_str());
    return true;
}

}  // namespace allplay::posix
