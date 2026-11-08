/**
 * @file   usb_transport.h
 * @brief  Android Auto 的 USB 传输端口（AOAP 控制请求 + bulk 端点）
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/usb_transport.h —— AA USB 传输端口
// ============================================================================
// AOAP 的握手（GET_PROTOCOL / SEND_STRING / START）是控制传输，重枚举之后是两条
// bulk 端点。平台差异只有"怎么拿 libusb 句柄"，协议顺序由 core 的会话驱动，
// 所以 core 只依赖这个接口；platform/posix/aa_usb_libusb.cpp 实现它。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace core::projection::androidauto {

class IAaUsbDevice {
public:
    virtual ~IAaUsbDevice() = default;

    /// 稳定标识（序列号；读不到时平台自己编一个）。
    virtual std::string id() const = 0;

    /// 厂商控制请求（AOAP 的 51/52/53）。requestType 用 libusb 的位定义：
    /// 0xC0 = IN|VENDOR, 0x40 = OUT|VENDOR。
    virtual bool controlTransfer(uint8_t requestType, uint8_t request, uint16_t value,
                                 uint16_t index, uint8_t* data, uint16_t len,
                                 int timeoutMs, std::string* err) = 0;

    /// START 之后手机重枚举成 0x18D1:0x2D00/0x2D01。等它出现并打开。
    virtual std::unique_ptr<IAaUsbDevice> waitForAccessory(int timeoutMs, std::string* err) = 0;

    /// 声明接口 0，按描述符挑 bulk IN/OUT。
    virtual bool openBulk(std::string* err) = 0;

    /// 成功返回写入字节数；失败 -1。
    virtual int writeBulk(const uint8_t* data, size_t len, int timeoutMs, std::string* err) = 0;

    /// 返回读到的字节数；0 = 超时（不是错误）；-1 = 错误。
    virtual int readBulk(uint8_t* data, size_t cap, int timeoutMs, std::string* err) = 0;
};

/// 组合根注入的 USB 能力。
struct AaUsbPorts {
    /// 枚举当前可用的手机（显示用 id 列表；空 err 表示没找到）。
    std::function<std::vector<std::string>(std::string* err)> probeDevices;

    /// 按 id 打开设备（准备发 AOAP 控制请求）。
    std::function<std::unique_ptr<IAaUsbDevice>(const std::string& id, std::string* err)> open;
};

}  // namespace core::projection::androidauto
