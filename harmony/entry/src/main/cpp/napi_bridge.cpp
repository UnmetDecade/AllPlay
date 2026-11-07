/**
 * @file   napi_bridge.cpp
 * @brief  AllPlay 鸿蒙原生桥：ArkTS 的 SPP 字节流 ←→ core::iap2::WirelessTrigger
 * @author blt
 */

// ============================================================================
// napi_bridge.cpp —— 把 core 的无线 iAP2 触发接到鸿蒙的蓝牙 socket 上
// ============================================================================
// 线程模型：
//   · ArkTS 主线程：sppConnect / sppRead / sppWrite（系统 API 只能在 ArkTS 调）
//   · 原生工作线程：跑 WirelessTrigger::run()（阻塞式状态机，链路层 + 识别 + MFi…）
//   · 两个 threadsafe function 把原生线程的输出送回 ArkTS：
//       onWrite(ArrayBuffer) → 调 socket.sppWrite
//       onLog(string)        → 写进界面日志
//   · ArkTS → 原生只有 feed(ArrayBuffer) 一个入口（sppRead 收到的字节）
//
// 为什么不让原生自己开蓝牙：OHOS NDK 没有 SPP/RFCOMM 的 C 接口，只有 ArkTS 的
// @kit.ConnectivityKit 有。所以蓝牙留在 ArkTS，协议全在 core。
// ============================================================================

#include <napi/native_api.h>
#include <hilog/log.h>

#include "common/log.h"
#include "iap2/wireless_client.h"
#include "mfi/mfi_identity.h"
#include "port/stream.h"
#ifndef ALLPLAY_MFI_STUB
#include "platform/device_identity.h"
#endif
#ifdef ALLPLAY_HAVE_AIRPLAY
#include "airplay/server.h"
#include "harmony_audio.h"
#include "harmony_decoder.h"
#include "harmony_platform.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0B01
#define LOG_TAG "AllPlay"

namespace {

// ---------------------------------------------------------------------------
// 收/发字节流：read 阻塞等 ArkTS feed 进来的字节；write 递回 ArkTS 去写 socket
// ---------------------------------------------------------------------------
class NapiByteStream : public core::IByteStream {
public:
    explicit NapiByteStream(napi_threadsafe_function write) : write_(write) {}

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [&] { return !in_.empty() || closed_; })) {
            return 0;   // 超时
        }
        if (in_.empty()) return -1;   // 已关闭
        const size_t take = std::min(n, in_.size());
        std::copy(in_.begin(), in_.begin() + take, buf);
        in_.erase(in_.begin(), in_.begin() + take);
        return int(take);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (n == 0) return true;
        auto* payload = new std::vector<uint8_t>(buf, buf + n);
        if (napi_call_threadsafe_function(write_, payload, napi_tsfn_nonblocking) != napi_ok) {
            delete payload;
            return false;
        }
        return true;
    }

    void close() override
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void feed(const uint8_t* p, size_t n)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_.insert(in_.end(), p, p + n);
        }
        cv_.notify_all();
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<uint8_t>     in_;
    bool                    closed_ = false;
    napi_threadsafe_function write_ = nullptr;
};

// ---------------------------------------------------------------------------
// 模块状态
// ---------------------------------------------------------------------------
std::mutex        g_mu;
std::shared_ptr<NapiByteStream> g_stream;   // 工作线程持有期间非空
std::thread       g_thread;
std::atomic<bool> g_cancel{false};

napi_threadsafe_function g_logTsfn = nullptr;   // 受 g_mu 保护

#ifndef ALLPLAY_MFI_STUB
// AirPlay 配对身份（Ed25519 + pairing id）：模块级单例，第一次触发时从 stateDir 读出/生成，
// 之后一直复用 —— 每次重新生成的话手机那边的配对记录就失效了。
std::unique_ptr<core::DeviceIdentity> g_devIdentity;
#endif

/// 原生线程打一行日志：hilog + 界面
void logLine(const std::string& s)
{
    OH_LOG_INFO(LOG_APP, "%{public}s", s.c_str());
    napi_threadsafe_function tsfn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        tsfn = g_logTsfn;
    }
    if (!tsfn) return;
    auto* payload = new std::string(s);
    if (napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking) != napi_ok)
        delete payload;
}

// ---------------------------------------------------------------------------
// AirPlay 7000 探针
// ---------------------------------------------------------------------------
// 完整的 RTSP 服务器（core/airplay）还没接进 HAP。这一层先只回答一个问题：
// 手机拿到 0x4301 里的「IP + 7000」之后，到底会不会连过来。
//   bind 0.0.0.0:port → accept → 把第一条 RTSP 请求打进界面日志
// 手机连上来说明 Wi-Fi / IP / 0x4301 整段是通的，剩下才是 RTSP 协议本身。
std::thread        g_probeThread;
std::atomic<bool>  g_probeCancel{false};
std::mutex         g_probeMu;
napi_threadsafe_function g_probeLogTsfn = nullptr;   // 受 g_probeMu 保护

void probeLog(const std::string& s)
{
    OH_LOG_INFO(LOG_APP, "[airplay] %{public}s", s.c_str());
    napi_threadsafe_function tsfn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_probeMu);
        tsfn = g_probeLogTsfn;
    }
    if (!tsfn) return;
    auto* payload = new std::string(s);
    if (napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking) != napi_ok)
        delete payload;
}

/// 把 RTSP 请求头压成一行，方便看日志
std::string oneLine(const std::string& head)
{
    std::string s;
    s.reserve(head.size());
    for (char ch : head) {
        if (ch == '\r') continue;
        s += (ch == '\n') ? '|' : ch;
        if (s.size() > 320) { s += "…"; break; }
    }
    return s;
}

void probeLoop(int port)
{
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        probeLog("socket() 失败 errno=" + std::to_string(errno));
        return;
    }
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(uint16_t(port));
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        probeLog("bind " + std::to_string(port) + " 失败 errno=" + std::to_string(errno));
        ::close(lfd);
        return;
    }
    if (::listen(lfd, 4) != 0) {
        probeLog("listen 失败 errno=" + std::to_string(errno));
        ::close(lfd);
        return;
    }
    probeLog("已在 0.0.0.0:" + std::to_string(port) + " 监听 —— 等手机连过来");

    while (!g_probeCancel.load()) {
        pollfd p{lfd, POLLIN, 0};
        if (::poll(&p, 1, 500) <= 0) continue;

        sockaddr_in peer{};
        socklen_t   plen = sizeof peer;
        const int   cfd  = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) continue;

        char ip[64] = "?";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        probeLog(std::string("★ 手机连上来了：") + ip + ":" +
                 std::to_string(ntohs(peer.sin_port)));

        // 读第一条请求（到这里 iOS 已经连上 TCP，说明网络是通的）
        std::string head;
        char        buf[4096];
        for (int i = 0; i < 8 && head.find("\r\n\r\n") == std::string::npos; ++i) {
            pollfd cp{cfd, POLLIN, 0};
            if (::poll(&cp, 1, 3000) <= 0) break;
            const ssize_t n = ::recv(cfd, buf, sizeof buf, 0);
            if (n <= 0) break;
            head.append(buf, size_t(n));
            if (head.size() > 8192) break;
        }
        if (head.empty()) {
            probeLog("连上了但没发数据（3s 内）—— 手机可能在等 RTSP 响应");
        } else {
            probeLog("请求：" + oneLine(head));
        }
        ::close(cfd);
    }
    ::close(lfd);
    probeLog("7000 探针已停止");
}

const char* stageName(core::iap2::TriggerStage st)
{
    switch (st) {
        case core::iap2::TriggerStage::Idle:            return "Idle";
        case core::iap2::TriggerStage::Identified:      return "已识别";
        case core::iap2::TriggerStage::Authenticated:   return "已认证";
        case core::iap2::TriggerStage::Subscribed:      return "已订阅";
        case core::iap2::TriggerStage::WifiConfigSent:  return "Wi-Fi 配置已发";
        case core::iap2::TriggerStage::StartSessionSent: return "0x4301 已发";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// threadsafe function 的 JS 侧回调（在 ArkTS 主线程执行）
// ---------------------------------------------------------------------------
void CallJsWrite(napi_env env, napi_value jsCb, void* /*context*/, void* data)
{
    std::unique_ptr<std::vector<uint8_t>> bytes(static_cast<std::vector<uint8_t>*>(data));
    if (env == nullptr || jsCb == nullptr || !bytes) return;

    void* raw = nullptr;
    napi_value buf = nullptr;
    if (napi_create_arraybuffer(env, bytes->size(), &raw, &buf) != napi_ok) return;
    if (!bytes->empty()) std::memcpy(raw, bytes->data(), bytes->size());

    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    napi_call_function(env, undef, jsCb, 1, &buf, nullptr);
}

void CallJsLog(napi_env env, napi_value jsCb, void* /*context*/, void* data)
{
    std::unique_ptr<std::string> msg(static_cast<std::string*>(data));
    if (env == nullptr || jsCb == nullptr || !msg) return;

    napi_value arg = nullptr;
    napi_create_string_utf8(env, msg->c_str(), NAPI_AUTO_LENGTH, &arg);
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    napi_call_function(env, undef, jsCb, 1, &arg, nullptr);
}

// ---------------------------------------------------------------------------
// 从 ArkTS 的配置对象里取值
// ---------------------------------------------------------------------------
bool typeOfIs(napi_env env, napi_value v, napi_valuetype want)
{
    napi_valuetype t = napi_undefined;
    return napi_typeof(env, v, &t) == napi_ok && t == want;
}

std::string getString(napi_env env, napi_value obj, const char* key, const std::string& def = "")
{
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok || !typeOfIs(env, v, napi_string))
        return def;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return def;
    std::string s(len, '\0');
    if (napi_get_value_string_utf8(env, v, &s[0], len + 1, &len) != napi_ok) return def;
    s.resize(len);
    return s;
}

int getInt(napi_env env, napi_value obj, const char* key, int def)
{
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok || !typeOfIs(env, v, napi_number))
        return def;
    int32_t out = 0;
    if (napi_get_value_int32(env, v, &out) != napi_ok) return def;
    return out;
}

/// 取一个 ArrayBuffer 属性的副本（identity.pk8 / certificate.p7b 从 rawfile 读进来）
bool getBuffer(napi_env env, napi_value obj, const char* key, std::vector<uint8_t>* out)
{
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
    bool isAb = false;
    if (napi_is_arraybuffer(env, v, &isAb) != napi_ok || !isAb) return false;
    void* raw = nullptr;
    size_t len = 0;
    if (napi_get_arraybuffer_info(env, v, &raw, &len) != napi_ok || raw == nullptr || len == 0)
        return false;
    out->assign(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + len);
    return true;
}

}  // namespace

#ifdef ALLPLAY_HAVE_AIRPLAY
/// 手机是否已经拨到接收端（实现和接收端实例一起在文件后半段）。
/// 触发线程用它判断"0x4301 之后手机到底连没连上来"。
bool rxPhoneConnected();
#endif

// ===========================================================================
// 导出给 ArkTS 的四个函数
// ===========================================================================

// start(config, onWrite, onLog)
static napi_value Start(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 3) {
        napi_throw_type_error(env, nullptr, "start(config, onWrite, onLog) 需要 3 个参数");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_thread.joinable()) {
            napi_throw_error(env, nullptr, "上一次触发还在跑，先 stop()");
            return nullptr;
        }
    }

    // ---- 配置（必须在主线程读完，交给工作线程时已经是普通 C++ 结构）----
    core::iap2::WirelessTriggerConfig cfg;
    cfg.name         = getString(env, args[0], "name", "AllPlay");
    cfg.model        = getString(env, args[0], "model", "AllPlay1,1");
    cfg.manufacturer = getString(env, args[0], "manufacturer", "AllPlay");
    cfg.serial       = getString(env, args[0], "serial", "000000000000");
    cfg.firmware     = getString(env, args[0], "firmware", "1.0");
    cfg.hardware     = getString(env, args[0], "hardware", "1.0");
    cfg.btMac        = getString(env, args[0], "btMac");
    cfg.ssid         = getString(env, args[0], "ssid");
    cfg.passphrase   = getString(env, args[0], "passphrase");
    cfg.securityType = getInt(env, args[0], "securityType", 2);
    cfg.airPlayPort  = uint16_t(getInt(env, args[0], "airPlayPort", 7000));
    cfg.deviceIdentifier = getString(env, args[0], "deviceIdentifier");
    cfg.publicKeyHex     = getString(env, args[0], "publicKeyHex");
    cfg.mfiDir       = getString(env, args[0], "mfiDir");
    cfg.timeoutMs    = getInt(env, args[0], "timeoutMs", 45000);
    // 第一阶段没有 Wi-Fi 配置可填；IP 空着不填，0x5703 不会走到。
    std::string ip = getString(env, args[0], "ip");
    if (!ip.empty()) cfg.ipAddresses.push_back(ip);

    // ---- 两个 threadsafe function ----
    napi_value writeName = nullptr, logName = nullptr;
    napi_create_string_utf8(env, "allplay.write", NAPI_AUTO_LENGTH, &writeName);
    napi_create_string_utf8(env, "allplay.log", NAPI_AUTO_LENGTH, &logName);

    napi_threadsafe_function writeTsfn = nullptr;
    napi_threadsafe_function logTsfn = nullptr;
    if (napi_create_threadsafe_function(env, args[1], nullptr, writeName, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsWrite,
                                        &writeTsfn) != napi_ok) {
        napi_throw_error(env, nullptr, "创建 onWrite 回调失败");
        return nullptr;
    }
    if (napi_create_threadsafe_function(env, args[2], nullptr, logName, 0, 1,
                                        nullptr, nullptr, nullptr, CallJsLog,
                                        &logTsfn) != napi_ok) {
        napi_release_threadsafe_function(writeTsfn, napi_tsfn_release);
        napi_throw_error(env, nullptr, "创建 onLog 回调失败");
        return nullptr;
    }

    auto stream = std::make_shared<NapiByteStream>(writeTsfn);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stream = stream;
        g_logTsfn = logTsfn;
    }
    g_cancel.store(false);

    // ---- MFi 身份：从 HAP rawfile 读出的字节（鸿蒙不开放应用读任意路径）----
    // 放在两个回调建好之后，身份加载的成败才能同时出现在界面日志里。
    // 由工作线程持有 shared_ptr，保证比 trigger.run() 活得久。
    std::shared_ptr<core::mfi::CertificateMfiIdentity> identity;
    std::vector<uint8_t> keyDer, certDer;
    if (getBuffer(env, args[0], "mfiKey", &keyDer) &&
        getBuffer(env, args[0], "mfiCert", &certDer)) {
        std::string mfiErr;
        identity = core::mfi::CertificateMfiIdentity::loadFromMemory(
            keyDer.data(), keyDer.size(), certDer.data(), certDer.size(), &mfiErr);
        if (identity) {
            logLine("MFi 身份已载入：" + std::to_string(identity->certificate().size()) +
                    " 字节证书");
        } else {
            logLine("MFi 身份不可用：" + mfiErr);
        }
    } else {
        logLine("没有收到 MFi 身份字节 —— 0xAA01 会发空证书，手机会回 0xAA04");
    }

    // ---- AirPlay 配对身份（Ed25519 + deviceid）----
    // 0x4301 的 base 参数里要带 publicKey / deviceIdentifier。iOS 把它当成"这台接收端"
    // 的身份，后面去 7000 上做 pair-verify；这两项空着，手机不会来连 AirPlay。
    // stateDir 由 ArkTS 给（应用沙箱，卸载才清）—— 身份必须持久化，换一把手机就不认了。
#ifndef ALLPLAY_MFI_STUB
    {
        const std::string stateDir = getString(env, args[0], "stateDir");
        if (stateDir.empty()) {
            logLine("没给 stateDir —— 0x4301 会带空 publicKey，手机不会来连 AirPlay");
        } else {
            if (!g_devIdentity) {
                auto id = std::make_unique<core::DeviceIdentity>();
                std::string ierr;
                if (id->loadOrCreate(stateDir, &ierr)) {
                    g_devIdentity = std::move(id);
                } else {
                    logLine("AirPlay 身份不可用：" + ierr);
                }
            }
            if (g_devIdentity) {
                cfg.publicKeyHex = g_devIdentity->pkHex;
                bool created = false;
                cfg.deviceIdentifier = core::loadOrCreateSyntheticDeviceId(stateDir, &created);
                logLine("AirPlay 身份：deviceid=" + cfg.deviceIdentifier +
                        " pk=" + cfg.publicKeyHex.substr(0, 16) + "…" +
                        (created ? "（本次新生成）" : "（复用）"));
            }
        }
    }
#else
    logLine("ALLPLAY_MFI_STUB：没有 OpenSSL，0x4301 不带 publicKey");
#endif

    // ---- 0x4301 的落点：手机就照着这组 ip:port 回连，必须和本机实际网卡一致 ----
    {
        std::string ips;
        for (const auto& a : cfg.ipAddresses) {
            if (!ips.empty()) ips += ",";
            ips += a;
        }
        logLine("0x4301 将报告：ssid=" + (cfg.ssid.empty() ? "（空）" : cfg.ssid) +
                " ip=" + (ips.empty() ? "（空！手机无处可连）" : ips) +
                " port=" + std::to_string(cfg.airPlayPort) +
                " security=" + std::to_string(cfg.securityType));
    }

#ifdef ALLPLAY_HAVE_AIRPLAY
    // 0x4301 之后手机不拨号就重发（见 WirelessTriggerConfig::phoneConnected）
    cfg.phoneConnected = []() { return rxPhoneConnected(); };
#endif

    // ---- 工作线程 ----
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_thread = std::thread([cfg, stream, identity, writeTsfn, logTsfn]() mutable {
            logLine("iAP2 触发线程启动（core/iap2）");

            cfg.mfiIdentity = identity.get();   // 可空：空则发空证书
            core::iap2::WirelessTrigger trigger(stream.get(), cfg);
            trigger.setProgress([](const std::string& s) { logLine(s); });
            trigger.setStageCb([](core::iap2::TriggerStage st) {
                logLine(std::string("阶段 → ") + stageName(st));
            });

            const bool ok = trigger.run();
            stream->close();

            if (ok) {
                logLine("触发完成：0x4301 已发出");
            } else {
                logLine("触发结束（未成功）：" + trigger.error());
            }

            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_stream.reset();
                g_logTsfn = nullptr;
            }
            // 本线程持有的那一份引用用完即还；队列里剩下的回调仍会在主线程执行
            napi_release_threadsafe_function(writeTsfn, napi_tsfn_release);
            napi_release_threadsafe_function(logTsfn, napi_tsfn_release);
        });
    }

    return nullptr;
}

// feed(data: ArrayBuffer)
static napi_value Feed(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value arg = nullptr;
    if (napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr) != napi_ok || argc < 1)
        return nullptr;

    void* raw = nullptr;
    size_t len = 0;
    if (napi_get_arraybuffer_info(env, arg, &raw, &len) != napi_ok || raw == nullptr || len == 0)
        return nullptr;

    std::shared_ptr<NapiByteStream> stream;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        stream = g_stream;
    }
    if (stream) stream->feed(static_cast<const uint8_t*>(raw), len);
    return nullptr;
}

// stop()
static napi_value Stop(napi_env env, napi_callback_info info)
{
    (void)info;
    g_cancel.store(true);

    std::shared_ptr<NapiByteStream> stream;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        stream = g_stream;
    }
    if (stream) stream->close();   // 唤醒工作线程里阻塞的 read

    std::thread t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_thread.joinable()) t = std::move(g_thread);
    }
    if (t.joinable()) t.join();

    OH_LOG_INFO(LOG_APP, "%{public}s", "触发线程已停止");
    return nullptr;
}

// isRunning(): boolean
static napi_value IsRunning(napi_env env, napi_callback_info info)
{
    (void)info;
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        running = g_thread.joinable();
    }
    napi_value out = nullptr;
    napi_get_boolean(env, running, &out);
    return out;
}

// startAirplayProbe(port, onLog)
static napi_value StartAirplayProbe(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t port = 7000;
    if (argc >= 1) napi_get_value_int32(env, args[0], &port);

    // 先收掉上一轮
    g_probeCancel.store(true);
    if (g_probeThread.joinable()) g_probeThread.join();

    napi_threadsafe_function tsfn = nullptr;
    if (argc >= 2) {
        napi_valuetype t = napi_undefined;
        napi_typeof(env, args[1], &t);
        if (t == napi_function) {
            napi_value name = nullptr;
            napi_create_string_utf8(env, "allplayProbeLog", NAPI_AUTO_LENGTH, &name);
            napi_create_threadsafe_function(env, args[1], nullptr, name,
                                            0, 1, nullptr, nullptr, nullptr,
                                            CallJsLog, &tsfn);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_probeMu);
        g_probeLogTsfn = tsfn;
    }
    g_probeCancel.store(false);
    g_probeThread = std::thread([port, tsfn] {
        probeLoop(int(port));
        if (tsfn) napi_release_threadsafe_function(tsfn, napi_tsfn_release);
        std::lock_guard<std::mutex> lk(g_probeMu);
        if (g_probeLogTsfn == tsfn) g_probeLogTsfn = nullptr;
    });

    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// stopAirplayProbe()
static napi_value StopAirplayProbe(napi_env env, napi_callback_info /*info*/)
{
    g_probeCancel.store(true);
    if (g_probeThread.joinable()) g_probeThread.join();
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// hotspotIpv4() → string
// 本机热点（AP）网卡的 IPv4，没有热点就返回空串。
//
// 为什么不在 ArkTS 里取：鸿蒙的 connection.getAllNetsSync() **不列 AP 网卡**
// （真机实测：开着热点、手机该连 192.168.43.1，列表里却只有 wlan0 的 STA 地址）。
// 热点开着时 0x4301 必须报 AP 的地址，手机加入热点后才连得到我们，
// 所以只能绕过 NetManager，直接问内核要网卡地址。
static napi_value HotspotIpv4(napi_env env, napi_callback_info info)
{
    std::string ip;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            const std::string name = p->ifa_name ? p->ifa_name : "";
            const bool isAp = (name.rfind("softap", 0) == 0) ||
                              (name.rfind("wlan", 0) == 0 && name != "wlan0") ||
                              (name.rfind("ap", 0) == 0 && name.find("wlan") == std::string::npos);
            if (!isAp) continue;
            char buf[INET_ADDRSTRLEN] = {0};
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf) != nullptr) {
                ip = buf;
                break;
            }
        }
        freeifaddrs(ifa);
    }
    OH_LOG_INFO(LOG_APP, "hotspotIpv4 -> %{public}s", ip.empty() ? "（无热点）" : ip.c_str());
    napi_value out = nullptr;
    napi_create_string_utf8(env, ip.c_str(), ip.size(), &out);
    return out;
}

// 给手机的 _carplay-ctrl 服务打一枪（GET /ctrl-int/1/connect）。
// 系统 mDNS 发现和自建 mDNS 发现都用这一份实现。
static bool sendCarPlayPoke(const std::string& host, int port, const std::string& deviceId)
{
    if (host.empty() || port <= 0 || port >= 65536) return false;

    std::string sid;
    for (char c : deviceId) if (c != ':') sid.push_back(c);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) return false;

    bool ok = false;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        // 非阻塞 connect + 3s 超时：别把调用方钉在这里
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            rc = (::poll(&pfd, 1, 3000) > 0) ? 0 : -1;
            if (rc == 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr != 0) rc = -1;
            }
        }
        if (rc == 0) {
            const std::string hostHeader = host + ":" + std::to_string(port);
            const std::string req = "GET /ctrl-int/1/connect HTTP/1.1\r\n"
                                    "Host: " + hostHeader + "\r\n"
                                    "User-Agent: AirPlay/950.7.1\r\n"
                                    "AirPlay-Receiver-Device-ID: " + sid + "\r\n"
                                    "Connection: close\r\n\r\n";
            ::send(fd, req.data(), req.size(), 0);
            ok = true;
        }
        ::close(fd);
    }
    return ok;
}

// probeCarPlay(host, port, deviceId) → bool
// 照 Android 的 MdnsService.probeCarPlay：给手机的 _carplay-ctrl._tcp 发一枪
//   GET /ctrl-int/1/connect
//   AirPlay-Receiver-Device-ID: <deviceid 去掉冒号>
// 手机收到这枪才会主动来连我们的 RTSP 端口。光广播 _airplay._tcp 不够 ——
// 真机实测 0x4301 发完之后手机一声不吭，就是差这一步。
static napi_value ProbeCarPlay(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value args[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::string host, deviceId;
    int32_t port = 0;
    if (argc >= 1 && typeOfIs(env, args[0], napi_string)) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) == napi_ok) {
            host.resize(len);
            if (napi_get_value_string_utf8(env, args[0], &host[0], len + 1, &len) == napi_ok)
                host.resize(len);
        }
    }
    if (argc >= 2) napi_get_value_int32(env, args[1], &port);
    if (argc >= 3 && typeOfIs(env, args[2], napi_string)) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, args[2], nullptr, 0, &len) == napi_ok) {
            deviceId.resize(len);
            if (napi_get_value_string_utf8(env, args[2], &deviceId[0], len + 1, &len) == napi_ok)
                deviceId.resize(len);
        }
    }

    const bool ok = sendCarPlayPoke(host, port, deviceId);
    OH_LOG_INFO(LOG_APP, "carplay probe %{public}s:%{public}d -> %{public}s",
                host.c_str(), int(port), ok ? "已发" : "失败");
    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

// ---------------------------------------------------------------------------
// 自己实现的 mDNS 浏览（绑热点网卡）
// ---------------------------------------------------------------------------
// 为什么不用 @ohos.net.mdns：它的 createDiscoveryService / addLocalService 都
// 没有网络/网卡参数（SDK 的 @ohos.net.mdns.d.ts 里确认过），只能跟着系统默认
// 网络走。热点 + STA + 蜂窝同时存在时，默认网络不是热点，查询包发不到手机所在
// 的那张网卡上，`_carplay-ctrl` 就永远发现不了 —— 真机症状是 0x4301 发完手机
// 一声不吭、RTSP 端口 0 连接。这里直接在热点网卡上发 PTR 查询、解析应答，
// 发现服务后照旧打一枪 /ctrl-int/1/connect。
//
// 报文按 RFC 6762 手搓：只处理 PTR(12) / SRV(33) / A(1)，够用。
// ---------------------------------------------------------------------------
static std::atomic<bool> g_ctrlWatchStop{false};
static std::thread g_ctrlWatchThread;

// 把 "a.b.local" 编码成 DNS 名字（长度前缀 + 结尾 0）
static size_t encodeDnsName(uint8_t* buf, const std::string& name)
{
    size_t n = 0;
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        const size_t len = dot - start;
        if (len == 0) { start = dot + 1; continue; }
        if (len > 63) return 0;
        buf[n++] = uint8_t(len);
        std::memcpy(buf + n, name.data() + start, len);
        n += len;
        start = dot + 1;
    }
    buf[n++] = 0;
    return n;
}

// 跳过一个（可能带压缩指针的）DNS 名字，返回消耗的字节数
static int skipDnsName(const uint8_t* p, const uint8_t* end)
{
    int n = 0;
    while (p + n < end) {
        const uint8_t c = p[n];
        if (c == 0) return n + 1;
        if ((c & 0xC0) == 0xC0) return n + 2;
        n += 1 + int(c);
    }
    return -1;
}

// 解出一个（可能带压缩指针的）DNS 名字
static bool decodeDnsName(const uint8_t* p, size_t len, size_t off, std::string& out, size_t* next)
{
    out.clear();
    int jumps = 0;
    size_t cur = off;
    while (cur < len && jumps < 8) {
        const uint8_t c = p[cur];
        if (c == 0) {
            if (jumps == 0 && next) *next = cur + 1;
            return true;
        }
        if ((c & 0xC0) == 0xC0) {
            if (cur + 1 >= len) return false;
            const size_t ptr = size_t(((c & 0x3F) << 8) | p[cur + 1]);
            if (jumps == 0 && next) *next = cur + 2;
            if (ptr >= len) return false;
            cur = ptr;
            jumps++;
            continue;
        }
        if (cur + 1 + c > len) return false;
        if (!out.empty()) out.push_back('.');
        out.append(reinterpret_cast<const char*>(p + cur + 1), c);
        cur += 1 + c;
    }
    return false;
}

// 从一条应答里抠出 SRV 端口和 A 地址；两样都见过才算解析成功
struct MdnsAnswer {
    bool hasSrv = false;
    bool hasA = false;
    int port = 0;
    std::string host;
    std::vector<std::string> ptr;   // 报文里出现的 PTR 目标（服务实例名）
    std::vector<int> types;         // 出现过的记录类型，排查用
};

static bool parseMdnsResponse(const uint8_t* p, size_t len, MdnsAnswer& out)
{
    if (len < 12) return false;
    const uint16_t qd = uint16_t((p[4] << 8) | p[5]);
    const uint16_t an = uint16_t((p[6] << 8) | p[7]);
    const uint16_t ns = uint16_t((p[8] << 8) | p[9]);
    const uint16_t ar = uint16_t((p[10] << 8) | p[11]);

    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        const int n = skipDnsName(p + off, p + len);
        if (n < 0 || off + size_t(n) + 4 > len) return false;
        off += size_t(n) + 4;
    }

    const int total = int(an) + int(ns) + int(ar);
    for (int i = 0; i < total; i++) {
        const int n = skipDnsName(p + off, p + len);
        if (n < 0 || off + size_t(n) + 10 > len) return false;
        off += size_t(n);
        const uint16_t type  = uint16_t((p[off] << 8) | p[off + 1]);
        const uint16_t rdlen = uint16_t((p[off + 8] << 8) | p[off + 9]);
        const size_t rdoff = off + 10;
        if (rdoff + rdlen > len) return false;
        out.types.push_back(type);
        if (type == 12) {                                     // PTR：服务实例名
            std::string name;
            if (decodeDnsName(p, len, rdoff, name, nullptr) && !name.empty())
                out.ptr.push_back(name);
        } else if (type == 33 && rdlen >= 6) {                // SRV
            const uint16_t port = uint16_t((p[rdoff + 4] << 8) | p[rdoff + 5]);
            if (port > 0) { out.port = port; out.hasSrv = true; }
        } else if (type == 1 && rdlen == 4) {                 // A
            char buf[INET_ADDRSTRLEN] = {0};
            in_addr a{};
            std::memcpy(&a, p + rdoff, 4);
            if (inet_ntop(AF_INET, &a, buf, sizeof buf) != nullptr) {
                out.host.assign(buf);
                out.hasA = true;
            }
        }
        off = rdoff + rdlen;
    }
    return out.hasSrv && out.hasA;
}

static size_t buildMdnsQuery(uint8_t* buf, const std::string& service)
{
    size_t n = 0;
    buf[n++] = 0; buf[n++] = 0;     // id
    buf[n++] = 0; buf[n++] = 0;     // flags: 标准查询
    buf[n++] = 0; buf[n++] = 1;     // qdcount
    buf[n++] = 0; buf[n++] = 0;     // ancount
    buf[n++] = 0; buf[n++] = 0;     // nscount
    buf[n++] = 0; buf[n++] = 0;     // arcount
    n += encodeDnsName(buf + n, service);
    buf[n++] = 0; buf[n++] = 12;    // PTR
    buf[n++] = 0x80; buf[n++] = 0x01;   // IN + QU（请求单播应答）
    return n;
}

static void ctrlWatchLoop(std::string ifaceIp, std::string deviceId)
{
    in_addr ifaddr{};
    if (inet_pton(AF_INET, ifaceIp.c_str(), &ifaddr) != 1) {
        OH_LOG_INFO(LOG_APP, "[mdns] 网卡地址不合法：%{public}s", ifaceIp.c_str());
        return;
    }

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        OH_LOG_INFO(LOG_APP, "[mdns] socket 失败 errno=%{public}d", errno);
        return;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif

    // 优先抢 5353（组播应答都发到这个端口）；抢不到就用临时端口 + QU 单播应答
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(5353);
    bool boundWell = ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0;
    if (!boundWell) {
        local.sin_port = 0;
        ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local);
    }

    // 加入组播组（imr_interface 指定网卡，这是绑到热点上的关键）
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface = ifaddr;
    const int joined = ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr);
    unsigned char ttl = 255;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    unsigned char loop = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 400000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    OH_LOG_INFO(LOG_APP,
                "[mdns] 自建浏览启动：网卡 %{public}s，5353=%{public}s，组播=%{public}s",
                ifaceIp.c_str(), boundWell ? "占用成功" : "改用临时端口",
                joined == 0 ? "已加入" : "加入失败");

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);

    // 除了 _carplay-ctrl，再问一遍常用的 AirPlay 服务和服务枚举，
    // 这样日志里能直接看到手机到底广播了哪些名字。
    const char* services[] = {
        "_carplay-ctrl._tcp.local",
        "_airplay._tcp.local",
        "_raop._tcp.local",
        "_services._dns-sd._udp.local",
    };
    uint8_t queries[4][256];
    size_t qlens[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) qlens[i] = buildMdnsQuery(queries[i], services[i]);

    MdnsAnswer ans;
    int packets = 0;
    bool poked = false;
    std::vector<std::string> seenPtr;

    // 最多找 120 秒：手机从 0x4301 到广播服务通常是秒级，给足余量
    for (int tick = 0; tick < 300 && !g_ctrlWatchStop.load(); ++tick) {
        if ((tick % 5) == 0) {
            for (int i = 0; i < 4; i++) {
                ::sendto(fd, queries[i], qlens[i], 0,
                         reinterpret_cast<sockaddr*>(&dst), sizeof dst);
            }
        }

        uint8_t buf[2048];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const ssize_t rn = ::recvfrom(fd, buf, sizeof buf, 0,
                                      reinterpret_cast<sockaddr*>(&from), &fl);
        if (rn <= 0) continue;

        // 收到自己的查询回声就跳过（四条查询里长度对上一条即可）
        if (from.sin_addr.s_addr == ifaddr.s_addr) {
            bool echo = false;
            for (int i = 0; i < 4; i++) {
                if (rn == ssize_t(qlens[i])) { echo = true; break; }
            }
            if (echo) continue;
        }

        packets++;
        MdnsAnswer one;
        if (parseMdnsResponse(buf, size_t(rn), one)) {
            if (one.hasSrv) { ans.port = one.port; ans.hasSrv = true; }
            if (one.hasA)   { ans.host = one.host; ans.hasA = true; }
        }
        if (ans.hasSrv && ans.hasA && !poked) {
            char ips[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ips, sizeof ips);
            OH_LOG_INFO(LOG_APP,
                        "[mdns] 发现手机控制服务 %{public}s:%{public}d（来自 %{public}s，共 %{public}d 包）",
                        ans.host.c_str(), ans.port, ips, packets);
            const bool ok = sendCarPlayPoke(ans.host, ans.port, deviceId);
            OH_LOG_INFO(LOG_APP, "[mdns] /ctrl-int/1/connect -> %{public}s",
                        ok ? "已发" : "失败");
            poked = true;
        }
    }

    OH_LOG_INFO(LOG_APP, "[mdns] 自建浏览结束：收包 %{public}d 个，%{public}s",
                packets, poked ? "已发过 connect" : "没找到 _carplay-ctrl");
    ::close(fd);
}

// startCarPlayCtrlWatch(ifaceIp, deviceId) → bool
// 在指定网卡上自己找 _carplay-ctrl 并打一枪。ifaceIp 传空则直接返回 false。
static napi_value StartCarPlayCtrlWatch(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto getString = [&](size_t idx) {
        std::string s;
        if (idx < argc && typeOfIs(env, args[idx], napi_string)) {
            size_t len = 0;
            if (napi_get_value_string_utf8(env, args[idx], nullptr, 0, &len) == napi_ok) {
                s.resize(len);
                if (napi_get_value_string_utf8(env, args[idx], &s[0], len + 1, &len) == napi_ok)
                    s.resize(len);
            }
        }
        return s;
    };

    const std::string ifaceIp = getString(0);
    const std::string deviceId = getString(1);

    bool ok = false;
    if (!ifaceIp.empty()) {
        g_ctrlWatchStop.store(true);
        if (g_ctrlWatchThread.joinable()) g_ctrlWatchThread.join();
        g_ctrlWatchStop.store(false);
        g_ctrlWatchThread = std::thread(ctrlWatchLoop, ifaceIp, deviceId);
        ok = true;
    }

    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

static napi_value StopCarPlayCtrlWatch(napi_env env, napi_callback_info info)
{
    (void)env;
    (void)info;
    g_ctrlWatchStop.store(true);
    if (g_ctrlWatchThread.joinable()) g_ctrlWatchThread.join();
    g_ctrlWatchStop.store(false);
    return nullptr;
}

// airplayIdentity(stateDir) → { deviceId, pk, pi }
// 给 ArkTS 侧广播 Bonjour（mdns.addLocalService）用：TXT 里的 deviceid / pk / pi
// 必须和 0x4301 里报的是同一份身份，手机才认得出"这是刚才那台车机"。
static napi_value AirplayIdentity(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr, };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::string stateDir;
    if (argc >= 1 && typeOfIs(env, args[0], napi_string)) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) == napi_ok) {
            stateDir.resize(len);
            if (napi_get_value_string_utf8(env, args[0], &stateDir[0], len + 1, &len) == napi_ok)
                stateDir.resize(len);
        }
    }

    napi_value out = nullptr;
    napi_create_object(env, &out);
    auto set = [&](const char* k, const std::string& v) {
        napi_value s = nullptr;
        napi_create_string_utf8(env, v.c_str(), v.size(), &s);
        napi_set_named_property(env, out, k, s);
    };

#ifndef ALLPLAY_MFI_STUB
    if (!stateDir.empty()) {
        if (!g_devIdentity) {
            auto id = std::make_unique<core::DeviceIdentity>();
            std::string ierr;
            if (id->loadOrCreate(stateDir, &ierr)) g_devIdentity = std::move(id);
            else OH_LOG_WARN(LOG_APP, "AirPlay 身份不可用：%{public}s", ierr.c_str());
        }
        if (g_devIdentity) {
            bool created = false;
            set("deviceId", core::loadOrCreateSyntheticDeviceId(stateDir, &created));
            set("pk", g_devIdentity->pkHex);
            set("pi", g_devIdentity->pi);
            return out;
        }
    }
#endif
    set("deviceId", "");
    set("pk", "");
    set("pi", "");
    return out;
}

// ===========================================================================
// AirPlay 接收端（core/airplay：RTSP + 会话状态机 + 视频流解包）
// ===========================================================================
// 手机拿到 0x4301 之后会来连 airPlayPort（实测：1 秒内就来）。这一层把它接住：
//   /info → /pair-setup → /pair-verify → /auth-setup → SETUP → RECORD
// 视频/音频输出暂不装（nullptr）—— 先把协议走到 RECORD，再谈解码上屏。
#ifdef ALLPLAY_HAVE_AIRPLAY

std::mutex   g_rxMu;                       // 保护 g_receiver / g_rxMfi
std::unique_ptr<core::airplay::Receiver> g_receiver;
std::unique_ptr<core::mfi::CertificateMfiIdentity> g_rxMfi;
std::atomic<napi_threadsafe_function> g_rxTsfn{nullptr};   // 日志 → ArkTS

/// 触发线程问："手机拨过来没有？"
/// 接收端还没起来时返回 true = "没法判断，别催"—— 但**不**代表手机已连上；
/// run() 只在它为 true 时停止催，所以接收端一建起来就会立刻反映真实状态。
bool rxPhoneConnected()
{
    std::lock_guard<std::mutex> lk(g_rxMu);
    if (!g_receiver) return true;
    return g_receiver->connecting() || g_receiver->sessionActive();
}

/// core 的日志（LOG_INFO 等）转进 hilog + 界面。用原子量而不是 g_rxMu：
/// Receiver::start() 持锁期间 core 会打日志，sink 里再去拿锁就自锁了。
void rxLogSink(core::Level lv, const char* msg)
{
    const char* tag = lv == core::Level::Error ? "core!" : "core";
    OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, msg);
    napi_threadsafe_function tsfn = g_rxTsfn.load();
    if (!tsfn) return;
    auto* payload = new std::string(std::string("[core] ") + msg);
    if (napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking) != napi_ok)
        delete payload;
}

// ---------------------------------------------------------------------------
// 视频/音频输出：这一版只"接住"——数帧、认关键帧、把配置打出来
// ---------------------------------------------------------------------------
// 为什么必须先有它：Session 只有在 video != nullptr 时才建屏幕流、才会在 SETUP
// 回包里给手机 dataPort。没有它，手机会把 RECORD 之后的那次 SETUP 当成失败，
// 5 秒后断开整场会话（实测：14 条请求后 read 超时）。
// 解码上屏（OHOS AVCodec / XComponent）是下一步，接口先按 core 的要求接满。
class HarmonyVideoOutput : public core::IVideoOutput {
public:
    void configure(core::VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& csd) override
    {
        width_ = width;
        height_ = height;
        const int kind = codec == core::VideoCodec::HEVC ? 1 : 0;
        kind_ = kind;
        rxLogSink(core::Level::Info,
                  ("视频配置：" + std::string(kind ? "HEVC" : "H.264") + " " +
                   std::to_string(width) + "x" + std::to_string(height) +
                   "，codecData " + std::to_string(csd.size()) + " 字节").c_str());
        if (!dec_.configure(kind, width, height, csd)) {
            rxLogSink(core::Level::Warn, "解码器没建起来（等 surface 或等下一个关键帧）");
        }
    }

    bool feedAnnexB(const uint8_t* data, size_t len) override
    {
        bool key = false;
        // 扫 NAL 类型：5 = H.264 IDR；HEVC 的 IDR_W_RADL 在 (b>>1)&0x3f == 19..21
        for (size_t i = 0; i + 4 < len; ++i) {
            if (data[i] != 0 || data[i + 1] != 0) continue;
            if (data[i + 2] != 1) continue;
            const uint8_t h = data[i + 3];
            if (kind_ == 0) {
                if ((h & 0x1f) == 5) { key = true; break; }        // H.264 IDR
            } else {
                const uint8_t t = (h >> 1) & 0x3f;                  // HEVC IRAP 16..21
                if (t >= 16 && t <= 21) { key = true; break; }
            }
        }
        const bool pushed = dec_.feed(data, len, key);
        const long long n = frames_.fetch_add(1) + 1;
        if (key) keyFrames_.fetch_add(1);
        if (n <= 3 || n % 600 == 0) {
            const std::string msg = std::string("视频帧 #") + std::to_string(n) +
                                    (key ? "（关键帧）" : "") + "，" + std::to_string(len) +
                                    " 字节，已上屏 " + std::to_string(dec_.rendered());
            rxLogSink(core::Level::Info, msg.c_str());
        }
        // 解码器还没就绪（surface 未到 / 缓冲满）就返回 false：
        // core 会把闸门重新放下，等下一个 IDR 再来 —— 这正是我们要的。
        return pushed;
    }

    void reset() override { dec_.reset(); }
    void flush() override {}

    void setSurface(uint64_t id) { dec_.setSurface(id); }

private:
    allplay::harmony::OhosVideoDecoder dec_;
    int kind_ = 0;
    std::atomic<long long> frames_{0};
    std::atomic<long long> keyFrames_{0};
    int width_ = 0, height_ = 0;
};

// 音频：OH_AudioCodec 解码 → OHAudio 渲染（harmony_audio.cpp）。
// 解码/送声卡都在 core 的 AudioStream 线程里同步做，这里只负责日志与计数。
class HarmonyAudioOutput : public core::IAudioOutput {
public:
    void configure(core::AudioCodec codec, int sampleRate, int channels) override
    {
        const char* name = codec == core::AudioCodec::OPUS ? "Opus"
                         : codec == core::AudioCodec::AAC_LC ? "AAC-LC" : "LPCM";
        rxLogSink(core::Level::Info,
                  ("音频配置：" + std::string(name) + " " + std::to_string(sampleRate) + "Hz/" +
                   std::to_string(channels) + "ch").c_str());
        sink_.configure(codec, sampleRate, channels);
    }

    void feed(const uint8_t* payload, size_t len, uint32_t sample) override
    {
        sink_.feed(payload, len, sample);
        const long long n = packets_.fetch_add(1) + 1;
        if (n == 1 || n % 1500 == 0) {
            const std::string msg = std::string("音频包 #") + std::to_string(n) +
                                    "，解码输出 " + std::to_string(sink_.decodedFrames()) +
                                    "，已送声卡 " + std::to_string(sink_.playedBuffers());
            rxLogSink(core::Level::Info, msg.c_str());
        }
    }

    void reset() override { sink_.reset(); }

    void selfTest(int ms) { sink_.selfTest(ms); }
    void selfTestAac(const uint8_t* data, size_t len) { sink_.selfTestAac(data, len); }

private:
    allplay::harmony::OhosAudioSink sink_;
    std::atomic<long long> packets_{0};
};

HarmonyVideoOutput g_videoOut;
HarmonyAudioOutput g_audioOut;

// startAirplayServer(config, onLog) → bool
static napi_value StartAirplayServer(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc < 1) {
        napi_throw_type_error(env, nullptr, "startAirplayServer(config, onLog) 需要 1~2 个参数");
        return nullptr;
    }

    // ---- 先把上一场的接收端收干净（端口只有一个）----
    {
        std::lock_guard<std::mutex> lk(g_rxMu);
        if (g_receiver) { g_receiver->stop(); g_receiver.reset(); }
        g_rxMfi.reset();
    }

    // ---- 日志回调 ----
    napi_threadsafe_function old = g_rxTsfn.exchange(nullptr);
    if (old) napi_release_threadsafe_function(old, napi_tsfn_release);
    if (argc >= 2 && typeOfIs(env, args[1], napi_function)) {
        napi_value name = nullptr;
        napi_create_string_utf8(env, "allplayAirplayLog", NAPI_AUTO_LENGTH, &name);
        napi_threadsafe_function tsfn = nullptr;
        if (napi_create_threadsafe_function(env, args[1], nullptr, name, 0, 1,
                                            nullptr, nullptr, nullptr, CallJsLog,
                                            &tsfn) == napi_ok) {
            g_rxTsfn.store(tsfn);
        }
    }
    core::setLogSink(&rxLogSink);

    // ---- 配置 ----
    core::airplay::ReceiverConfig cfg;
    cfg.name     = getString(env, args[0], "name", "AllPlay");
    cfg.port     = uint16_t(getInt(env, args[0], "port", 7000));
    cfg.stateDir = getString(env, args[0], "stateDir");
    cfg.btMac    = getString(env, args[0], "btMac");
    cfg.width    = getInt(env, args[0], "width", 1280);
    cfg.height   = getInt(env, args[0], "height", 720);
    // 广播交给 ArkTS（@ohos.net.mdns 已经报过 _airplay._tcp）；这里用空实现，
    // 但 enableAvahi 保持 true —— Receiver 少一个必填依赖反而更脆。
    cfg.enableAvahi = true;
    // 接口必须给：没有 video 就没有屏幕流，SETUP 拿不到 dataPort，手机 5 秒后退场。
    cfg.video = &g_videoOut;
    cfg.audio = &g_audioOut;

    // ---- MFi 身份（HAP rawfile → 内存）----
    std::vector<uint8_t> keyDer, certDer;
    const bool haveKey  = getBuffer(env, args[0], "mfiKey", &keyDer);
    const bool haveCert = getBuffer(env, args[0], "mfiCert", &certDer);
    if (haveKey && haveCert) {
        std::string err;
        auto ident = core::mfi::CertificateMfiIdentity::loadFromMemory(
            keyDer.data(), keyDer.size(), certDer.data(), certDer.size(), &err);
        if (!ident) {
            rxLogSink(core::Level::Warn, ("MFi 身份解析失败：" + err).c_str());
        } else {
            g_rxMfi = std::move(ident);
            cfg.mfiIdentity = g_rxMfi.get();
        }
    } else {
        rxLogSink(core::Level::Warn, "没拿到 MFi 身份，/auth-setup 会回 400");
    }

    // ---- 起服务 ----
    const core::PlatformServices services = allplay::harmony::makeHarmonyPlatformServices();
    std::string err;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(g_rxMu);
        g_receiver = std::make_unique<core::airplay::Receiver>();
        ok = g_receiver->start(cfg, services, &err);
        if (!ok) {
            g_receiver.reset();
            g_rxMfi.reset();
            core::setLogSink(nullptr);
        }
    }

    if (ok) {
        OH_LOG_INFO(LOG_APP, "AirPlay 接收端已启动 :%{public}u（ifname=%{public}s ip=%{public}s）",
                    unsigned(g_receiver->port()), g_receiver->ifname().c_str(),
                    g_receiver->ip().c_str());
    } else {
        OH_LOG_ERROR(LOG_APP, "AirPlay 接收端启动失败：%{public}s", err.c_str());
        rxLogSink(core::Level::Error, ("接收端启动失败：" + err).c_str());
    }

    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

// stopAirplayServer()
static napi_value StopAirplayServer(napi_env env, napi_callback_info /*info*/)
{
    {
        std::lock_guard<std::mutex> lk(g_rxMu);
        if (g_receiver) { g_receiver->stop(); g_receiver.reset(); }
        g_rxMfi.reset();
    }
    core::setLogSink(nullptr);
    napi_threadsafe_function old = g_rxTsfn.exchange(nullptr);
    if (old) napi_release_threadsafe_function(old, napi_tsfn_release);

    napi_value out = nullptr;
    napi_get_undefined(env, &out);
    return out;
}

// airplayServerPort() → number（0 = 没在跑）
static napi_value AirplayServerPort(napi_env env, napi_callback_info /*info*/)
{
    std::lock_guard<std::mutex> lk(g_rxMu);
    const int port = g_receiver ? int(g_receiver->port()) : 0;
    napi_value out = nullptr;
    napi_create_int32(env, port, &out);
    return out;
}

static double propNumber(napi_env env, napi_value obj, const char* key, double def)
{
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return def;
    double d = def;
    return napi_get_value_double(env, v, &d) == napi_ok ? d : def;
}

static bool propBool(napi_env env, napi_value obj, const char* key, bool def)
{
    napi_value v = nullptr;
    if (napi_get_named_property(env, obj, key, &v) != napi_ok) return def;
    bool b = def;
    return napi_get_value_bool(env, v, &b) == napi_ok ? b : def;
}

// sendTouch([{x, y, down}, ...]) → bool
// 坐标是**归一化**的（0..1，相对协议里协商的主屏 1280x720），不是像素：
// 缩放成像素是 core 的事（EventChannel 要按 /info 里报的 widthPixels 填 HID 坐标）。
static napi_value SendTouch(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    std::vector<core::airplay::TouchPoint> contacts;
    bool isArr = false;
    if (argc >= 1 && napi_is_array(env, args[0], &isArr) == napi_ok && isArr) {
        uint32_t n = 0;
        napi_get_array_length(env, args[0], &n);
        if (n > 2) n = 2;   // HID 触摸描述符就只有两个触点槽
        for (uint32_t i = 0; i < n; ++i) {
            napi_value el = nullptr;
            if (napi_get_element(env, args[0], i, &el) != napi_ok) continue;
            core::airplay::TouchPoint p;
            double x = propNumber(env, el, "x", 0.0);
            double y = propNumber(env, el, "y", 0.0);
            p.x = float(x < 0 ? 0 : (x > 1 ? 1 : x));
            p.y = float(y < 0 ? 0 : (y > 1 ? 1 : y));
            p.down = propBool(env, el, "down", true);
            contacts.push_back(p);
        }
    }

    bool ok = false;
    {
        // 和 start/stop 共用一把锁：sendTouch 走事件通道的 TCP 写，
        // 这里持锁时间只到一次写完（未连接时立即失败），不会挡住别的操作。
        std::lock_guard<std::mutex> lk(g_rxMu);
        if (g_receiver) ok = g_receiver->sendTouch(contacts);
    }

    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

#endif  // ALLPLAY_HAVE_AIRPLAY

// setVideoSurface(surfaceId: string) —— XComponent 的 surface id，string 是 OHOS 的约定
static napi_value SetVideoSurface(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    uint64_t id = 0;
    if (argc >= 1 && typeOfIs(env, args[0], napi_string)) {
        std::string s;
        size_t len = 0;
        if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) == napi_ok && len > 0) {
            s.resize(len);
            if (napi_get_value_string_utf8(env, args[0], &s[0], len + 1, &len) == napi_ok) {
                s.resize(len);
                id = std::strtoull(s.c_str(), nullptr, 10);
            }
        }
    }
    OH_LOG_INFO(LOG_APP, "setVideoSurface(%{public}llu)", (unsigned long long)id);
    g_videoOut.setSurface(id);

    napi_value out = nullptr;
    napi_get_boolean(env, id != 0, &out);
    return out;
}

// testAudioSink(ms) —— 不开会话，直接放一段正弦验证 OHAudio 渲染器
static napi_value TestAudioSink(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t ms = 1000;
    if (argc >= 1) napi_get_value_int32(env, args[0], &ms);
    if (ms < 100) ms = 100;
    if (ms > 5000) ms = 5000;
    OH_LOG_INFO(LOG_APP, "testAudioSink(%{public}d)", ms);
    g_audioOut.selfTest(ms);

    napi_value out = nullptr;
    napi_get_boolean(env, true, &out);
    return out;
}

// testAudioAac(bytes: Uint8Array) —— 用一段 ADTS 样片自检 AAC 解码（不需要手机）
static napi_value TestAudioAac(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    const uint8_t* data = nullptr;
    size_t len = 0;
    bool ok = false;
    if (argc >= 1) {
        napi_typedarray_type tt = napi_uint8_array;
        void* raw = nullptr;
        napi_value ab = nullptr;
        size_t off = 0;
        if (napi_get_typedarray_info(env, args[0], &tt, &len, &raw, &ab, &off) == napi_ok) {
            data = static_cast<const uint8_t*>(raw);
            ok = true;
        }
    }
    OH_LOG_INFO(LOG_APP, "testAudioAac(%{public}zu 字节)", ok ? len : size_t(0));
    if (ok) g_audioOut.selfTestAac(data, len);

    napi_value out = nullptr;
    napi_get_boolean(env, ok, &out);
    return out;
}

// ---------------------------------------------------------------------------
// 模块注册
// ---------------------------------------------------------------------------
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"feed", nullptr, Feed, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isRunning", nullptr, IsRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startAirplayProbe", nullptr, StartAirplayProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAirplayProbe", nullptr, StopAirplayProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"airplayIdentity", nullptr, AirplayIdentity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"probeCarPlay", nullptr, ProbeCarPlay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startCarPlayCtrlWatch", nullptr, StartCarPlayCtrlWatch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopCarPlayCtrlWatch", nullptr, StopCarPlayCtrlWatch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hotspotIpv4", nullptr, HotspotIpv4, nullptr, nullptr, nullptr, napi_default, nullptr},
#ifdef ALLPLAY_HAVE_AIRPLAY
        {"startAirplayServer", nullptr, StartAirplayServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAirplayServer", nullptr, StopAirplayServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"airplayServerPort", nullptr, AirplayServerPort, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setVideoSurface", nullptr, SetVideoSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendTouch", nullptr, SendTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"testAudioSink", nullptr, TestAudioSink, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"testAudioAac", nullptr, TestAudioAac, nullptr, nullptr, nullptr, napi_default, nullptr},
#endif
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "allplay_core",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAllPlayCoreModule(void)
{
    napi_module_register(&g_module);
}
