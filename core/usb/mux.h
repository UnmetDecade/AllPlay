/**
 * @file   mux.h
 * @brief  控制面的"多路复用器"抽象：给手机上的某个端口开一条字节流
 * @author blt
 */

// ============================================================================
// core/usb/mux.h —— 控制面脚下的 mux 抽象
// ============================================================================
// 有线控制面（lockdown → carkit）一共要开三条到手机端口的连接，但"怎么开"有两种：
//
//   Linux：自己抓 USBMUX 那条 bulk 管道，在它上面跑 usbmux v2 + 迷你 TCP
//          → UsbMux（见 usbmux.h）
//   macOS：根本不碰 USB。系统 usbmuxd 已经在跑，跟它要隧道就行
//          → SystemUsbMuxd（见 platform/posix/usbmuxd_mac.cpp）
//
// 再往上（lockdown / TLS / iAP2）完全一样，所以只把"开连接"这一件事抽出来。
#pragma once

#include "port/stream.h"

#include <cstdint>
#include <memory>
#include <string>

namespace core::usb {

class IMux {
public:
    virtual ~IMux() = default;

    /// 建立 mux 会话（Linux：版本协商；macOS：确认手机在不在、修好设备号）。
    /// 任何 connectTcp 之前必须先调一次；重复调应当是无害的。
    virtual bool begin(std::string* err) = 0;

    /// 开一条到手机 port 的字节流（典型：62078 = lockdown）。
    /// 成功返回该连接（归调用方所有）；失败返回 nullptr。
    virtual std::unique_ptr<IByteStream> connectTcp(uint16_t port, int timeoutMs,
                                                    std::string* err) = 0;
};

}  // namespace core::usb
