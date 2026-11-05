// ============================================================================
// platform/posix/bt_bluez.cpp —— Linux 传输层（BlueZ / libbluetooth）
// ============================================================================
// 方向（与 macOS 那版一致）：**我们（车机）主动连手机的 iAP2 SPP 服务**。
// iPhone 从不主动发起，所以协议主路径是 connectRFCOMM（core/bt/bt_trigger.cpp:92）。
//
// 本文件在目标机（Ubuntu 22.04，bluetoothd 5.64，iPhone 16 已配对且已连接）上
// 实测确认过三件事 —— 这是选"客户端模式"的全部依据，别凭印象改：
//
//   1. SDP 查得到 iPhone 的 iAP2 记录。`sdptool browse 68:44:65:AA:AE:4B` 里：
//        Service Name: Wireless iAP      UUID 128: 00000000-deca-fade-deca-deafdecacafe
//        Protocol Descriptor List: L2CAP / RFCOMM   Channel: 1
//      （同机的 "Wireless iAP v2"（02030302-…）也在**同一个通道 1**。）
//   2. 裸 RFCOMM socket 连通道 1 **能连上**：
//        socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM) + connect(addr, 1) → 成功
//   3. 连上后**手机不先发数据** —— iAP2 的起始帧 0xFF 0x5A 由我方发出，
//      与 core/iap2/wireless_client.cpp 的实现方向一致。
//
// 由此：pairedDevices 走 BlueZ D-Bus（org.bluez），connectRFCOMM 走
// SDP 查通道 + 裸 RFCOMM socket。**服务端模式留作诚实的桩** —— 无线 CarPlay 的
// 触发方向就是我们连手机，本项目没有"等手机连进来"的场景（见 bt_trigger.h 的说明）。
//
// ⚠️ 真机跑通还差一份 **MFi 身份**（identity/identity.pk8 + certificate.p7b）。
//    蓝牙链路与 iAP2 识别（到手机发 0xAA00 请求证书为止）都不需要它，能实测；
//    再往后 iPhone 会因空证书回 0xAA04。凭证来自 Apple MFi 计划，不是代码能解决的。
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

/// 单次 RFCOMM 连接的超时。手机侧的 iAP2 SPP 服务要等蓝牙 ACL 连上后才异步打开，
/// 太短会连不上 —— 但 SDP 查询本身已经建立过一次 L2CAP，等于先把链路捂热了。
/// 调用方仍应自己重试（安卓端是 10 次 × 3s，见 jni_bridge.cpp:417 附近）。
constexpr int kConnectTimeoutMs = 15000;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/// "00000000-deca-fade-deca-deafdecacafe" → 16 字节（与 uuid 字符串同序，网络序）
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

/// 系统 D-Bus 连接（进程级共享；dbus_bus_get 返回的是单例，**不要 unref**）
DBusConnection* systemBus()
{
    static DBusConnection* c = []() -> DBusConnection* {
        DBusError e;
        dbus_error_init(&e);
        DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
        if (!conn) {
            LOG_WARN("[%s] 连不上系统 D-Bus：%s", kTag,
                     dbus_error_is_set(&e) ? e.message : "未知");
            dbus_error_free(&e);
            return nullptr;
        }
        // 总线掉线时别把整个进程带走 —— 我们只是取个设备列表
        dbus_connection_set_exit_on_disconnect(conn, false);
        return conn;
    }();
    return c;
}

// ---------------------------------------------------------------------------
// RFCOMM 字节流：把 fd 包成 core::IByteStream
// ---------------------------------------------------------------------------
// 与 macOS 那版（MacRfcommStream）语义一致：**对象生命周期 = 进程**，调用方不 delete
// （core/bt/bt_trigger.cpp 拿到流后就一直持有，从不释放）。
//
// **fd 的所有权归读线程**：close() 只 shutdown 唤醒读线程，由读线程在退出时真正 close。
// 这样避免"主线程关了 fd、读线程还在用同一个 fd 号"的经典竞态（fd 号可能已被复用）。
class BlueZRfcommStream : public core::IByteStream {
public:
    explicit BlueZRfcommStream(int fd) : fd_(fd)
    {
        reader_ = std::thread([this] { readerLoop(); });
        reader_.detach();   // 进程级对象，不需要 join
    }

    ~BlueZRfcommStream() override
    {
        // 正常路径不会走到这（对象不释放）。真走到这，必须先让读线程停下，
        // 否则它还在用 this —— 所以这里只做 shutdown，不 delete 任何东西。
        close();
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cvData_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [&] { return !queue_.empty() || closed_; }))
            return 0;                      // 超时
        if (queue_.empty()) return -1;     // 已断且无残留
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
            // MSG_NOSIGNAL：对端断开时返回 EPIPE 而不是把进程打死（我们没装 SIGPIPE 处理）
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
        if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);   // 唤醒阻塞的 read；**不 close**（读线程负责）
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
            break;   // 0 = 对端正常关闭；<0 = 出错（含 shutdown 把 read 唤醒）
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
        LOG_INFO("[%s] RFCOMM 流已结束", kTag);
    }

    int                 fd_;
    std::mutex          fdMu_;          // 保护 fd_ 本身
    std::thread         reader_;

    std::mutex              mu_;        // 保护下面这一组
    std::condition_variable cvData_;
    std::deque<uint8_t>     queue_;
    bool                    closed_ = false;
};

// ---------------------------------------------------------------------------
// SDP：按 UUID 查 RFCOMM 通道号
// ---------------------------------------------------------------------------
/// 查到返回 1..30 的通道号；查不到返回 -1 并写 err。
/// 成功后 sdp_close 会断掉那条 L2CAP —— 但蓝牙 ACL 链路已经建立，紧接着连 RFCOMM 更稳。
int findRfcommChannel(const std::string& address, const uint8_t uuidRaw[16], std::string* err)
{
    bdaddr_t dst;
    if (str2ba(address.c_str(), &dst) != 0) {
        if (err) *err = "蓝牙地址形态不对：" + address;
        return -1;
    }
    // 本机侧留空 = 让内核挑默认控制器。
    // 不用 BDADDR_ANY：它是 `(&(bdaddr_t){{0,...}})` 这样的复合字面量，
    // 在 C++ 里对它取地址属于 "taking address of rvalue"。
    bdaddr_t src{};

    sdp_session_t* sess = sdp_connect(&src, &dst, SDP_RETRY_IF_BUSY);
    if (!sess) {
        if (err) *err = "SDP 连接失败（手机没在听 / 未配对 / 蓝牙未开）";
        return -1;
    }

    uuid_t svc;
    sdp_uuid128_create(&svc, uuidRaw);
    sdp_list_t* search = sdp_list_append(nullptr, &svc);
    uint32_t    range  = 0x0000ffff;                       // 全属性
    sdp_list_t* attrs  = sdp_list_append(nullptr, &range);

    sdp_list_t* rsp     = nullptr;
    int         channel = -1;
    if (sdp_service_search_attr_req(sess, search, SDP_ATTR_REQ_RANGE, attrs, &rsp) != 0) {
        if (err) *err = "SDP 查询请求失败";
    } else {
        for (sdp_list_t* r = rsp; r && channel < 0; r = r->next) {
            auto* rec = static_cast<sdp_record_t*>(r->data);
            sdp_list_t* protos = nullptr;
            if (sdp_get_access_protos(rec, &protos) != 0 || !protos) continue;

            // ⚠️ 把 **protos 整个**传进去，不是 `protos->data`。
            //    `protos->data` 是内部的协议描述符列表，传错会让 sdp_get_proto_port
            //    解引用野指针、当场 SIGSEGV —— 这个坑实测踩过（gdb 栈顶就是它）。
            //    用法在 iPhone 上按 4 条不同记录验证过：iAP2→1、Handsfree→8、
            //    电话本→13、MAP→2，与 `sdptool browse` 逐条一致。
            const int ch = sdp_get_proto_port(protos, RFCOMM_UUID);
            if (ch > 0) channel = ch;

            // 只释放外层：内层列表一次性泄漏，无所谓（每次连接只跑一遍）
            sdp_list_free(protos, nullptr);
        }
        if (channel < 0 && err)
            *err = "SDP 里有这个 UUID 的记录，但没有 RFCOMM 通道（不是 SPP 服务？）";
    }

    if (rsp) sdp_list_free(rsp, nullptr);   // 记录体同样一次性泄漏，见上
    sdp_list_free(search, nullptr);
    sdp_list_free(attrs, nullptr);
    sdp_close(sess);
    return channel;
}

// ---------------------------------------------------------------------------
// 裸 RFCOMM 连接（带超时）
// ---------------------------------------------------------------------------
int openRfcomm(const std::string& address, uint8_t channel, std::string* err)
{
    bdaddr_t dst;
    if (str2ba(address.c_str(), &dst) != 0) {
        if (err) *err = "蓝牙地址形态不对：" + address;
        return -1;
    }

    const int fd = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0) {
        if (err) *err = std::string("socket(AF_BLUETOOTH) 失败：") + std::strerror(errno) +
                        "（内核没蓝牙栈？）";
        return -1;
    }

    // 阻塞式 connect 在手机没在听时会挂很久（蓝牙重传很慢）→ 非阻塞 + poll 自己控超时
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    sockaddr_rc sa{};
    sa.rc_family  = AF_BLUETOOTH;
    sa.rc_bdaddr  = dst;
    sa.rc_channel = channel;

    int r = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (r < 0 && errno != EINPROGRESS) {
        if (err) *err = std::string("RFCOMM 连接失败：") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (r < 0) {
        pollfd p{};
        p.fd     = fd;
        p.events = POLLOUT;
        const int pr = ::poll(&p, 1, kConnectTimeoutMs);
        if (pr == 0) {
            if (err) *err = "RFCOMM 连接超时（" + std::to_string(kConnectTimeoutMs) +
                            "ms；手机刚配对完时 SPP 服务可能还没起来，重试一次）";
            ::close(fd);
            return -1;
        }
        if (pr < 0) {
            if (err) *err = std::string("poll 失败：") + std::strerror(errno);
            ::close(fd);
            return -1;
        }
        int soErr = 0;
        socklen_t sl = sizeof(soErr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl);
        if (soErr != 0) {
            if (err) *err = std::string("RFCOMM 连接被拒：") + std::strerror(soErr) +
                            "（通道号不对？手机没在听？）";
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, fl);   // 恢复阻塞：读线程用阻塞 read 最省事
    return fd;
}

// ---------------------------------------------------------------------------
// BlueZ D-Bus：已配对设备
// ---------------------------------------------------------------------------
/// 走 org.freedesktop.DBus.ObjectManager.GetManagedObjects，筛 org.bluez.Device1 且 Paired。
/// 注意目标机上**配对设备可能上百台**（用户自己的耳机/车/手表都在），所以名字要过滤。
std::vector<core::BtDevice> bluezPairedDevices(std::string* err)
{
    std::vector<core::BtDevice> out;
    DBusConnection* c = systemBus();
    if (!c) {
        if (err) *err = "连不上系统 D-Bus（bluetoothd 没跑？）";
        return out;
    }

    DBusMessage* m = dbus_message_new_method_call(
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    if (!m) {
        if (err) *err = "构造 D-Bus 请求失败（内存？）";
        return out;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 8000, &e);
    dbus_message_unref(m);
    if (!r) {
        if (err) *err = std::string("问 org.bluez 要设备列表失败：") +
                        (dbus_error_is_set(&e) ? e.message : "未知");
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
            dbus_message_iter_next(&de);            // 跳过 object path
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
                    // Alias 是用户在 bluetoothctl 里看到的那个名字（"iPhone 16"），优先用它
                    d.name = !alias.empty() ? alias : name;
                    if (paired && !d.address.empty()) out.push_back(std::move(d));
                }
                dbus_message_iter_next(&ifaces);
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(r);

    // 已连接的排前面 —— 用户十有八九要连的就是那台
    std::stable_sort(out.begin(), out.end(),
                     [](const core::BtDevice& a, const core::BtDevice& b) {
                         return a.connected && !b.connected;
                     });
    if (out.empty() && err)
        *err = "BlueZ 里没有已配对设备（先在系统设置里配对一次手机）";
    return out;
}

// ---------------------------------------------------------------------------
// BlueZ D-Bus：适配器名（手机蓝牙列表里显示的那个名字）
// ---------------------------------------------------------------------------
/// 取 org.bluez.Adapter1 的 Alias（用户改过的名字就存这儿）；
/// 没改过时 Alias 与 Name 相同（默认是主机名）。用 GetManagedObjects 而不是直接
/// 拼 /org/bluez/hci0 —— hci 编号在多适配器机器上不保证是 0。
/// 不用 hci_read_local_name：那条路要 CAP_NET_ADMIN，普通用户跑必失败。
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
            dbus_message_iter_next(&de);              // 跳过 object path
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
// NetworkManager D-Bus：当前 Wi-Fi SSID
// ---------------------------------------------------------------------------
/// iAP2 的 0x5703 要把"车机的 Wi-Fi"告诉手机。这里取当前连着的那个 SSID。
/// 拿不到就返回空串（core_selftest 有 --bt-ssid 可显式给）。
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

/// NM_DEVICE_TYPE_WIFI（NetworkManager 的枚举值，不是我们能改的）
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
            ap.str == "/")   // 没连任何 AP 时 NM 给的就是 "/"
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
            if (err) *err = "UUID 形态不对（要 8-4-4-4-12）：" + uuid;
            return nullptr;
        }

        // 先 SDP 查通道。查不到**不直接放弃** —— iOS 的 iAP2 服务在通道 1 是众所周知
        // 的事实（本文件顶部实测也印证了），SDP 本身在刚配对完时容易查空。
        std::string sdpErr;
        int channel = findRfcommChannel(address, raw, &sdpErr);
        if (channel < 0) {
            LOG_WARN("[%s] SDP 查通道失败（%s），回退到通道 1", kTag, sdpErr.c_str());
            channel = 1;
        } else {
            LOG_INFO("[%s] SDP：%s → RFCOMM 通道 %d", kTag, uuid.c_str(), channel);
        }

        const int fd = openRfcomm(address, uint8_t(channel), err);
        if (fd < 0) return nullptr;

        LOG_INFO("[%s] RFCOMM 已连上 %s（通道 %d）—— 接下来发 iAP2 起始帧",
                 kTag, address.c_str(), channel);
        // 对象生命周期 = 进程：core/bt/bt_trigger.cpp 拿到后一直持有，从不 delete
        return new BlueZRfcommStream(fd);
    }

    core::IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) override
    {
        (void)timeoutMs;
        if (err)
            *err = "Linux 端未实现服务端模式（注册 SDP 等手机连进来）。"
                   "无线 CarPlay 的触发方向是**我们连手机**，请用客户端模式："
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
    const int dev = hci_get_route(nullptr);   // 默认控制器；没蓝牙时返回 -1
    if (dev < 0) return "";
    bdaddr_t ba;
    if (hci_devba(dev, &ba) < 0) return "";
    char s[19] = {0};
    if (ba2str(&ba, s) < 0) return "";
    return s;   // 形如 "C8:FF:28:7B:D4:60"
}

std::string bluetoothAdapterName()
{
    std::string n = bluezAdapterName();
    if (!n.empty()) return n;

    // bluetoothd 没跑 / 机器上根本没有适配器时拿不到 Adapter1.Alias。
    // 但 BlueZ 的默认适配器名就是主机名（PRETTY_HOSTNAME 优先），
    // 所以这里照着 BlueZ 的规则回退 —— 别退回 Bonjour 名，那是两回事。
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
    return nmCurrentSsid();   // 拿不到返回空串（iAP2 那步还能靠 IP 兜）
}

}  // namespace allplay::posix
