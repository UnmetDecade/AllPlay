// ============================================================================
// platform/windows/bt_windows.cpp -- Windows Bluetooth (Winsock AF_BTH / RFCOMM)
// ============================================================================
// Client side: connect to the iPhone's iAP2 SPP service (UUID 00000000-deca-fade-deca-deafdecacafe).
// Enumerate paired devices with WSALookupService* (NS_BTH); connect with an AF_BTH socket + SOCKADDR_BTH
// (port=0 + serviceClassId=UUID, letting the Bluetooth stack resolve the RFCOMM channel via SDP).
#include "windows/net_windows.h"

#include "common/log.h"
#include "port/bluetooth.h"
#include "port/stream.h"

#include <winsock2.h>
#include <ws2bth.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace allplay::windows {

namespace {

// MAC string "AA:BB:CC:DD:EE:FF" (or hyphenated) -> 6 bytes
bool parseMac(const std::string& s, uint8_t mac[6])
{
    uint8_t out[6];
    int n = 0;
    for (size_t i = 0; i < s.size() && n < 6; ++i) {
        char c = s[i];
        if (c == ':' || c == '-') continue;
        int hi = std::isdigit((unsigned char)c) ? c - '0' : (std::tolower((unsigned char)c) - 'a' + 10);
        if (hi < 0 || hi > 15) return false;
        if (i + 1 >= s.size()) return false;
        char c2 = s[++i];
        int lo = std::isdigit((unsigned char)c2) ? c2 - '0' : (std::tolower((unsigned char)c2) - 'a' + 10);
        if (lo < 0 || lo > 15) return false;
        out[n++] = uint8_t((hi << 4) | lo);
    }
    if (n != 6) return false;
    std::memcpy(mac, out, 6);
    return true;
}

std::string macToString(const uint8_t mac[6])
{
    char b[18];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

ULONGLONG macToBtAddr(const uint8_t mac[6])
{
    ULONGLONG a = 0;
    for (int i = 0; i < 6; ++i) a |= (ULONGLONG(mac[i]) << (8 * i));
    return a;
}

// 128-bit Bluetooth UUID "00000000-deca-fade-deca-deafdecacafe" -> Windows GUID
// (first 3 fields byte-reversed to little-endian, last 8 bytes copied as-is)
GUID uuidToGuid(const std::string& u)
{
    uint8_t b[16] = {0};
    int n = 0;
    for (size_t i = 0; i < u.size() && n < 16; ) {
        if (u[i] == '-') { ++i; continue; }
        if (i + 1 >= u.size()) break;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(u[i]), lo = nib(u[i + 1]);
        if (hi < 0 || lo < 0) break;
        b[n++] = uint8_t((hi << 4) | lo);
        i += 2;
    }
    GUID g{};
    g.Data1 = (uint32_t(b[3]) << 24) | (uint32_t(b[2]) << 16) | (uint32_t(b[1]) << 8) | b[0];
    g.Data2 = uint16_t((uint16_t(b[5]) << 8) | b[4]);
    g.Data3 = uint16_t((uint16_t(b[7]) << 8) | b[6]);
    std::memcpy(g.Data4, b + 8, 8);
    return g;
}

// RFCOMM stream (read timeout via select, same semantics as TCP in net_windows)
class BtStream : public core::IByteStream {
public:
    explicit BtStream(SOCKET s) : s_(s) {}
    ~BtStream() override { close(); }
    BtStream(const BtStream&) = delete;

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (s_ == INVALID_SOCKET) return -1;
        fd_set fds; FD_ZERO(&fds); FD_SET(s_, &fds);
        timeval tv{};
        timeval* ptv = nullptr;
        if (timeoutMs >= 0) { tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000; ptv = &tv; }
        int r = ::select(0, &fds, nullptr, nullptr, ptv);
        if (r == 0) return 0;
        if (r == SOCKET_ERROR) return -1;
        int got = ::recv(s_, reinterpret_cast<char*>(buf), int(n), 0);
        if (got > 0) return got;
        if (got == 0) return -1;   // peer closed
        return -1;
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (s_ == INVALID_SOCKET) return false;
        size_t off = 0;
        while (off < n) {
            int w = ::send(s_, reinterpret_cast<const char*>(buf + off), int(n - off), 0);
            if (w == SOCKET_ERROR || w == 0) return false;
            off += size_t(w);
        }
        return true;
    }

    void close() override
    {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

class WinBluetooth : public core::IBluetooth {
public:
    std::vector<core::BtDevice> pairedDevices(std::string* err) override
    {
        std::vector<core::BtDevice> out;
        WSAQUERYSETW q{};
        q.dwSize = sizeof q;
        q.dwNameSpace = NS_BTH;

        HANDLE h = nullptr;
        if (::WSALookupServiceBeginW(&q, LUP_CONTAINERS, &h) != 0) {
            if (err) *err = "Bluetooth enumeration failed: " + std::to_string(WSAGetLastError());
            return out;
        }

        char buf[8192];
        while (true) {
            DWORD len = sizeof buf;
            int r = ::WSALookupServiceNextW(h, LUP_RETURN_NAME | LUP_RETURN_ADDR,
                                            &len, reinterpret_cast<LPWSAQUERYSETW>(buf));
            if (r != 0) break;
            auto* qs = reinterpret_cast<LPWSAQUERYSETW>(buf);
            core::BtDevice d;
            if (qs->lpszServiceInstanceName) {
                char nb[256] = "";
                ::WideCharToMultiByte(CP_UTF8, 0, qs->lpszServiceInstanceName, -1, nb, sizeof nb, nullptr, nullptr);
                d.name = nb;
            }
            if (qs->lpcsaBuffer && qs->lpcsaBuffer->RemoteAddr.lpSockaddr &&
                qs->lpcsaBuffer->RemoteAddr.lpSockaddr->sa_family == AF_BTH) {
                auto* sa = reinterpret_cast<SOCKADDR_BTH*>(qs->lpcsaBuffer->RemoteAddr.lpSockaddr);
                uint8_t mac[6];
                for (int i = 0; i < 6; ++i) mac[i] = uint8_t((sa->btAddr >> (8 * i)) & 0xff);
                d.address = macToString(mac);
            }
            if (!d.address.empty()) out.push_back(std::move(d));
        }
        ::WSALookupServiceEnd(h);
        return out;
    }

    core::IByteStream* connectRFCOMM(const std::string& address,
                                     const std::string& uuid,
                                     std::string* err) override
    {
        uint8_t mac[6];
        if (!parseMac(address, mac)) {
            if (err) *err = "invalid Bluetooth address: " + address;
            return nullptr;
        }
        SOCKET s = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) {
            if (err) *err = "RFCOMM socket failed: " + std::to_string(WSAGetLastError());
            return nullptr;
        }
        SOCKADDR_BTH sa{};
        sa.addressFamily = AF_BTH;
        sa.btAddr = macToBtAddr(mac);
        sa.serviceClassId = uuidToGuid(uuid);
        sa.port = 0;   // let the system resolve the RFCOMM channel via SDP
        if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == SOCKET_ERROR) {
            int e = WSAGetLastError();
            ::closesocket(s);
            if (err) *err = "RFCOMM connection failed: " + std::to_string(e);
            return nullptr;
        }
        return new BtStream(s);
    }

    core::IByteStream* waitForPhoneConnection(int, std::string* err) override
    {
        // Server side (SDP registration, waiting for the phone to connect) is a later milestone; the client side is enough to trigger iAP2
        if (err) *err = "Windows server-side Bluetooth (SDP registration) is not implemented";
        return nullptr;
    }
};

}  // namespace

core::IBluetooth* makeBluetooth()
{
    static WinBluetooth s_bt;
    return &s_bt;
}

}  // namespace allplay::windows
