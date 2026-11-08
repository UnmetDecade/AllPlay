/**
 * @file   info.h
 * @brief  /info 响应的二进制 plist 构造
 * @author blt
 */

// ============================================================================
// core/airplay/info.h —— /info 响应的二进制 plist 构造
// ============================================================================
// /info 是手机连上 RTSP 后问的第一件事：它从里面读我们的能力（features/
// displays/HID/音频格式…）。字段值大多需要**真机校准**（见 PROTOCOL-NOTES §7/§8），
// 这里先给出结构正确、与协议行为对齐的初值，M3 联调时逐项对照真机调整。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core::airplay {

struct DeviceInfo {
    std::string name       = "AllPlay";
    std::string deviceId;            ///< AA:BB:CC:DD:EE:FF（网卡 MAC）
    std::string btMac;               ///< 本机蓝牙 MAC（bluetoothIDs，音频路由用）
    std::string pi;                  ///< pairing uuid
    std::string pkHex;               ///< ed25519 pubkey hex
    int         width  = 1280;       ///< 主屏宽（向手机协商的目标分辨率）
    int         height = 720;
    int         fps    = 60;
    uint64_t    features = 0x615653AEE2ULL;   ///< CarPlay 特性位（含音频；参照 CarPlay 特性位定义）
};

/// 生成 /info 的 binary plist
std::vector<uint8_t> buildInfoPlist(const DeviceInfo& info);

/// 主屏 UUID（协议固定值；副屏 …0002 先不做）
extern const char* const kMainDisplayUuid;

/// version 字符串（sourceVersion / srcvers / protovers 用）
extern const char* const kSourceVersion;   ///< "370.10"
extern const char* const kProtoVersion;    ///< "1.1"

}  // namespace core::airplay
