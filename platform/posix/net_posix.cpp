// ============================================================================
// platform/posix/net_posix.cpp -- TCP listener/connection + network interface info (shared by Linux/macOS)
// ============================================================================
// Implements core::ITcpListener / core::ITcpStream / core::INetworkInfo.
// Read timeouts are per-call via poll(); accept polls with poll(), and stop() closes the listening fd to make it exit.
#include "posix/posix.h"

#include "common/log.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>          // pclose exit codes (wired NCM bring-up goes through the shell)
#include <unistd.h>

#if defined(__APPLE__)
#  include <net/if_dl.h>       // sockaddr_dl / LLADDR
#else
#  include <linux/if_packet.h> // sockaddr_ll (AF_PACKET)
#  include <linux/sockios.h>   // SIOCGIFHWADDR
#  include <sys/ioctl.h>
#endif

#include <algorithm>           // std::swap
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__APPLE__)
#  include <dirent.h>          // /sys/class/net enumeration (wired CarPlay NCM interface)
#  include <cstdlib>
#endif

namespace allplay::posix {

namespace {

// macOS has no MSG_NOSIGNAL (writing to a closed socket raises SIGPIPE) -> use SO_NOSIGPIPE.
#if !defined(MSG_NOSIGNAL)
#  define MSG_NOSIGNAL 0
#endif

void setNoSigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

// ---------------------------------------------------------------------------
// TCP stream
// ---------------------------------------------------------------------------
class PosixTcpStream : public core::ITcpStream {
public:
    explicit PosixTcpStream(int fd) : fd_(fd) {}
    ~PosixTcpStream() override { close(); }
    PosixTcpStream(const PosixTcpStream&) = delete;

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r == 0) return 0;                       // timeout
        if (r < 0) {
            if (errno == EINTR) return read(buf, n, timeoutMs);
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        ssize_t got = ::recv(fd_, buf, n, 0);
        if (got > 0) return int(got);
        if (got == 0) return -1;                    // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0) return false;
        size_t off = 0;
        while (off < n) {
            ssize_t w = ::send(fd_, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) return false;
            off += size_t(w);
        }
        return true;
    }

    void close() override
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_;
};

// ---------------------------------------------------------------------------
// TCP listener
// ---------------------------------------------------------------------------
class PosixTcpListener : public core::ITcpListener {
public:
    ~PosixTcpListener() override { stop(); }

    bool start(const std::string& bindAddr, uint16_t port, std::string* err) override
    {
        // Dual-stack IPv6 listener: a single listener accepts both IPv4 and IPv6 (iPhone CarPlay typically uses IPv6)
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) {
            if (err) *err = "socket failed: " + std::string(std::strerror(errno));
            return false;
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int zero = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);   // dual stack: also accept IPv4

        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = in6addr_any;
        if (!bindAddr.empty()) {
            // The "address%interface" form has to be split here: **link-local (fe80::/10) binds
            // require a scope id**; without one bind() returns EINVAL outright -- not "cannot bind"
            // but "invalid argument", which easily misleads. inet_pton does not understand %zone, so split it ourselves.
            // The wired CarPlay path binds fe80::2 on the NCM interface and always takes this branch.
            std::string host = bindAddr;
            const size_t pct = host.find('%');
            if (pct != std::string::npos) {
                const std::string zone = host.substr(pct + 1);
                host.resize(pct);
                const unsigned idx = ::if_nametoindex(zone.c_str());
                if (idx == 0) {
                    ::close(fd);
                    if (err) *err = "unknown interface name in bind address: " + zone;
                    return false;
                }
                addr.sin6_scope_id = idx;
            } else if (host.rfind("fe80:", 0) == 0 || host.rfind("FE80:", 0) == 0) {
                ::close(fd);
                if (err) *err = "link-local address must include %<interface> (otherwise bind returns EINVAL): " + bindAddr;
                return false;
            }
            // Accept an IPv6 address or dotted IPv4 (written into the low 4 bytes of a v4-mapped address)
            if (::inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) != 1 &&
                ::inet_pton(AF_INET, host.c_str(), &addr.sin6_addr.s6_addr[12]) != 1) {
                ::close(fd);
                if (err) *err = "invalid bind address: " + bindAddr;
                return false;
            }
        }
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd, 16) != 0) {
            int e = errno;
            ::close(fd);
            if (err) *err = "bind/listen failed: " + std::string(std::strerror(e));
            return false;
        }

        fd_ = fd;
        if (port == 0) {
            struct sockaddr_in6 got{};
            socklen_t alen = sizeof got;
            if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    std::unique_ptr<core::ITcpStream> accept(int timeoutMs) override
    {
        if (fd_ < 0) return nullptr;
        struct pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r <= 0) return nullptr;
        int cfd = ::accept(fd_, nullptr, nullptr);
        if (cfd < 0) return nullptr;
        setNoSigpipe(cfd);
        return std::make_unique<PosixTcpStream>(cfd);
    }

    uint16_t port() const override { return port_; }

    void stop() override
    {
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    }

private:
    int      fd_   = -1;
    uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// UDP socket (audio RTP/RTCP)
// ---------------------------------------------------------------------------
class PosixUdpSocket : public core::IUdpSocket {
public:
    ~PosixUdpSocket() override { close(); }

    bool bind(uint16_t port, std::string* err) override
    {
        int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);   // dual-stack UDP
        if (fd < 0) {
            if (err) *err = "udp socket failed: " + std::string(std::strerror(errno));
            return false;
        }
        int zero = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
            int e = errno;
            ::close(fd);
            if (err) *err = "udp bind failed: " + std::string(std::strerror(e));
            return false;
        }
        fd_ = fd;
        if (port == 0) {
            struct sockaddr_in6 got{};
            socklen_t alen = sizeof got;
            if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    int recv(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        const int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r <= 0) return r;   // 0 = timeout; -1 = error
        peerLen_ = sizeof peer_;
        const ssize_t got = ::recvfrom(fd_, buf, n, 0,
                                       reinterpret_cast<struct sockaddr*>(&peer_), &peerLen_);
        return got < 0 ? -1 : int(got);
    }

    bool send(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0 || peerLen_ == 0) return false;
        return ::sendto(fd_, buf, n, 0,
                        reinterpret_cast<struct sockaddr*>(&peer_), peerLen_) == ssize_t(n);
    }

    uint16_t port() const override { return port_; }
    void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    int                 fd_   = -1;
    uint16_t            port_ = 0;
    struct sockaddr_storage peer_{};
    socklen_t           peerLen_ = 0;
};

// ---------------------------------------------------------------------------
// Network interface info
// ---------------------------------------------------------------------------
namespace {

struct IfInfo {
    std::string        name;
    unsigned int       flags = 0;
    struct sockaddr_in addr {};
};

std::string ipv4String(const struct sockaddr_in& sin)
{
    char buf[INET_ADDRSTRLEN] = "";
    if (!inet_ntop(AF_INET, &sin.sin_addr, buf, sizeof buf)) return {};
    return buf;
}

#if !defined(__APPLE__)
/// Read a small sysfs text file and strip trailing newlines (empty string on failure).
std::string readSmallFile(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return {};
    char         buf[256] = "";
    const size_t n        = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
#endif

// Enumerate all IPv4 interfaces. Desktop/Linux uses getifaddrs; Android 4.4 (API<24) lacks
// getifaddrs/freeifaddrs, so SIOCGIFCONF is used instead (fixed-size entries on an AF_INET socket).
bool enumerateIPv4(std::vector<IfInfo>* out)
{
    out->clear();
#if defined(__ANDROID__) && __ANDROID_API__ < 24
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct ifconf ifc {};
    ifc.ifc_len = 0;
    if (::ioctl(fd, SIOCGIFCONF, &ifc) < 0) { ::close(fd); return false; }
    if (ifc.ifc_len <= 0) { ::close(fd); return true; }
    std::vector<char> buf(size_t(ifc.ifc_len) + 2 * sizeof(struct ifreq));
    ifc.ifc_buf = buf.data();
    if (::ioctl(fd, SIOCGIFCONF, &ifc) < 0) { ::close(fd); return false; }

    for (char* p = buf.data(); p < buf.data() + ifc.ifc_len; p += sizeof(struct ifreq)) {
        auto* ifr = reinterpret_cast<struct ifreq*>(p);
        IfInfo info;
        info.name = ifr->ifr_name;
        info.addr = *reinterpret_cast<struct sockaddr_in*>(&ifr->ifr_addr);
        struct ifreq fr {};
        std::strncpy(fr.ifr_name, ifr->ifr_name, sizeof fr.ifr_name - 1);
        if (::ioctl(fd, SIOCGIFFLAGS, &fr) == 0) info.flags = unsigned(fr.ifr_flags);
        out->push_back(std::move(info));
    }
    ::close(fd);
    return true;
#else
    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) return false;
    for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        IfInfo info;
        info.name  = ifa->ifa_name;
        info.flags = ifa->ifa_flags;
        info.addr  = *reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        out->push_back(std::move(info));
    }
    freeifaddrs(head);
    return true;
#endif
}

}  // namespace

class PosixNetworkInfo : public core::INetworkInfo {
public:
    std::string pickInterfaceIPv4(std::string* ifnameOut) override
    {
        if (ifnameOut) ifnameOut->clear();
        std::vector<IfInfo> list;
        if (!enumerateIPv4(&list)) return {};
        for (auto& ifa : list) {
            if (!(ifa.flags & IFF_UP) || (ifa.flags & IFF_LOOPBACK)) continue;
            std::string ip = ipv4String(ifa.addr);
            if (ip.empty()) continue;
            if (ifnameOut) *ifnameOut = ifa.name;
            return ip;   // first one is enough (lo is excluded; for multiple NICs specify --ifname explicitly)
        }
        return {};
    }

    std::string interfaceIPv4(const std::string& ifname) override
    {
        std::vector<IfInfo> list;
        if (!enumerateIPv4(&list)) return {};
        for (auto& ifa : list) {
            if (ifname != ifa.name) continue;
            return ipv4String(ifa.addr);
        }
        return {};
    }

    bool interfaceMAC(const std::string& ifname, uint8_t mac[6]) override
    {
#if defined(__APPLE__)
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return false;
        bool ok = false;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
            if (ifname != ifa->ifa_name) continue;
            auto* dl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
            if (dl->sdl_alen == 6) {
                std::memcpy(mac, LLADDR(dl), 6);
                ok = true;
            }
            break;
        }
        freeifaddrs(head);
        return ok;
#else
        // With getifaddrs, try AF_PACKET first; Android 4.4 (API<24) has no getifaddrs, so go straight to SIOCGIFHWADDR.
#if !(defined(__ANDROID__) && __ANDROID_API__ < 24)
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) == 0) {
            bool ok = false;
            for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
                if (ifname != ifa->ifa_name) continue;
                auto* ll = reinterpret_cast<struct sockaddr_ll*>(ifa->ifa_addr);
                if (ll->sll_halen == 6) {
                    std::memcpy(mac, ll->sll_addr, 6);
                    ok = true;
                }
                break;
            }
            freeifaddrs(head);
            if (ok) return true;
        }
#endif
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        struct ifreq ifr {};
        std::snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname.c_str());
        bool ok = ::ioctl(fd, SIOCGIFHWADDR, &ifr) == 0;
        if (ok) std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
        ::close(fd);
        return ok;
#endif
    }

    std::string interfaceForUsbDevice(int bus, int address) override
    {
#if defined(__APPLE__)
        // On macOS there is no kernel cdc_ncm to query: the NCM interface is exposed by AppleUSBNCMData,
        // and the mapping exists only in IORegistry (looked up by serial number; see prepareWiredNcmDarwin
        // in usbmuxd_mac.cpp). No interface name can be produced here -- callers fall back to "explicitly specify the interface".
        (void)bus;
        (void)address;
        return {};
#else
        if (bus <= 0 || address <= 0) return {};
        DIR* d = ::opendir("/sys/class/net");
        if (d == nullptr) return {};

        const std::string wantBus  = std::to_string(bus);
        const std::string wantAddr = std::to_string(address);

        // One USB device may expose **several** network interfaces: after the iPhone enters the
        // CarPlay configuration (6), the kernel binds both ipheth (6.2, Apple-private, observed
        // permanently NO-CARRIER) and cdc_ncm (6.3, the one that actually carries IP). readdir
        // order is not guaranteed; hitting ipheth first would pin fe80::2 to an interface that never
        // has carrier -- SYNs from the phone to the 0x4301 address then arrive on the other interface,
        // the kernel finds no local address and drops them silently (symptom: RX/TX all zero bytes, listener never accepts).
        // So collect candidates first, then score: cdc_ncm > has carrier > anything.
        std::vector<std::string> cands;
        while (struct dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == ".." || name == "lo") continue;
            // /sys/class/net/<n>/device is a symlink to the USB **interface** directory
            // (e.g. .../usb1/1-3/1-3:6.3); its parent is the USB device itself,
            // where busnum/devnum live -- the same pair libusb reports as bus/address.
            const std::string usbDev = "/sys/class/net/" + name + "/device/..";
            if (readSmallFile(usbDev + "/busnum") != wantBus) continue;
            if (readSmallFile(usbDev + "/devnum") != wantAddr) continue;
            cands.push_back(name);
        }
        ::closedir(d);

        if (cands.size() <= 1) return cands.empty() ? std::string() : cands.front();

        auto driverOf = [](const std::string& n) {
            char buf[256] = "";
            const std::string link = "/sys/class/net/" + n + "/device/driver";
            const ssize_t k = ::readlink(link.c_str(), buf, sizeof buf - 1);
            if (k <= 0) return std::string();
            buf[k] = '\0';
            const std::string s(buf);
            return s.substr(s.find_last_of('/') == std::string::npos ? 0 : s.find_last_of('/') + 1);
        };
        std::string best;
        int         bestScore = -1;
        for (const std::string& n : cands) {
            int score = 0;
            if (driverOf(n) == "cdc_ncm") score += 2;
            if (readSmallFile("/sys/class/net/" + n + "/carrier") == "1") score += 1;
            if (score > bestScore) { bestScore = score; best = n; }
        }
        return best;
#endif
    }

    std::vector<std::string> interfaceIPv6LinkLocal(const std::string& ifname) override
    {
        std::vector<std::string> out;
#if defined(__ANDROID__)
        // The Android host does not support the wired (USB cdc_ncm) path (see the default implementation in core/port/network.h),
        // and getifaddrs requires API 24+ (not declared under minSdk 19) -- return empty and fall back to the interface default behavior.
        (void)ifname;
        return out;
#else
        if (ifname.empty()) return out;

        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return out;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET6) continue;
            if (ifname != ifa->ifa_name) continue;
            auto* sa = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
            if (!IN6_IS_ADDR_LINKLOCAL(&sa->sin6_addr)) continue;
            char buf[INET6_ADDRSTRLEN] = "";
            if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof buf) == nullptr) continue;
            out.emplace_back(buf);
        }
        freeifaddrs(head);
        return out;
#endif
    }
};

}  // namespace

core::INetworkInfo& networkInfo()
{
    static PosixNetworkInfo s_net;
    return s_net;
}

std::unique_ptr<core::ITcpListener> makeTcpListener()
{
    return std::make_unique<PosixTcpListener>();
}

std::unique_ptr<core::IUdpSocket> makeUdpSocket()
{
    return std::make_unique<PosixUdpSocket>();
}

// ---------------------------------------------------------------------------
// Wired CarPlay media plane (kernel cdc_ncm)
// ---------------------------------------------------------------------------

namespace {

/// Run a shell command and collect its output into out. Returns the exit code (-1 = could not start the process).
/// Why the shell instead of netlink: the jobs here (detach NetworkManager, bring up, pin an address with
/// nodad) are one-liners in ip/nmcli, while hand-written netlink would take hundreds of lines and be easier to get wrong. This is a
/// **boot-time bring-up path**, not a hot path, so one process spawn does not matter.
int runCmd(const std::string& cmd, std::string* out)
{
    const std::string full = cmd + " 2>&1";
    std::FILE*        f    = ::popen(full.c_str(), "r");
    if (f == nullptr) return -1;

    std::string  acc;
    char         buf[256];
    size_t       n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) acc.append(buf, n);
    const int status = ::pclose(f);
    if (out != nullptr) *out = acc;

    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::string trimNewlines(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

bool prepareWiredNcm(int usbBus, int usbAddress, const std::string& wantLinkLocal,
                     WiredNcmLink* out, std::string* err)
{
    if (out == nullptr) return false;
    *out = WiredNcmLink{};

    const std::string ifname = networkInfo().interfaceForUsbDevice(usbBus, usbAddress);
    if (ifname.empty()) {
        if (err != nullptr) {
            *err = "cannot find the network interface exposed by USB " + std::to_string(usbBus) + "-" + std::to_string(usbAddress) +
                   " (did the kernel cdc_ncm create it? did the device enter the CarPlay configuration?)";
        }
        return false;
    }
    out->ifname = ifname;

    uint8_t mac[6];
    if (networkInfo().interfaceMAC(ifname, mac)) out->mac = core::deviceIdFromMAC(mac);

    const std::string want = wantLinkLocal.empty() ? std::string("fe80::2") : wantLinkLocal;

    // 1. Detach NetworkManager first. It treats this cable as a "wired connection" and asks for
    //    DHCP (there is no DHCP server on a point-to-point link), stays stuck in connecting, and
    //    may wipe our pinned address at any time. Failing to detach is not fatal (some systems do not have NM); continue.
    std::string log;
    if (runCmd("nmcli device set '" + ifname + "' managed no", &log) != 0) {
        LOG_INFO("[wired-ncm] failed to unmanage via nmcli (NM not installed? harmless): %s", trimNewlines(log).c_str());
    } else {
        out->unmanaged = true;
    }

    // 2. Bring it up. A freshly created interface is DOWN by default, and addresses cannot be pinned on a DOWN link.
    if (runCmd("ip link set dev '" + ifname + "' up", &log) != 0) {
        if (err != nullptr) *err = "ip link set up failed: " + trimNewlines(log);
        return false;
    }
    out->broughtUp = true;

    // 3. Is there a usable link-local address on the link? If not, pin one ourselves.
    //    The criterion is "read it back", not "command exit code" -- a duplicate add reports EEXIST, which is not a failure.
    out->ipv6 = networkInfo().interfaceIPv6LinkLocal(ifname);
    bool hasWant = false;
    for (const std::string& a : out->ipv6)
        if (a == want) hasWant = true;

    if (!hasWant) {
        if (runCmd("ip -6 addr add " + want + "/64 dev '" + ifname + "' nodad", &log) != 0) {
            if (err != nullptr) {
                *err = "failed to pin " + want + "/64: " + trimNewlines(log);
            }
            return false;
        }
        out->pinned = true;
        out->ipv6   = networkInfo().interfaceIPv6LinkLocal(ifname);
    }

    // 4. Put the pinned address first -- the caller (0x4301) uses only the first one, so it must be the one we chose.
    for (size_t i = 0; i < out->ipv6.size(); ++i) {
        if (out->ipv6[i] == want) {
            std::swap(out->ipv6[0], out->ipv6[i]);
            break;
        }
    }

    if (out->ipv6.empty()) {
        if (err != nullptr) *err = ifname + " has no IPv6 link-local address (still missing after pinning?)";
        return false;
    }
    LOG_INFO("[wired-ncm] %s: MAC %s, IPv6 %s (%s), NM %s", ifname.c_str(), out->mac.c_str(),
             out->ipv6.front().c_str(), out->pinned ? "pinned just now" : "already present",
             out->unmanaged ? "unmanaged" : "untouched");
    return true;
}

core::PlatformServices makePlatformServices()
{
    core::PlatformServices s;
    s.network           = &networkInfo();
    s.makeTcpListener   = &makeTcpListener;
    s.makeMdnsAdvertiser = &makeMdnsAdvertiser;
    s.makeUdpSocket     = &makeUdpSocket;
    s.bluetooth         = nullptr;   // Bluetooth trigger is wired explicitly by the composition root (see makeBluetooth)
    return s;
}

}  // namespace allplay::posix
