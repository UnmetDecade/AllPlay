// ============================================================================
// host/wired_task.cpp —— 有线接入任务（宿主层）
// ============================================================================
// 两段的落地位置：
//   ① runBringUp() —— tools/wired_probe.cpp 的 ①～⑦ 那几步，一步不少地搬过来。
//      探针里是"每步都可以停在半路让人看"，这里是"跑完给出 ifname/bindAddr"。
//   ② runPump()    —— 探针的 ⑧：等 0x4300 → 回 0x4301 → 之后一直泵（保活）。
//
// 两段之间的交接点只有 Impl 里那三个 unique_ptr（pipe / control / link）：
// 它们是"手机那头还认为我们在车上"的全部凭据，断了就得从头再来。
#include "wired_task.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <utility>

// 这几个头**不放在条件编译里**：Impl 的类型、以及类本身，两种构建下都得是完整的。
// 真正依赖 libusb 的只有 openWiredCarPlay() 的实现（usb_libusb.cpp），
// 它缺席时是链接错误而不是编译错误 —— 所以只在函数体那一层开条件编译。
#include "iap2/wired_link.h"
#include "mfi/mfi_identity.h"
#include "posix/posix.h"
#include "posix/usb.h"
#include "usb/wired_control.h"

namespace allplay {

namespace {

/// 格式化成一条日志。log 为空就打到 stdout（跟 bt_task.cpp 的 emit 一个意思）。
void emit(const WiredLogFn& log, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log) log(buf);
    else {
        std::printf("[wired] %s\n", buf);
        std::fflush(stdout);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl：全部状态。两个线程都只通过 shared_ptr 摸它，所以 WiredTask 析构不等于
//       线程悬垂 —— 这是 stop() 里敢 join 的根据。
// ---------------------------------------------------------------------------
struct WiredTask::Impl {
    std::atomic<int>  phase{int(WiredPhase::Idle)};
    std::atomic<bool> stop{false};

    WiredBringUp bringUp;

    // ② 还要用的东西。**声明顺序就是析构顺序**，不能重排：
    // WiredControl 存的是 pipe 的引用，WiredLink 存的是 service() 流的引用。
    std::unique_ptr<core::IByteStream>       pipe;
    std::unique_ptr<core::usb::WiredControl> control;
    std::unique_ptr<core::iap2::WiredLink>   link;

    mutable std::mutex mu;
    std::string        err;

    void setError(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(mu);
        err = s;
    }
    std::string getError() const
    {
        std::lock_guard<std::mutex> lk(mu);
        return err;
    }
};

WiredTask::WiredTask() : impl_(std::make_shared<Impl>()) {}

WiredPhase WiredTask::phase() const { return WiredPhase(impl_->phase.load()); }

const WiredBringUp& WiredTask::result() const { return impl_->bringUp; }

std::string WiredTask::error() const { return impl_->getError(); }

#if defined(ALLPLAY_HAVE_LIBUSB) || defined(__APPLE__)

// ---------------------------------------------------------------------------
// ① USB 控制面 + 媒体面准备
// ---------------------------------------------------------------------------
void WiredTask::runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions o, WiredLogFn log)
{
    auto fail = [&](const std::string& why) {
        emit(log, "✗ %s", why.c_str());
        impl->setError(why);
        impl->phase.store(int(WiredPhase::Failed));
    };

    // ---- ① 传输层 ----
    // Linux：切 USB 配置、claim USBMUX 口，自己跑 usbmux v2。
    // macOS：不碰 USB，借系统 usbmuxd 的隧道（AppleUSBNCMData 会自己把网卡建好）。
    allplay::posix::WiredUsbInfo info;
    std::string                  err;
    std::unique_ptr<core::IByteStream> pipe;   // Linux 才有；macOS 留空

#ifdef __APPLE__
    (void)o.productId;   // macOS 不枚举 USB，限定产品号没有意义
    std::unique_ptr<core::usb::IMux> sysMux = allplay::posix::openSystemUsbMuxd(&info, &err);
    if (!sysMux) {
        fail("接上系统 usbmuxd 失败：" + err);
        return;
    }
    auto control = std::make_unique<core::usb::WiredControl>(std::move(sysMux));
    emit(log, "系统 usbmuxd 已接手（%s）—— 不切配置，控制面走 Apple 自己的隧道",
         info.serial.c_str());
#else
    allplay::posix::WiredUsbOptions uopt;
    if (o.productId > 0) uopt.productId = o.productId;
    uopt.switchWaitMs = o.switchWaitMs;

    pipe = allplay::posix::openWiredCarPlay(uopt, &info, &err);
    if (!pipe) {
        fail("打开 USB 失败：" + err);
        return;
    }
    emit(log, "USB 已进 CarPlay 配置（%s，bus %u addr %u，配置 %d）", info.serial.c_str(),
         unsigned(info.bus), unsigned(info.address), info.configValue);

    auto control = std::make_unique<core::usb::WiredControl>(*pipe);
#endif

    // ---- ② 控制面：lockdown 配对 → 会话 → 原地 TLS → carkit 服务 ----
    // 这一段的失败原因常常是"手机锁屏了"（只报 1 个配置）或"配对框没人点"，
    // 都不是代码问题 —— 所以原样把 err 透出去，别自己编一句笼统的。
    // 配对材料按手机序列号存、复用 —— 不存的话每跑一轮都换一套 HostID 和证书，
    // 手机每次都当陌生人，**每次都弹"要信任此电脑吗？"**。info.serial 是 USB
    // 描述符里的序列号，读到 DevicePublicKey 之前就有，正好能当这个键。
    control->setPairStore(info.serial, o.stateDir);
    if (!control->connect(&err)) {
        fail("控制面没跑通：" + err);
        return;
    }
    emit(log, "控制面通了（HostID=%s）", control->hostId().c_str());

    // ---- ③ iAP2 链路协商（**主机发起**：marker + SYN，不能等手机先开口）----
    auto link = std::make_unique<core::iap2::WiredLink>(*control->service());
    std::string lerr;
    if (!link->negotiate(30000, &lerr)) {
        fail("iAP2 链路没协商起来：" + lerr);
        return;
    }
    emit(log, "链路 NORMAL（对端载荷上限 %zu 字节）", link->engine().peerMaxPayload());

    // ---- ④ 识别（0x1D00 → 0x1D01 → 0x1D02）----
    // 链路 NORMAL 之后**必须立刻做**：手机等的就是这一条，干等 ~44 秒它会把 USB 重置。
    core::iap2::WiredConfig wcfg;
    wcfg.name = o.name;
    // USBHostTransport 那一组要填手机那侧 CarPlay 用的接口号，即我们认出来的 USBMUX 口。
    // 写死成 1 只是碰巧对上这台机器。
    wcfg.carPlayUsbInterfaceNumber = int(info.usbMuxInterface);
    if (!link->identify(wcfg, &lerr)) {
        fail("识别没通过：" + lerr);
        return;
    }
    emit(log, "识别通过（USBHostTransport 接口号 %d）", wcfg.carPlayUsbInterfaceNumber);

    // ---- ⑤ MFi 认证 ----
    auto mfi = core::mfi::CertificateMfiIdentity::load(o.mfiDir, &err);
    if (!mfi) {
        fail("装不上 MFi 身份（" + o.mfiDir + "）：" + err);
        return;
    }
    if (!link->authenticate(mfi.get(), 30000, &lerr)) {
        fail("MFi 认证没通过：" + lerr);
        return;
    }
    emit(log, "MFi 认证通过（证书 %zu 字节）", mfi->certificate().size());

    // ---- ⑥ 电源 + 5 条订阅 ----
    // 这两条不是走流程：有线这条道上手机真的靠我们供电；不发订阅手机不推任何状态。
    link->sendPowerSourceUpdate(wcfg.availableCurrentMilliAmps, /*charging=*/true);
    link->sendSubscriptions();
    emit(log, "已报电源 %u mA 与 5 条订阅", unsigned(wcfg.availableCurrentMilliAmps));

    // ---- ⑦ 媒体面：iPhone 那侧 NCM 功能挂出来的网卡 ----
    // Linux 上是内核 cdc_ncm 建的，我们只把地址钉上去；macOS 上是 AppleUSBNCMData
    // 建的，地址也是系统给的（按序列号在 IORegistry 里找出来）。两条路都只用到
    // "网卡名 + IPv6 链路本地"，NTB16/以太网/IPv6 本身不归我们管。
    allplay::posix::WiredNcmLink ncm;
#ifdef __APPLE__
    const bool ncmOk = allplay::posix::prepareWiredNcmDarwin(info.serial, &ncm, &err);
#else
    const bool ncmOk = allplay::posix::prepareWiredNcm(int(info.bus), int(info.address),
                                                       o.ncmLinkLocal, &ncm, &err);
#endif
    if (!ncmOk) {
        fail("有线媒体面起不来：" + err);
        return;
    }
    if (ncm.ipv6.empty()) {   // 两边都保证非空，这里只是别让 front() 踩空
        fail("NCM 网卡 " + ncm.ifname + " 上一个 IPv6 链路本地都没有");
        return;
    }

    impl->bringUp.ifname    = ncm.ifname;
    impl->bringUp.bindAddr  = ncm.ipv6.front();   // 指定的那个都排在第一个
    impl->bringUp.mac       = ncm.mac;
    impl->bringUp.serial    = info.serial;
    impl->bringUp.unmanaged = ncm.unmanaged;
    impl->bringUp.pinned    = ncm.pinned;

    emit(log, "NCM 网卡 %s：MAC %s，IPv6 %s%s", ncm.ifname.c_str(), ncm.mac.c_str(),
         ncm.ipv6.front().c_str(), ncm.pinned ? "（本次钉的）" : "");
    if (ncm.unmanaged) emit(log, "已把网卡从 NetworkManager 摘开（不然它会去要 DHCP）");

    // ---- 交给 ② ----
    // **先按析构顺序把上一轮的三个拆干净，再装新的。** 不能直接连着赋值：那样
    // `impl->control = …` 会先销毁旧的 control（TLS 流在它里面），而旧的 link 还
    // 攥着那条流的引用 —— 紧接着销毁 link 时踩的就是已析构对象，直接段错误。
    // 第一轮跑旧的三个都是空的，所以这条错路在原设计里一直没暴露；"断了重来"
    // 是第一次让这里跑到第二遍。（顺序与 Impl 的成员声明相反：link → control → pipe。）
    impl->link.reset();
    impl->control.reset();
    impl->pipe.reset();

    impl->pipe    = std::move(pipe);
    impl->control = std::move(control);
    impl->link    = std::move(link);
    impl->phase.store(int(WiredPhase::Ready));
    emit(log, "USB 侧就绪 —— 该起 Receiver 了");
}

// ---------------------------------------------------------------------------
// ② 等 0x4300 → 回 0x4301 → 保活
// ---------------------------------------------------------------------------
void WiredTask::runPump(std::shared_ptr<Impl> impl, PumpParams pp, WiredLogFn log)
{
    core::iap2::WiredCarPlayEndpoint ep;
    ep.ipv6Addresses.push_back(pp.address);
    ep.airPlayPort      = pp.airPlayPort;
    ep.deviceIdentifier = pp.deviceId;
    ep.publicKeyHex     = pp.publicKeyHex;
    ep.sourceVersion    = core::airplay::kSourceVersion;   // 不能为空，手机会挑版本

    bool     sent = false;
    uint64_t beat = core::iap2::monoMs();

    while (!impl->stop.load()) {
        std::vector<core::iap2::WiredLink::Message> msgs;
        if (!impl->link->pump(200, &msgs)) {
            emit(log, "iAP2 链路断开");
            break;
        }
        for (const auto& m : msgs) {
            if (m.id != 0x4300) {
                emit(log, "控制帧 0x%04x，%zu 字节", unsigned(m.id), m.body.size());
                continue;
            }
            core::iap2::CarPlayAvailability av;
            if (core::iap2::parseCarPlayAvailability(m.body, &av))
                emit(log, "收到 0x4300：%s", core::iap2::describeCarPlayAvailability(av).c_str());
            else
                emit(log, "收到 0x4300（解析失败，%zu 字节）", m.body.size());

            if (sent) continue;   // 手机可能反复问，回过一次就别再发（DiPlay 也是只回一次）

            // **不按 available 过滤**：DiPlay 收到就回，可不可用让手机自己判断。
            std::string serr;
            if (!impl->link->sendCarPlayStartSession(ep, &serr)) {
                emit(log, "✗ 0x4301 发不出去：%s", serr.c_str());
                impl->setError("0x4301 发不出去：" + serr);
                impl->phase.store(int(WiredPhase::Done));
                return;
            }
            sent = true;
            emit(log, "已回 0x4301 —— 等手机往 [%s]:%u 连 AirPlay", pp.address.c_str(),
                 unsigned(pp.airPlayPort));
        }
        // 心跳：10 秒一条。等 0x4300 可能要等一会儿（手机要先起它的 CarPlay 会话），
        // 一行都不打的话看着像卡死了。
        if (core::iap2::monoMs() - beat >= 10000) {
            beat = core::iap2::monoMs();
            emit(log, "%s", sent ? "0x4301 已发，链路保活中" : "在等手机的 0x4300");
        }
    }
    impl->phase.store(int(WiredPhase::Done));
}

#else  // 既没有 libusb 也不是 macOS

// 这个构建没带有线传输层：接口都在，但一调就明确报错，宿主不用为此写条件编译。
void WiredTask::runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions, WiredLogFn)
{
    impl->setError("这个构建没带有线传输层（Linux 装 libusb-1.0-dev 后重新 cmake）");
    impl->phase.store(int(WiredPhase::Failed));
}

void WiredTask::runPump(std::shared_ptr<Impl>, PumpParams, WiredLogFn) {}

#endif  // ALLPLAY_HAVE_LIBUSB / __APPLE__

// ---------------------------------------------------------------------------
// 对外接口（两半都在这儿，条件编译只在上面）
// ---------------------------------------------------------------------------
bool WiredTask::beginBringUp(const WiredTaskOptions& o, WiredLogFn log)
{
    int expect = int(WiredPhase::Idle);
    if (!impl_->phase.compare_exchange_strong(expect, int(WiredPhase::Working))) {
        // Failed / Done 允许重来（手机上点过"信任"之后重跑是常规操作），
        // 正在跑（Working / Ready / Pumping）就不打断。
        if (expect == int(WiredPhase::Failed) || expect == int(WiredPhase::Done)) {
            if (bringUpThread_.joinable()) bringUpThread_.join();
            // 上一轮的泵线程也收掉。相位落到 Done 就说明它已经退出循环了，但线程对象
            // 还挂着没 join —— 不 join 的话，新的一轮装 pipe/control/link 时，它理论上
            // 还有机会去碰那些正在被换掉的引用（见 runBringUp 末尾那段注释）。
            if (pumpThread_.joinable()) pumpThread_.join();
            impl_->setError(std::string());
            impl_->phase.store(int(WiredPhase::Working));
        } else {
            return false;
        }
    }
    impl_->stop.store(false);

    auto impl = impl_;
    bringUpThread_ = std::thread(
        [impl, o, log]() mutable { runBringUp(impl, o, std::move(log)); });
    return true;
}

bool WiredTask::startPump(const core::airplay::Receiver& rx, WiredLogFn log)
{
    if (impl_->phase.load() != int(WiredPhase::Ready)) return false;

    // rx 上的东西一律在这里（调用线程）取成值 —— 线程里不再碰 rx。
    PumpParams pp;
    pp.airPlayPort  = rx.port();
    pp.deviceId     = rx.info().deviceId;
    pp.publicKeyHex = rx.info().pkHex;
    // 报给手机的地址必须是 Receiver **实际绑的那个**：同一个 deviceid、同一把公钥、
    // 同一个端口。说一套自己监听另一套，手机连上来第一件事就是对不上。
    pp.address = impl_->bringUp.bindAddr;

    if (pumpThread_.joinable()) pumpThread_.join();
    impl_->stop.store(false);
    impl_->phase.store(int(WiredPhase::Pumping));

    auto impl = impl_;
    pumpThread_ = std::thread(
        [impl, pp, log]() mutable { runPump(impl, std::move(pp), std::move(log)); });
    return true;
}

bool WiredTask::pumpDone() const { return impl_->phase.load() == int(WiredPhase::Done); }

void WiredTask::stop()
{
    impl_->stop.store(true);
    if (pumpThread_.joinable())    pumpThread_.join();
    if (bringUpThread_.joinable()) bringUpThread_.join();
    // 停止位一置，runBringUp 醒来后也会走到 Done —— 别把它按回 Failed，
    // 那会让宿主在收尾时误报一次"有线失败"。
    if (impl_->phase.load() == int(WiredPhase::Working)) impl_->phase.store(int(WiredPhase::Done));
}

}  // namespace allplay
