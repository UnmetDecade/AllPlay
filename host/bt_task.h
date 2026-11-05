/**
 * @file   bt_task.h
 * @brief  桌面宿主的蓝牙触发任务（iAP2 → 0x4301）
 * @author blt
 */

// ============================================================================
// host/bt_task.h —— 蓝牙触发任务（宿主层）
// ============================================================================
// 为什么需要单独一个任务：`core::bt::runBtTrigger()` 是**全阻塞**的，而且**没有取消接口**
// （core/iap2/wireless_client.cpp 里 100ms 轮询跑到 cfg.timeoutMs 为止；发出 0x4301 之后
//  也不会提前返回，只是让之后的超时仍报 ok=true）。直接放在 SDL 主线程上会把画面冻住——
// 而"看画面"恰恰是这个程序存在的意义。
//
// 线程自己拥有 transport 与全部触发参数（start() 里 move 进闭包），
// 因此 stop() 可以安全 detach。详见 bt_task.cpp。
#pragma once

#include "airplay/server.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace allplay {

struct BtTaskOptions {
    std::string name          = "AllPlay";  ///< iAP2 识别信息里的配件名（和 Bonjour 名一致）
    std::string deviceName    = "iPhone";   ///< 名字包含匹配
    std::string deviceAddress;              ///< 非空则按地址直连（跳过名字匹配）
    bool        serverMode    = false;      ///< Linux/macOS 真机可用的是 client（我们主动连手机）
    std::string ssid;                       ///< 空则取 allplay::posix::currentSSID()
    std::string passphrase;                 ///< 0x5703 里发给手机的 Wi-Fi 密码
    std::string identityDir   = "identity"; ///< MFi 身份目录
    int         retry         = 10;         ///< 整链重试次数（对齐安卓 jni_bridge.cpp:445）
    int         retryDelayMs  = 3000;
};

class BtTask {
public:
    BtTask() = default;
    ~BtTask() { stop(); }
    BtTask(const BtTask&) = delete;
    BtTask& operator=(const BtTask&) = delete;

    /// 起一次触发。**必须在 rx.start() 之后调用**——参数要用 rx.ip()/port()/info()。
    /// 上一次还没跑完时返回 false（不排队，也不打断）。
    bool start(const core::airplay::Receiver& rx, const BtTaskOptions& o,
               std::function<void(const std::string&)> log);

    /// 上一次触发是否已结束。false = 还在跑。
    bool done() const { return done_->load(); }

    /// 收尾：置取消旗标 → 线程在 100ms 内自己收工（会把 done 置 true），然后 detach。
    /// 手机离开后调用它，下一次 start() 才能重新叫 CarPlay —— 否则 0x4301 发出后
    /// 线程会一直守在旧链路上，永远不会再叫。
    void stop();

private:
    std::thread                        thread_;
    /// 用 shared_ptr 而不是成员 bool：线程要能写它，又不许引用 this（见 stop()）。
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(true);
    /// 本次触发的取消旗标（每次 start() 新建一个，旧线程拿着旧的，互不影响）。
    std::shared_ptr<std::atomic<bool>> cancel_;
};

}  // namespace allplay
