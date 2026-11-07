/**
 * @file   wired_control.h
 * @brief  有线控制面：配对 → 会话 → TLS → 起 carkit 服务
 * @author blt
 */

// ============================================================================
// core/usb/wired_control.h —— 有线 CarPlay 的控制面
// ============================================================================
// 把 usbmux / lockdown / tls_stream / pair_record 串成一条链。手机那边一共要开
// **三条** 迷你 TCP 连接，缺一条都拿不到最后的 iAP2 流：
//
//   1. :62078  明文 lockdown —— SetValue(UntrustedHostBUID) → GetValue(DevicePublicKey)
//              → GetValue(WiFiAddress) → Pair。配完就关。
//   2. :62078  StartSession → **同一条连接原地升成 TLS** → StartService(carkit)
//              这条**要一直开着**，它一断，第三条也就废了。
//   3. StartService 给的端口 —— 新的连接，可选再做一次 TLS。**这就是 iAP2 那条流**。
//
// TLS 的客户端身份是 **root 证书**，不是 host 那张 —— 这点很容易搞反（见 pair_record.h）。
#pragma once

#include "port/stream.h"
#include "usb/mux.h"
#include "usb/pair_record.h"

#include <memory>
#include <string>

namespace core::usb {

class UsbMux;

class WiredControl {
public:
    /// pipe = platform 层给的 USB bulk 管道（见 platform/posix/usb_libusb.cpp）。
    /// **必须比本对象活得久**。mux 由本对象自己造（UsbMux），这也是 Linux 那条路。
    explicit WiredControl(IByteStream& pipe);

    /// 直接用现成的 mux（macOS：系统 usbmuxd 的隧道，见 platform/posix/usbmuxd_mac.cpp）。
    /// 这条路上没有"我们的 USB 管道"，mux 归本对象所有。
    explicit WiredControl(std::unique_ptr<IMux> mux);

    ~WiredControl();

    WiredControl(const WiredControl&) = delete;
    WiredControl& operator=(const WiredControl&) = delete;

    /// 让配对材料落盘复用（**不调就是老行为：每次现造一套，每次都要点信任**）。
    /// @param key     认手机用的键 —— 传 USB 序列号（pair_record.h 里说明了为什么不是
    ///                 DevicePublicKey）。空串 = 不落盘。
    /// @param stateDir 空 = defaultStateDir()（$ALLPLAY_STATE_DIR 或 ~/.local/state/allplay）。
    ///                 **注意 sudo**：它会改 HOME，跑在 root 下时得显式给这个值。
    void setPairStore(std::string key, std::string stateDir = {});

    /// 跑完整套控制面。成功后 service() 就能用了。
    bool connect(std::string* err);

    /// iAP2 那条流。**第一条字节就是 iAP2 链路帧**，中间没有任何额外握手、没有封装。
    IByteStream* service() const { return service_.get(); }

    /// 本次配对用的 HostID / BUID（配对记录要落盘时用得上）
    const std::string& hostId() const { return identity_.hostId; }
    const std::string& systemBuid() const { return identity_.systemBuid; }

private:
    bool doPair(std::string* err);
    bool doSessionAndService(std::string* err);
    /// 盘上有"配对成功过"的记录，就把它填进 identity_。**没发过任何报文**。
    bool loadTrustedRecord();
    /// 把会话那几条流按**析构顺序**收掉（TLS 那两条引用着各自的 raw，不能反着来）。
    void resetSessionStreams();

    IByteStream*                 pipe_ = nullptr;
    std::unique_ptr<IMux>        mux_;
    std::unique_ptr<IByteStream> pairCh_;      ///< 第 1 条，配完即弃
    std::unique_ptr<IByteStream> sessionRaw_;  ///< 第 2 条（TLS 之前的底，TLS 对象要引用它）
    std::unique_ptr<IByteStream> sessionCh_;   ///< 第 2 条对外用的那个（可能是 TLS）
    std::unique_ptr<IByteStream> serviceRaw_;  ///< 第 3 条的底
    std::unique_ptr<IByteStream> service_;     ///< 第 3 条对外用的那个
    WiredPairIdentity            identity_;
    std::string                  pairKey_;     ///< 配对材料的键（手机序列号）；空 = 不落盘
    std::string                  pairStateDir_;///< 空 = defaultStateDir()
};

}  // namespace core::usb
