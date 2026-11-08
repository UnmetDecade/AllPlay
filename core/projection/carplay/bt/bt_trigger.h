/**
 * @file   bt_trigger.h
 * @brief  蓝牙触发编排：选设备 → 连 RFCOMM → 跑 iAP2
 * @author blt
 */

// ============================================================================
// core/bt/bt_trigger.h —— 蓝牙触发编排：选设备 → 连 RFCOMM → 跑 iAP2
// ============================================================================
#pragma once

#include "port/bluetooth.h"
#include "iap2/wireless_client.h"

#include <functional>
#include <string>

namespace core::bt {

struct BtTriggerParams {
    std::string deviceName;      ///< 名字过滤（包含即可，如 "iPhone"）
    std::string deviceAddress;   ///< 非空则按地址直连（跳过名字匹配）
    bool        serverMode = true;   ///< true=注册 SDP 等手机连（通行做法）；
                                     ///< false=主动连手机的 iAP2 服务（备选）
    int         serverWaitMs = 180000;  ///< 服务端等手机连接的时长
    iap2::WirelessTriggerConfig iap2;   ///< 识别/无线/端点/MFi 配置（调用方填好）
};

struct BtTriggerResult {
    bool        ok = false;
    std::string error;
    std::string deviceName;
    std::string deviceAddress;
};

/// 选设备 → SDP/RFCOMM → WirelessTrigger::run()（发出 0x4301 即成功）。
/// RFCOMM 之后保持打开（流由传输层持有到进程结束，等手机连 AirPlay）。
BtTriggerResult runBtTrigger(core::IBluetooth* transport, const BtTriggerParams& params,
                             const std::function<void(const std::string&)>& progress);

}  // namespace core::bt
