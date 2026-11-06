/**
 * @file   hid.h
 * @brief  方控按键 → iAP2 HID 报表
 * @author blt
 */

// ============================================================================
// core/iap2/hid.h —— 方控按键 → iAP2 HID 报表
// ============================================================================
// 方控在 iAP2 上是"配件发 HID 报表"：车机按键 → HID 消费者/电话页的 usage →
// 一条 HID 报表 → 0x4E0x 报文发给手机。手机侧把 usage 翻成媒体控制。
//
// 报文里的报表格式是**我们和报表描述符之间的约定**（描述符还没声明，属于
// 上层/驱动那一半）。这里定成：
//   [reportId?] [usagePage LE u16] [usage LE u16]
// 抬起 = usage 0（HID 的惯例：按下报 usage，松手报 0）。
// reportId 传 0 表示描述符里没声明报表 ID，报文里就不带这一字节。
//
// 消息号见下面的常量：**这一族还没在真机上核验过**。本模块先把编码、队列、
// 发送路径立起来，真机接进来时用抓包对一遍；要改只有一处。
#pragma once

#include "port/control.h"

#include <cstdint>
#include <vector>

namespace core::iap2 {

struct HidUsage {
    uint16_t page = 0;   ///< HID usage page（0x0C=Consumer，0x0B=Telephony）
    uint16_t id   = 0;   ///< usage id；0 表示"没有按键"
};

/// 按键 → HID usage。没有对应项时返回 {0,0}（等同抬起）。
HidUsage mediaKeyUsage(core::MediaKey key);

/// 一次按键的报表字节（见文件头的格式说明）。
std::vector<uint8_t> hidReport(core::MediaKey key, bool down, uint8_t reportId = 0);

/// iAP2 HID 组件族的消息号（未核验，见文件头）
constexpr uint16_t kStartHidUpdates    = 0x4e00;
constexpr uint16_t kStopHidUpdates     = 0x4e01;
constexpr uint16_t kAccessoryHidReport = 0x4e02;

/// 把报表包成一条 CSM 报文（未分片，调用方按对端可承载大小切）
std::vector<uint8_t> hidReportMessage(const std::vector<uint8_t>& report);

/// 一条完整的"按下/抬起"报文，便于上层或自测直接用
std::vector<uint8_t> hidReportMessage(core::MediaKey key, bool down, uint8_t reportId = 0);

}  // namespace core::iap2
