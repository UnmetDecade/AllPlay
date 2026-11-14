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

    // ---- 0) Server mode: register SDP and wait for the phone (no device selection) ----
    if (params.serverMode) {
        say("Server mode: registering SDP 'Wireless iAPv2' and waiting for the phone to connect (up to " +
            std::to_string(params.serverWaitMs / 1000) + " seconds)...");
        std::string err0;
        core::IByteStream* stream0 =
            transport->waitForPhoneConnection(params.serverWaitMs, &err0);
        if (!stream0) {
            result.error = err0.empty() ? "Failed to wait for phone connection" : err0;
            say("Failed: " + result.error);
            return result;
        }
        say("Phone connected (iAP2 link handshake starting)");
        iap2::WirelessTrigger trigger0(stream0, params.iap2);
        trigger0.setProgress(progress);
        result.ok = trigger0.run();
        if (!result.ok) {
            result.error = trigger0.error();
            say("Trigger failed: " + result.error);
        } else {
            say("iAP2 link finished: 0x4301 sent, link stays up during CarPlay until the phone disconnects");
        }
        return result;
    }

    // ---- 1) Select device ----
    std::string deviceAddr = params.deviceAddress;
    std::string deviceName = "(by address)";
    if (deviceAddr.empty()) {
        std::string err;
        auto devices = transport->pairedDevices(&err);
        say("Paired devices: " + std::to_string(devices.size()));
        for (const auto& d : devices) {
            say("  - " + d.name + "  " + d.address + (d.connected ? "  [connected]" : ""));
        }
        if (devices.empty()) {
            result.error = err.empty() ? "No paired devices" : err;
            return result;
        }
        const core::BtDevice* pick = nullptr;
        for (const auto& d : devices) {
            if (nameMatches(d.name, params.deviceName)) { pick = &d; break; }
        }
        if (!pick) {
            result.error = "No paired device matching \"" + params.deviceName + "\"";
            return result;
        }
        deviceAddr = pick->address;
        deviceName = pick->name;
    }
    result.deviceAddress = deviceAddr;
    result.deviceName = deviceName;
    say("Target device: " + deviceName + "  " + deviceAddr);

    // ---- 2) SDP + RFCOMM ----
    say("Querying SDP and opening RFCOMM (UUID " + std::string(kIap2SppUuid) + ")...");
    std::string err;
    core::IByteStream* stream = transport->connectRFCOMM(deviceAddr, kIap2SppUuid, &err);
    if (!stream) {
        result.error = err.empty() ? "RFCOMM connection failed" : err;
        say("RFCOMM failed: " + result.error);
        return result;
    }
    say("RFCOMM connected (iAP2 link handshake starting)");

    // ---- 3) iAP2 trigger ----
    iap2::WirelessTrigger trigger(stream, params.iap2);
    trigger.setProgress(progress);
    result.ok = trigger.run();
    if (!result.ok) {
        result.error = trigger.error();
        say("Trigger failed: " + result.error);
    } else {
        say("iAP2 link finished: 0x4301 sent, link stays up during CarPlay until the phone disconnects");
    }
    return result;
}

}  // namespace core::bt
