// ============================================================================
// platform/posix/bt_bluez.cpp -- Linux transport (BlueZ / libbluetooth)
// ============================================================================
// Direction (same as the macOS version): **we (the head unit) actively connect to the phone's iAP2 SPP service**.
// The iPhone never initiates, so the main protocol path is connectRFCOMM (core/bt/bt_trigger.cpp:92).
//
// On the target machine (Ubuntu 22.04, bluetoothd 5.64, iPhone 16 paired and connected) this file
// confirmed three things on real hardware -- the entire basis for choosing "client mode"; do not change it from memory:
//
//   1. SDP can find the iPhone's iAP2 record. In `sdptool browse 68:44:65:AA:AE:4B`:
//        Service Name: Wireless iAP      UUID 128: 00000000-deca-fade-deca-deafdecacafe
//        Protocol Descriptor List: L2CAP / RFCOMM   Channel: 1
//      (The same machine's "Wireless iAP v2" (02030302-...) is also on **the same channel 1**.)
//   2. A raw RFCOMM socket to channel 1 **connects**:
//        socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM) + connect(addr, 1) -> success
//   3. After connecting, **the phone does not send first** -- the iAP2 start frame 0xFF 0x5A comes from us,
//      matching the direction of core/iap2/wireless_client.cpp.
//
// Hence: pairedDevices goes through BlueZ D-Bus (org.bluez), and connectRFCOMM uses
// SDP channel lookup + a raw RFCOMM socket. **Server mode is left as an honest stub** -- wireless CarPlay's
// trigger direction is us connecting to the phone, and this project has no "wait for the phone to connect" scenario (see bt_trigger.h).
//
// WARNING: getting this working on real hardware still needs an **MFi identity** (identity/identity.pk8 + certificate.p7b).
//    The Bluetooth link and iAP2 identification (up to the phone sending 0xAA00 to request the certificate) do not need it and can be tested;
//    beyond that the iPhone replies 0xAA04 due to the empty certificate. The credentials come from the Apple MFi program and cannot be solved in code.
#include "posix/posix.h"

#include "common/log.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <dbus/dbus.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace allplay::posix {

namespace {

constexpr const char* kTag = "bt";

/// Timeout for a single RFCOMM connection. The phone-side iAP2 SPP service opens asynchronously after the Bluetooth ACL connects,
/// so too short a timeout fails to connect -- but the SDP query already established an L2CAP link once, effectively warming up the link.
/// Callers should still retry on their own (the Android side does 10 x 3s; see around jni_bridge.cpp:417).
constexpr int kConnectTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// "00000000-deca-fade-deca-deafdecacafe" -> 16 bytes (same order as the uuid string, network order)
bool uuidToRaw16(const std::string& uuid, uint8_t out[16])
{
    int bi = 0;
    for (size_t i = 0; i < uuid.size() && bi < 16;) {
        if (uuid[i] == '-') { ++i; continue; }
        if (i + 1 >= uuid.size()) return false;
        out[bi++] = uint8_t(std::strtoul(uuid.substr(i, 2).c_str(), nullptr, 16));
        i += 2;
    }
    return bi == 16;
}

/// System D-Bus connection (process-wide shared; dbus_bus_get returns a singleton, **do not unref**)
DBusConnection* systemBus()
{
    static DBusConnection* c = []() -> DBusConnection* {
        DBusError e;
        dbus_error_init(&e);
        DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
        if (!conn) {
            LOG_WARN("[%s] cannot connect to the system D-Bus: %s", kTag,
                     dbus_error_is_set(&e) ? e.message : "unknown");
            dbus_error_free(&e);
            return nullptr;
        }
        // Do not take the whole process down when the bus disconnects -- we only fetch a device list
        dbus_connection_set_exit_on_disconnect(conn, false);
        return conn;
    }();
    return c;
}

// ---------------------------------------------------------------------------
// RFCOMM byte stream: wrap an fd as core::IByteStream
// ---------------------------------------------------------------------------
// Same semantics as the macOS version (MacRfcommStream): **object lifetime = process**, callers do not delete
// (core/bt/bt_trigger.cpp holds the stream from the moment it gets it and never releases it).
//
// **The fd is owned by the reader thread**: close() only shuts down to wake the reader, and the reader really closes it on exit.
// This avoids the classic race of "the main thread closed the fd while the reader still uses the same fd number" (the number may already be reused).
class BlueZRfcommStream : public core::IByteStream {
public:
    explicit BlueZRfcommStream(int fd) : fd_(fd)
    {
        reader_ = std::thread([this] { readerLoop(); });
        reader_.detach();   // process-level object, no join needed
    }

    ~BlueZRfcommStream() override
    {
        // The normal path never gets here (the object is not released). If it does, the reader must be stopped first,
        // otherwise it is still using this -- so only shut down here, do not delete anything.
        close();
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cvData_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return !queue_.empty() || closed_; }))
            return 0;                      // timeout
        if (queue_.empty()) return -1;     // closed and nothing buffered
        const size_t take = std::min(n, queue_.size());
        for (size_t i = 0; i < take; ++i) { buf[i] = queue_.front(); queue_.pop_front(); }
        return int(take);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        int fd;
        {
            std::lock_guard<std::mutex> lk(fdMu_);
            fd = fd_;
        }
        if (fd < 0) return false;
        size_t off = 0;
        while (off < n) {
            const size_t chunk = std::min<size_t>(n - off, 4096);
            // MSG_NOSIGNAL: return EPIPE when the peer disconnects instead of killing the process (we install no SIGPIPE handler)
            const ssize_t w = ::send(fd, buf + off, chunk, MSG_NOSIGNAL);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                return false;
            }
            off += size_t(w);
        }
        return true;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lk(fdMu_);
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);   // wake the blocked read; **do not close** (the reader thread owns it)
    }

private:
    void readerLoop()
    {
        std::vector<uint8_t> buf(4096);
        for (;;) {
            const ssize_t n = ::read(fd_, buf.data(), buf.size());
            if (n > 0) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    queue_.insert(queue_.end(), buf.begin(), buf.begin() + n);
                }
                cvData_.notify_all();
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;   // 0 = peer closed normally; <0 = error (including shutdown waking the read)
        }
        {
            std::lock_guard<std::mutex> lk(fdMu_);
            if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cvData_.notify_all();
        LOG_INFO("[%s] RFCOMM stream ended", kTag);
    }

    int                 fd_;
    std::mutex          fdMu_;          // protects fd_ itself
    std::thread         reader_;

    std::mutex              mu_;        // protects the group below
    std::condition_variable cvData_;
    std::deque<uint8_t>     queue_;
    bool                    closed_ = false;
};

// ---------------------------------------------------------------------------
// SDP: look up the RFCOMM channel number by UUID
// ---------------------------------------------------------------------------
/// Returns the channel number 1..30 if found; -1 with err written if not.
/// On success sdp_close tears down that L2CAP link -- but the Bluetooth ACL link is already up, so connecting RFCOMM right after is more reliable.
int findRfcommChannel(const std::string& address, const uint8_t uuidRaw[16], std::string* err)
{
    bdaddr_t dst;
    if (str2ba(address.c_str(), &dst) != 0) {
        if (err) *err = "malformed Bluetooth address: " + address;
        return -1;
    }
    // Leave the local side empty = let the kernel pick the default controller.
    // Do not use BDADDR_ANY: it is a compound literal like `(&(bdaddr_t){{0,...}})`,
    // and taking its address in C++ is "taking address of rvalue".
    bdaddr_t src{};

    sdp_session_t* sess = sdp_connect(&src, &dst, SDP_RETRY_IF_BUSY);
    if (!sess) {
        if (err) *err = "SDP connection failed (phone not listening / not paired / Bluetooth off)";
        return -1;
    }

    uuid_t svc;
    sdp_uuid128_create(&svc, uuidRaw);
    sdp_list_t* search = sdp_list_append(nullptr, &svc);
    uint32_t    range  = 0x0000ffff;                       // all attributes
    sdp_list_t* attrs  = sdp_list_append(nullptr, &range);

    sdp_list_t* rsp     = nullptr;
    int         channel = -1;
    if (sdp_service_search_attr_req(sess, search, SDP_ATTR_REQ_RANGE, attrs, &rsp) != 0) {
        if (err) *err = "SDP service search request failed";
    } else {
        for (sdp_list_t* r = rsp; r && channel < 0; r = r->next) {
            auto* rec = static_cast<sdp_record_t*>(r->data);
            sdp_list_t* protos = nullptr;
            if (sdp_get_access_protos(rec, &protos) != 0 || !protos) continue;

            // WARNING: pass **protos itself**, not `protos->data`.
            //    `protos->data` is the inner protocol descriptor list; passing it makes sdp_get_proto_port
            //    dereference a wild pointer and SIGSEGV on the spot -- this was hit on real hardware (top of the gdb stack).
            //    Verified on iPhone against 4 different records: iAP2->1, Handsfree->8,
            //    phonebook->13, MAP->2, matching `sdptool browse` line by line.
            const int ch = sdp_get_proto_port(protos, RFCOMM_UUID);
            if (ch > 0) channel = ch;

            // Free only the outer list: the inner list leaks once, which is fine (this runs once per connection)
            sdp_list_free(protos, nullptr);
        }
        if (channel < 0 && err)
            *err = "SDP has a record for this UUID but no RFCOMM channel (not an SPP service?)";
    }

    if (rsp) sdp_list_free(rsp, nullptr);   // record bodies likewise leak once, see above
    sdp_list_free(search, nullptr);
    sdp_list_free(attrs, nullptr);
    sdp_close(sess);
    return channel;
}

// ---------------------------------------------------------------------------
// Raw RFCOMM connection (with timeout)
// ---------------------------------------------------------------------------
int openRfcomm(const std::string& address, uint8_t channel, std::string* err)
{
    bdaddr_t dst;
    if (str2ba(address.c_str(), &dst) != 0) {
        if (err) *err = "malformed Bluetooth address: " + address;
        return -1;
    }

    const int fd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0) {
        if (err) *err = std::string("socket(AF_BLUETOOTH) failed: ") + std::strerror(errno) +
                        " (no Bluetooth stack in the kernel?)";
        return -1;
    }

    // A blocking connect hangs for a long time when the phone is not listening (Bluetooth retransmits slowly) -> non-blocking + own poll timeout
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    sockaddr_rc sa{};
    sa.rc_family  = AF_BLUETOOTH;
    sa.rc_bdaddr  = dst;
    sa.rc_channel = channel;

    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) {
        if (err) *err = std::string("RFCOMM connection failed: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (r < 0) {
        pollfd p{};
        p.fd     = fd;
        p.events = POLLOUT;
        const int pr = ::poll(&p, 1, kConnectTimeoutMs);
        if (pr == 0) {
            if (err) *err = "RFCOMM connection timed out (" + std::to_string(kConnectTimeoutMs) +
                            "ms; the SPP service may not be up right after pairing, try again)";
            ::close(fd);
            return -1;
        }
        if (pr < 0) {
            if (err) *err = std::string("poll failed: ") + std::strerror(errno);
            ::close(fd);
            return -1;
        }
        int soErr = 0;
        socklen_t sl = sizeof(soErr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl);
        if (soErr != 0) {
            if (err) *err = std::string("RFCOMM connection refused: ") + std::strerror(soErr) +
                            " (wrong channel number? phone not listening?)";
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, fl);   // restore blocking: a blocking read in the reader thread is simplest
    return fd;
}

// ---------------------------------------------------------------------------
// BlueZ D-Bus: paired devices
// ---------------------------------------------------------------------------
/// Use org.freedesktop.DBus.ObjectManager.GetManagedObjects, filtering org.bluez.Device1 with Paired set.
/// Note that the target machine may have **hundreds of paired devices** (the user's own headphones/car/watch), so names must be filtered.
std::vector<core::BtDevice> bluezPairedDevices(std::string* err)
{
    std::vector<core::BtDevice> out;
    DBusConnection* c = systemBus();
    if (!c) {
        if (err) *err = "cannot connect to the system D-Bus (bluetoothd not running?)";
        return out;
    }

    DBusMessage* m = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!m) {
        if (err) *err = "failed to build D-Bus request (out of memory?)";
        return out;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 8000, &e);
    dbus_message_unref(m);
    if (!r) {
        if (err) *err = std::string("failed to request device list from org.bluez: ") +
                        (dbus_error_is_set(&e) ? e.message : "unknown");
        dbus_error_free(&e);
        return out;
    }

    DBusMessageIter it, arr;
    if (dbus_message_iter_init(r, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter de, ifaces;
            dbus_message_iter_recurse(&arr, &de);
            dbus_message_iter_next(&de);            // skip the object path
            dbus_message_iter_recurse(&de, &ifaces);

            while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ide, props;
                dbus_message_iter_recurse(&ifaces, &ide);
                const char* ifname = nullptr;
                dbus_message_iter_get_basic(&ide, &ifname);
                dbus_message_iter_next(&ide);

                if (ifname && !std::strcmp(ifname, "org.bluez.Device1")) {
                    core::BtDevice d;
                    std::string    alias, name;
                    bool           paired = false;
                    dbus_message_iter_recurse(&ide, &props);
                    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter pde, var;
                        dbus_message_iter_recurse(&props, &pde);
                        const char* key = nullptr;
                        dbus_message_iter_get_basic(&pde, &key);
                        dbus_message_iter_next(&pde);
                        dbus_message_iter_recurse(&pde, &var);
                        const int t = dbus_message_iter_get_arg_type(&var);
                        if (key && t == DBUS_TYPE_STRING) {
                            const char* v = nullptr;
                            dbus_message_iter_get_basic(&var, &v);
                            if (v) {
                                if (!std::strcmp(key, "Address"))      d.address = v;
                                else if (!std::strcmp(key, "Alias"))   alias = v;
                                else if (!std::strcmp(key, "Name"))    name  = v;
                            }
                        } else if (key && t == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b = 0;
                            dbus_message_iter_get_basic(&var, &b);
                            if (!std::strcmp(key, "Paired"))         paired     = (b != 0);
                            else if (!std::strcmp(key, "Connected")) d.connected = (b != 0);
                        }
                        dbus_message_iter_next(&props);
                    }
                    // Alias is the name the user sees in bluetoothctl ("iPhone 16"), prefer it
                    d.name = !alias.empty() ? alias : name;
                    if (paired && !d.address.empty()) out.push_back(std::move(d));
                }
                dbus_message_iter_next(&ifaces);
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(r);

    // Connected devices first -- that is almost certainly the one the user wants
    std::stable_sort(out.begin(), out.end(),
                     [](const core::BtDevice& a, const core::BtDevice& b) {
                         return a.connected && !b.connected;
                     });
    if (out.empty() && err)
        *err = "no paired devices in BlueZ (pair the phone in system settings first)";
    return out;
}

// ---------------------------------------------------------------------------
// BlueZ D-Bus: adapter name (the name shown in the phone's Bluetooth list)
// ---------------------------------------------------------------------------
/// Read org.bluez.Adapter1's Alias (a user-changed name is stored here);
/// when unchanged, Alias equals Name (the hostname by default). Use GetManagedObjects rather than
/// hardcoding /org/bluez/hci0 -- the hci number is not guaranteed to be 0 on machines with multiple adapters.
/// Do not use hci_read_local_name: that path needs CAP_NET_ADMIN and always fails for normal users.
std::string bluezAdapterName()
{
    DBusConnection* c = systemBus();
    if (!c) return "";

    DBusMessage* m = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!m) return "";
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 3000, &e);
    dbus_message_unref(m);
    if (!r) {
        dbus_error_free(&e);
        return "";
    }

    std::string name;
    DBusMessageIter it, arr;
    if (dbus_message_iter_init(r, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &arr);
        while (name.empty() && dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter de, ifaces;
            dbus_message_iter_recurse(&arr, &de);
            dbus_message_iter_next(&de);              // skip the object path
            dbus_message_iter_recurse(&de, &ifaces);

            while (name.empty() && dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ide, props;
                dbus_message_iter_recurse(&ifaces, &ide);
                const char* ifname = nullptr;
                dbus_message_iter_get_basic(&ide, &ifname);
                dbus_message_iter_next(&ide);

                if (ifname && !std::strcmp(ifname, "org.bluez.Adapter1")) {
                    std::string alias, plain;
                    dbus_message_iter_recurse(&ide, &props);
                    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter pde, var;
                        dbus_message_iter_recurse(&props, &pde);
                        const char* key = nullptr;
                        dbus_message_iter_get_basic(&pde, &key);
                        dbus_message_iter_next(&pde);
                        dbus_message_iter_recurse(&pde, &var);
                        if (key && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                            const char* v = nullptr;
                            dbus_message_iter_get_basic(&var, &v);
                            if (v) {
                                if (!std::strcmp(key, "Alias"))      alias = v;
                                else if (!std::strcmp(key, "Name"))  plain = v;
                            }
                        }
                        dbus_message_iter_next(&props);
                    }
                    name = !alias.empty() ? alias : plain;
                }
                dbus_message_iter_next(&ifaces);
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(r);
    return name;
}

// ---------------------------------------------------------------------------
// NetworkManager D-Bus: current Wi-Fi SSID
// ---------------------------------------------------------------------------
/// iAP2's 0x5703 tells the phone the "head unit's Wi-Fi". This takes the currently connected SSID.
/// Returns an empty string if unavailable (core_selftest has --bt-ssid to pass it explicitly).
struct DbusVal {
    int                  type = DBUS_TYPE_INVALID;
    std::string          str;
    std::vector<uint8_t> bytes;
    uint32_t             u32 = 0;
};

bool nmGetProp(DBusConnection* c, const std::string& path, const char* iface,
               const char* prop, DbusVal* out)
{
    DBusMessage* m = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager", path.c_str(),
        "org.freedesktop.DBus.Properties", "Get");
    if (!m) return false;
    DBusMessageIter it;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);

    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 3000, &e);
    dbus_message_unref(m);
    if (!r) { dbus_error_free(&e); return false; }

    bool ok = false;
    DBusMessageIter ri, var;
    if (dbus_message_iter_init(r, &ri) &&
        dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&ri, &var);
        out->type = dbus_message_iter_get_arg_type(&var);
        switch (out->type) {
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH: {
            const char* s = nullptr;
            dbus_message_iter_get_basic(&var, &s);
            out->str = s ? s : "";
            ok = true;
            break;
        }
        case DBUS_TYPE_UINT32: {
            dbus_uint32_t v = 0;
            dbus_message_iter_get_basic(&var, &v);
            out->u32 = v;
            ok = true;
            break;
        }
        case DBUS_TYPE_ARRAY: {
            DBusMessageIter a;
            dbus_message_iter_recurse(&var, &a);
            while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_BYTE) {
                uint8_t b = 0;
                dbus_message_iter_get_basic(&a, &b);
                out->bytes.push_back(b);
                dbus_message_iter_next(&a);
            }
            ok = !out->bytes.empty();
            break;
        }
        default:
            break;
        }
    }
    dbus_message_unref(r);
    return ok;
}

/// NM_DEVICE_TYPE_WIFI (a NetworkManager enum value, not ours to change)
constexpr uint32_t kNmDeviceTypeWifi = 2;

std::string nmCurrentSsid()
{
    DBusConnection* c = systemBus();
    if (!c) return "";

    DBusMessage* m = dbus_message_new_method_call(
        "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
        "org.freedesktop.NetworkManager", "GetDevices");
    if (!m) return "";
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 3000, &e);
    dbus_message_unref(m);
    if (!r) { dbus_error_free(&e); return ""; }

    std::vector<std::string> devices;
    DBusMessageIter it, arr;
    if (dbus_message_iter_init(r, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
            const char* p = nullptr;
            dbus_message_iter_get_basic(&arr, &p);
            if (p) devices.push_back(p);
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(r);

    for (const std::string& dev : devices) {
        DbusVal type;
        if (!nmGetProp(c, dev, "org.freedesktop.NetworkManager.Device", "DeviceType", &type) ||
            type.type != DBUS_TYPE_UINT32 || type.u32 != kNmDeviceTypeWifi)
            continue;

        DbusVal ap;
        if (!nmGetProp(c, dev, "org.freedesktop.NetworkManager.Device.Wireless",
                       "ActiveAccessPoint", &ap) || ap.str.empty() ||
            ap.str == "/")   // NM reports "/" when not connected to any AP
            continue;

        DbusVal ssid;
        if (nmGetProp(c, ap.str, "org.freedesktop.NetworkManager.AccessPoint", "Ssid", &ssid))
            return std::string(ssid.bytes.begin(), ssid.bytes.end());
    }
    return "";
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
class LinuxBluetooth : public core::IBluetooth {
public:
    std::vector<core::BtDevice> pairedDevices(std::string* err) override
    {
        return bluezPairedDevices(err);
    }

    core::IByteStream* connectRFCOMM(const std::string& address, const std::string& uuid,
                                     std::string* err) override
    {
        uint8_t raw[16] = {0};
        if (!uuidToRaw16(uuid, raw)) {
            if (err) *err = "malformed UUID (expected 8-4-4-4-12): " + uuid;
            return nullptr;
        }

        // Look up the channel via SDP first. If not found, **do not give up outright** -- the iOS iAP2 service
        // being on channel 1 is well known (the tests at the top of this file confirm it), and SDP itself often comes back empty right after pairing.
        std::string sdpErr;
        int channel = findRfcommChannel(address, raw, &sdpErr);
        if (channel < 0) {
            LOG_WARN("[%s] SDP channel lookup failed (%s), falling back to channel 1", kTag, sdpErr.c_str());
            channel = 1;
        } else {
            LOG_INFO("[%s] SDP: %s -> RFCOMM channel %d", kTag, uuid.c_str(), channel);
        }

        const int fd = openRfcomm(address, uint8_t(channel), err);
        if (fd < 0) return nullptr;

        LOG_INFO("[%s] RFCOMM connected to %s (channel %d) -- sending the iAP2 start frame next",
                 kTag, address.c_str(), channel);
        // Object lifetime = process: core/bt/bt_trigger.cpp holds it after receiving and never deletes it
        return new BlueZRfcommStream(fd);
    }

    core::IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) override
    {
        (void)timeoutMs;
        if (err)
            *err = "server mode (registering SDP and waiting for the phone to connect) is not implemented on Linux. "
                   "Wireless CarPlay is triggered in the opposite direction -- **we connect to the phone**; use client mode: "
                   "core_selftest --bt --bt-mode client --bt-device iPhone";
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<core::IBluetooth> makeBluetooth()
{
    return std::make_unique<LinuxBluetooth>();
}

std::string controllerAddress()
{
    const int dev = hci_get_route(nullptr);   // default controller; returns -1 when there is no Bluetooth
    if (dev < 0) return "";
    bdaddr_t ba;
    if (hci_devba(dev, &ba) < 0) return "";
    char s[19] = {0};
    if (ba2str(&ba, s) < 0) return "";
    return s;   // e.g. "C8:FF:28:7B:D4:60"
}

std::string bluetoothAdapterName()
{
    std::string n = bluezAdapterName();
    if (!n.empty()) return n;

    // When bluetoothd is not running / the machine has no adapter, Adapter1.Alias is unavailable.
    // But BlueZ's default adapter name is the hostname (PRETTY_HOSTNAME takes precedence),
    // so fall back following BlueZ's rules -- do not fall back to the Bonjour name; those are different things.
    if (FILE* f = std::fopen("/etc/machine-info", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            const char* k = "PRETTY_HOSTNAME=";
            if (!std::strncmp(line, k, std::strlen(k))) {
                std::string v = line + std::strlen(k);
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' ||
                                      v.back() == '"' || v.back() == ' '))
                    v.pop_back();
                if (v.size() && v.front() == '"') v.erase(0, 1);
                std::fclose(f);
                if (!v.empty()) return v;
                break;
            }
        }
        std::fclose(f);
    }
    char host[256] = {0};
    if (gethostname(host, sizeof host - 1) == 0 && host[0])
        return host;
    return "";
}

std::string currentSSID()
{
    return nmCurrentSsid();   // empty string if unavailable (the iAP2 step can still fall back to the IP)
}

}  // namespace allplay::posix
