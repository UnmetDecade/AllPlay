/**
 * @file   aa_usb_libusb.cpp
 * @brief  Android Auto 的 libusb 传输层（AOAP 控制请求 + 手机重枚举 + bulk 端点）
 */

#include "posix/aa_usb.h"

#include "common/log.h"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace allplay::posix {

namespace {

using core::projection::androidauto::AaUsbPorts;
using core::projection::androidauto::IAaUsbDevice;

constexpr const char* kTag = "aa-usb";

constexpr uint16_t kGoogleVendorId = 0x18d1;
constexpr uint16_t kAoapProductIds[] = {0x2d00, 0x2d01};

bool isAccessoryDevice(const libusb_device_descriptor& dd)
{
    if (dd.idVendor != kGoogleVendorId) return false;
    return dd.idProduct == kAoapProductIds[0] || dd.idProduct == kAoapProductIds[1];
}

/// 非手机（键盘、集线器、Apple 自己的设备）不打扰。
bool obviouslyNotPhone(const libusb_device_descriptor& dd)
{
    if (dd.idVendor == 0x05ac) return true;
    if (dd.bDeviceClass == 0x03 /*HID*/ || dd.bDeviceClass == 0x09 /*hub*/) return true;
    return false;
}

std::string serialOf(libusb_device_handle* h, const libusb_device_descriptor& dd)
{
    if (dd.iSerialNumber == 0) return {};
    unsigned char buf[256] = {};
    const int n = libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, buf,
                                                     sizeof buf);
    if (n <= 0) return {};
    return std::string(reinterpret_cast<char*>(buf), size_t(n));
}

std::string makeId(const libusb_device_descriptor& dd, libusb_device* dev,
                   const std::string& serial)
{
    if (!serial.empty()) return serial;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04x:%04x:%u:%u", unsigned(dd.idVendor),
                  unsigned(dd.idProduct), unsigned(libusb_get_bus_number(dev)),
                  unsigned(libusb_get_device_address(dev)));
    return buf;
}

/// 问一下手机支不支持 AOAP（只读，不会让它换模式）。
bool supportsAoap(libusb_device_handle* h)
{
    uint8_t v[2] = {};
    const int rc = libusb_control_transfer(h, 0xC0, 51, 0, 0, v, 2, 250);
    if (rc < 0) return false;
    return (uint16_t(v[0]) | (uint16_t(v[1]) << 8)) >= 1;
}

class LibusbAaDevice final : public IAaUsbDevice {
public:
    LibusbAaDevice(libusb_context* ctx, libusb_device_handle* handle, std::string id)
        : ctx_(ctx), handle_(handle), id_(std::move(id))
    {
    }

    ~LibusbAaDevice() override { close(); }
    LibusbAaDevice(const LibusbAaDevice&) = delete;
    LibusbAaDevice& operator=(const LibusbAaDevice&) = delete;

    std::string id() const override { return id_; }

    bool controlTransfer(uint8_t requestType, uint8_t request, uint16_t value,
                         uint16_t index, uint8_t* data, uint16_t len,
                         int timeoutMs, std::string* err) override
    {
        if (!handle_) { if (err) *err = "USB 设备已关闭"; return false; }
        const int rc = libusb_control_transfer(
            handle_, requestType, request, value, index, data, len,
            uint16_t(timeoutMs > 0 ? timeoutMs : 1000));
        if (rc < 0 || (len > 0 && rc != int(len))) {
            if (err) *err = std::string("控制传输失败：") + libusb_error_name(rc);
            return false;
        }
        return true;
    }

    std::unique_ptr<IAaUsbDevice> waitForAccessory(int timeoutMs,
                                                   std::string* err) override
    {
        // AOAP START 之后手机会用新的 PID 重新枚举，旧句柄必然失效。
        close();

        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) != 0) {
            if (err) *err = "libusb_init 失败";
            return nullptr;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 15000);
        while (std::chrono::steady_clock::now() < deadline) {
            libusb_device** list = nullptr;
            const ssize_t n = libusb_get_device_list(ctx, &list);
            if (n < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
                libusb_device_descriptor dd{};
                if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
                if (!isAccessoryDevice(dd)) continue;

                libusb_device_handle* h = nullptr;
                if (libusb_open(list[i], &h) != 0) continue;
                const std::string id = makeId(dd, list[i], serialOf(h, dd));
                libusb_free_device_list(list, 1);
                LOG_INFO("[%s] AOAP 配件模式：%04x:%04x %s", kTag, dd.idVendor,
                         dd.idProduct, id.c_str());
                return std::make_unique<LibusbAaDevice>(ctx, h, id);
            }
            libusb_free_device_list(list, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        libusb_exit(ctx);
        if (err)
            *err = "手机没有切到 Android Auto 模式（START 之后没等到 18d1:2d00/2d01）。"
                   "确认手机上装了 Android Auto，且 USB 模式没被别的程序占着。";
        return nullptr;
    }

    bool openBulk(std::string* err) override
    {
        if (!handle_) { if (err) *err = "USB 设备已关闭"; return false; }

#if defined(__linux__)
        // 手机常被 MTP 驱动占着接口；libusb 能自动 detach。
        libusb_set_auto_detach_kernel_driver(handle_, 1);
#endif

        // AOAP 配件只用 configuration 1。有的手机（或 macOS 的 IOKit 状态）会
        // 拒绝重复设置，BUSY 时沿用当前配置继续。
        const int cfgRc = libusb_set_configuration(handle_, 1);
        if (cfgRc < 0 && cfgRc != LIBUSB_ERROR_BUSY)
            LOG_WARN("[%s] set_configuration(1)：%s", kTag, libusb_error_name(cfgRc));

        int iface = 0;
        uint8_t epIn = 0, epOut = 0;
        if (!findBulkEndpoints(&iface, &epIn, &epOut)) {
            if (err) *err = "手机的 AOAP 接口上没有找到 bulk 端点";
            return false;
        }

        const int claim = libusb_claim_interface(handle_, iface);
        if (claim != 0) {
            if (err) *err = std::string("claim 接口 ") + std::to_string(iface) +
                            " 失败：" + libusb_error_name(claim) +
                            "（macOS 上通常是别的程序开着手机；Linux 上可能被 MTP 占着）";
            return false;
        }
        claimed_    = true;
        interface_  = iface;
        bulkIn_     = epIn;
        bulkOut_    = epOut;
        LOG_INFO("[%s] bulk 就绪：接口 %d out=0x%02x in=0x%02x", kTag, iface,
                 unsigned(epOut), unsigned(epIn));
        return true;
    }

    int writeBulk(const uint8_t* data, size_t len, int timeoutMs,
                  std::string* err) override
    {
        if (!handle_ || !claimed_) { if (err) *err = "bulk 未就绪"; return -1; }
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, bulkOut_,
                                            const_cast<uint8_t*>(data), int(len),
                                            &transferred,
                                            timeoutMs > 0 ? timeoutMs : 10000);
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            if (err) *err = std::string("bulk OUT 失败：") + libusb_error_name(rc);
            return -1;
        }
        return transferred;
    }

    int readBulk(uint8_t* data, size_t cap, int timeoutMs, std::string* err) override
    {
        if (!handle_ || !claimed_) {
            if (err) *err = "bulk 未就绪";
            return -1;
        }
        int transferred = 0;
        const int rc = libusb_bulk_transfer(handle_, bulkIn_, data, int(cap),
                                            &transferred,
                                            timeoutMs > 0 ? timeoutMs : 1000);
        if (rc == LIBUSB_ERROR_TIMEOUT) return 0;
        if (rc != 0) {
            if (err) *err = std::string("bulk IN 失败：") + libusb_error_name(rc);
            return -1;
        }
        return transferred;
    }

private:
    struct EndpointPair { int interface; uint8_t in; uint8_t out; };

    bool findBulkEndpoints(int* iface, uint8_t* epIn, uint8_t* epOut)
    {
        libusb_device* dev = libusb_get_device(handle_);
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_active_config_descriptor(dev, &cfg) != 0 ||
            cfg == nullptr)
            return false;

        EndpointPair found{-1, 0, 0};
        // 优先接口 0（AOAP 的约定），否则取第一个有完整 bulk 对的接口。
        for (int i = 0; i < int(cfg->bNumInterfaces) && found.interface < 0; ++i) {
            for (int a = 0; a < int(cfg->interface[i].num_altsetting) &&
                            found.interface < 0; ++a) {
                const libusb_interface_descriptor& d =
                    cfg->interface[i].altsetting[a];
                uint8_t in = 0, out = 0;
                for (int e = 0; e < int(d.bNumEndpoints); ++e) {
                    const uint8_t addr = d.endpoint[e].bEndpointAddress;
                    if ((d.endpoint[e].bmAttributes & 0x03) != 0x02) continue;
                    if (addr & 0x80) { if (!in) in = addr; }
                    else             { if (!out) out = addr; }
                }
                if (in && out) found = {d.bInterfaceNumber, in, out};
            }
        }
        libusb_free_config_descriptor(cfg);
        if (found.interface < 0) return false;
        *iface = found.interface;
        *epIn  = found.in;
        *epOut = found.out;
        return true;
    }

    void close()
    {
        if (handle_) {
            if (claimed_) libusb_release_interface(handle_, interface_);
            libusb_close(handle_);
            handle_ = nullptr;
        }
        claimed_ = false;
        if (ctx_) { libusb_exit(ctx_); ctx_ = nullptr; }
    }

    libusb_context*      ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    std::string          id_;
    bool                 claimed_ = false;
    int                  interface_ = 0;
    uint8_t              bulkIn_ = 0;
    uint8_t              bulkOut_ = 0;
};

/// 枚举一遍，按 id 找设备；找到就 open（不影响别的设备）。
std::unique_ptr<IAaUsbDevice> openById(libusb_context* ctx, const std::string& want,
                                       std::string* err)
{
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        if (err) *err = "枚举 USB 设备失败";
        return nullptr;
    }
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (obviouslyNotPhone(dd) && !isAccessoryDevice(dd)) continue;

        libusb_device_handle* h = nullptr;
        if (libusb_open(list[i], &h) != 0) continue;
        const std::string id = makeId(dd, list[i], serialOf(h, dd));
        if (id == want) {
            libusb_free_device_list(list, 1);
            if (!isAccessoryDevice(dd) && !supportsAoap(h)) {
                libusb_close(h);
                if (err) *err = "设备 " + want + " 不支持 AOAP";
                return nullptr;
            }
            return std::make_unique<LibusbAaDevice>(ctx, h, id);
        }
        libusb_close(h);
    }
    libusb_free_device_list(list, 1);
    if (err) *err = "找不到 Android Auto 设备：" + want + "（可能已拔出或重新枚举）";
    return nullptr;
}

}  // namespace

AaUsbPorts makeAndroidAutoUsbPorts()
{
    AaUsbPorts ports;

    ports.probeDevices = [](std::string* err) {
        std::vector<std::string> out;
        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) != 0) {
            if (err) *err = "libusb_init 失败";
            return out;
        }

        libusb_device** list = nullptr;
        const ssize_t n = libusb_get_device_list(ctx, &list);
        if (n < 0) {
            libusb_exit(ctx);
            if (err) *err = "枚举 USB 设备失败";
            return out;
        }
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (obviouslyNotPhone(dd) && !isAccessoryDevice(dd)) continue;

            libusb_device_handle* h = nullptr;
            if (libusb_open(list[i], &h) != 0) continue;
            const bool ok = isAccessoryDevice(dd) || supportsAoap(h);
            if (ok) out.push_back(makeId(dd, list[i], serialOf(h, dd)));
            libusb_close(h);
        }
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);

        if (out.empty() && err)
            *err = "没有找到支持 Android Auto 的手机：先用 USB 线连上，"
                   "解锁后在通知里选「文件传输 / Android Auto」。";
        return out;
    };

    ports.open = [](const std::string& id, std::string* err)
        -> std::unique_ptr<IAaUsbDevice> {
        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) != 0) {
            if (err) *err = "libusb_init 失败";
            return nullptr;
        }
        auto dev = openById(ctx, id, err);
        if (!dev) libusb_exit(ctx);
        // 成功时 ctx 的所有权交给 LibusbAaDevice。
        return dev;
    };

    return ports;
}

}  // namespace allplay::posix
