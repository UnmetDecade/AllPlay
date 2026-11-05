#include "bt/bt_trigger.h"

#include "common/log.h"

#include <algorithm>
#include <cctype>

namespace core::bt {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool nameMatches(const std::string& name, const std::string& filter)
{
    if (filter.empty()) return true;
    return lower(name).find(lower(filter)) != std::string::npos;
}

}  // namespace

BtTriggerResult runBtTrigger(core::IBluetooth* transport, const BtTriggerParams& params,
                             const std::function<void(const std::string&)>& progress)
{
    auto say = [&](const std::string& s) {
        LOG_INFO("[bt] %s", s.c_str());
        if (progress) progress(s);
    };

    BtTriggerResult result;

    // ---- 0) 服务端模式：注册 SDP 等手机连（不选设备）----
    if (params.serverMode) {
        say("服务端模式：注册 SDP 'Wireless iAPv2' 并等手机连接（最多 " +
            std::to_string(params.serverWaitMs / 1000) + " 秒）…");
        std::string err0;
        core::IByteStream* stream0 =
            transport->waitForPhoneConnection(params.serverWaitMs, &err0);
        if (!stream0) {
            result.error = err0.empty() ? "等手机连接失败" : err0;
            say("失败：" + result.error);
            return result;
        }
        say("手机已连入（iAP2 链路开始握手）");
        iap2::WirelessTrigger trigger0(stream0, params.iap2);
        trigger0.setProgress(progress);
        result.ok = trigger0.run();
        if (!result.ok) {
            result.error = trigger0.error();
            say("触发失败：" + result.error);
        } else {
            say("iAP2 链路已结束：0x4301 发过，CarPlay 期间链路一直保持到手机断开");
        }
        return result;
    }

    // ---- 1) 选设备 ----
    std::string deviceAddr = params.deviceAddress;
    std::string deviceName = "(按地址)";
    if (deviceAddr.empty()) {
        std::string err;
        auto devices = transport->pairedDevices(&err);
        say("已配对设备 " + std::to_string(devices.size()) + " 个");
        for (const auto& d : devices) {
            say("  - " + d.name + "  " + d.address + (d.connected ? "  [已连接]" : ""));
        }
        if (devices.empty()) {
            result.error = err.empty() ? "没有已配对设备" : err;
            return result;
        }
        const core::BtDevice* pick = nullptr;
        for (const auto& d : devices) {
            if (nameMatches(d.name, params.deviceName)) { pick = &d; break; }
        }
        if (!pick) {
            result.error = "没有名字匹配 \"" + params.deviceName + "\" 的已配对设备";
            return result;
        }
        deviceAddr = pick->address;
        deviceName = pick->name;
    }
    result.deviceAddress = deviceAddr;
    result.deviceName = deviceName;
    say("目标设备：" + deviceName + "  " + deviceAddr);

    // ---- 2) SDP + RFCOMM ----
    say("查 SDP 并开 RFCOMM（UUID " + std::string(kIap2SppUuid) + "）…");
    std::string err;
    core::IByteStream* stream = transport->connectRFCOMM(deviceAddr, kIap2SppUuid, &err);
    if (!stream) {
        result.error = err.empty() ? "RFCOMM 连接失败" : err;
        say("RFCOMM 失败：" + result.error);
        return result;
    }
    say("RFCOMM 已连接（iAP2 链路开始握手）");

    // ---- 3) iAP2 触发 ----
    iap2::WirelessTrigger trigger(stream, params.iap2);
    trigger.setProgress(progress);
    result.ok = trigger.run();
    if (!result.ok) {
        result.error = trigger.error();
        say("触发失败：" + result.error);
    } else {
        say("iAP2 链路已结束：0x4301 发过，CarPlay 期间链路一直保持到手机断开");
    }
    return result;
}

}  // namespace core::bt
