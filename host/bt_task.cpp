// ============================================================================
// host/bt_task.cpp —— 蓝牙触发任务（宿主层）
// ============================================================================
#include "bt_task.h"

#include "bt/bt_trigger.h"
#include "posix/posix.h"

#include <chrono>
#include <cstdio>

namespace allplay {

namespace {

void emit(const std::function<void(const std::string&)>& log, const std::string& s)
{
    if (log) log(s);
    else {
        std::printf("[bt] %s\n", s.c_str());
        std::fflush(stdout);
    }
}

}  // namespace

bool BtTask::start(const core::airplay::Receiver& rx, const BtTaskOptions& o,
                   std::function<void(const std::string&)> log)
{
    if (!done_->load()) return false;   // 上一次还在跑，不排队
    // 上一次的线程跑完了，但 std::thread 仍处于 joinable —— 直接给它赋新线程是
    // std::terminate（实测症状：自动重连第二/三次触发时进程直接 abort，
    // `libc++abi: terminating`）。它已经结束，join 是瞬间的。
    if (thread_.joinable()) thread_.join();

    // —— 参数在这里（调用线程上）全部快照成值 ——
    // 线程只碰自己 move 进去的东西，绝不回头引用 rx / Options / this。
    // 这是 stop() 能安全 detach 的前提。
    core::bt::BtTriggerParams bp;
    bp.deviceName    = o.deviceName;
    bp.deviceAddress = o.deviceAddress;
    bp.serverMode    = o.serverMode;
    bp.serverWaitMs  = 300000;

    // 照 tools/core_selftest.cpp:130-147 的装配搬过来
    auto& c = bp.iap2;
    c.name             = o.name;
    c.model            = "AllPlay1,1";
    c.manufacturer     = "AllPlay";
    c.serial           = rx.info().deviceId;
    c.firmware         = "1.0";
    c.hardware         = "1.0";
    c.btMac            = allplay::posix::controllerAddress();
    c.ssid             = o.ssid.empty() ? allplay::posix::currentSSID() : o.ssid;
    c.passphrase       = o.passphrase;
    c.ipAddresses      = {rx.ip()};
    c.airPlayPort      = rx.port();
    c.deviceIdentifier = rx.info().deviceId;
    c.publicKeyHex     = rx.info().pkHex;
    c.mfiDir           = o.identityDir;
    c.timeoutMs        = 90000;

    {
        char buf[256];
        std::snprintf(buf, sizeof buf, "开始触发：btMac=%s ssid=%s ip=%s port=%u",
                      c.btMac.c_str(), c.ssid.c_str(),
                      c.ipAddresses.empty() ? "?" : c.ipAddresses[0].c_str(),
                      unsigned(rx.port()));
        emit(log, buf);
    }

    auto transport = allplay::posix::makeBluetooth();
    if (!transport) {
        emit(log, "平台没有可用的 IBluetooth 实现");
        return false;
    }

    auto done = done_;   // shared_ptr 拷贝：线程写标志位时用不着 this
    done->store(false);
    // 取消旗标：stop() 置位后，runBtTrigger 的轮询循环 100ms 内收工。
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    bp.iap2.cancel = cancel.get();   // 指向上面的 shared_ptr，生命周期由闭包兜住
    thread_ = std::thread(
        [transport = std::move(transport), bp, o, log = std::move(log), done, cancel]() mutable {
            for (int attempt = 1; attempt <= o.retry; ++attempt) {
                if (cancel->load()) {   // 取消后不再重试
                    done->store(true);
                    return;
                }
                auto r = core::bt::runBtTrigger(transport.get(), bp, log);
                if (r.ok) {
                    done->store(true);
                    return;
                }
                // iPhone 的 iAP2 SPP 服务在蓝牙 ACL 连上之后才异步打开，单次 connectRFCOMM
                // 极易太早失败 —— 安卓侧同样整链重试（jni_bridge.cpp:443-455）。
                if (attempt < o.retry) {
                    char buf[256];
                    std::snprintf(buf, sizeof buf, "第 %d/%d 次未成（%s），%dms 后重试",
                                  attempt, o.retry, r.error.c_str(), o.retryDelayMs);
                    emit(log, buf);
                    std::this_thread::sleep_for(std::chrono::milliseconds(o.retryDelayMs));
                } else {
                    emit(log, "触发失败（已重试 " + std::to_string(o.retry) + " 次）：" + r.error);
                }
            }
            done->store(true);
        });
    return true;
}

void BtTask::stop()
{
    // 先置取消旗标：runBtTrigger 的 100ms 轮询会看到它并返回，线程随即把 done 置 true。
    // 这样手机离开后，主循环的重排逻辑能马上再叫一次；不置位的话，0x4301 发出后
    // 线程会守在旧链路上读一辈子（实测症状：手机拿回来也不进 CarPlay）。
    if (cancel_) cancel_->store(true);
    // 线程已经把 transport / 参数 / 日志回调全部 move 进了自己的闭包，不引用 this，
    // detach 是安全的 —— RFCOMM 流本来就按"对象生命周期 = 进程"泄漏
    // （platform/posix/bt_bluez.cpp:582），进程退出时一并收掉。
    //
    // 注意：std::thread 析构时若仍 joinable 会 std::terminate，所以 detach 不是可选项。
    if (thread_.joinable()) thread_.detach();
}

}  // namespace allplay
