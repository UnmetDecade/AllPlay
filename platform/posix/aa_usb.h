/**
 * @file   aa_usb.h
 * @brief  Android Auto 的 USB 传输层（libusb 实现）
 * @author blt
 */

// ============================================================================
// platform/posix/aa_usb.h —— Android Auto USB 传输层
// ============================================================================
// core 里的 AA 会话只认 core::projection::androidauto::AaUsbPorts（发现 + 打开 +
// 控制传输 + bulk）。这里用 libusb 把它实现出来，Linux 与 macOS 共用一份：
//   · Linux 需要 detach 内核驱动（手机常被 MTP/USB 存储驱动占着）；
//   · macOS 没有内核驱动可 detach，直接 open 即可。
// AOAP 的协议顺序（51/52/53）在 core 的会话里，本文件不参与。
#pragma once

#include "projection/androidauto/usb_transport.h"

namespace allplay::posix {

core::projection::androidauto::AaUsbPorts makeAndroidAutoUsbPorts();

}  // namespace allplay::posix
