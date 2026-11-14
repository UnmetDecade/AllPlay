// ============================================================================
// platform/posix/usb_libusb.cpp -- wired CarPlay USB transport (libusb)
// ============================================================================
// Of the whole wired link, only this part has to touch hardware directly. Four steps:
//   1. Find the iPhone (Apple VID, excluding keyboards/hubs)
//   2. Make it expose a different configuration set: vendor 0xC0/0x52/index 4
//   3. **Select** the CarPlay configuration ourselves: libusb_set_configuration
//   4. Claim the USBMUX interface and wrap its two bulk endpoints as a core::IByteStream
//
// Keeping steps 2 and 3 separate comes from hard experience (real hardware): vendor 0x52 only
// makes the iPhone re-enumerate with an extra configuration set; **the host decides which one is
// activated**. The Linux kernel picks "a configuration a driver can bind", landing on the generic USB-tethering
// one -- it still creates an interface and looks like success, but the media plane does not exist. So step 3 is mandatory; DiPlay also calls setConfiguration itself after open.
//
// Step 3 can also hit LIBUSB_ERROR_BUSY. There are three sources, and the kernel **does not distinguish
// who holds it** (it rejects if any interface is held under the current configuration):
//   - ipheth (kernel driver bound to the Apple Ethernet interface) -> this file detaches it automatically; after the configuration switch the kernel
//     re-probes it itself, so there is no need to rebind;
//   - usbmuxd (udev starts it on every re-enumeration) -> we cannot detach it, so the user has to stop it first;
//   - gvfsd-gphoto2 (GNOME photo-import daemon holding the PTP interface) -> same as above.
// What to do about the latter two is in kBusyHint, printed to the user verbatim.
//
// The media plane is not here: after switching into the CarPlay configuration, the kernel's cdc_ncm binds standard CDC-NCM itself and
// creates the network interface, and AirPlay binds its IPv6 link-local address directly (see the header of usb.h).
// This file claims only the USBMUX interface and touches nothing else.
#include "posix/usb.h"

#include "common/log.h"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace allplay::posix {

namespace {

constexpr const char* kTag = "usb";

constexpr uint16_t kAppleVendorId = 0x05ac;
// Non-iPhone Apple devices likely to be present on the same machine: listing them only confuses the detection
constexpr uint16_t kAppleKeyboardProduct = 0x024f;
constexpr uint16_t kAppleHubProduct      = 0x1006;

// Vendor request to switch the configuration set (same as DiPlay / LIVI)
constexpr uint8_t  kCarPlayConfigRequest = 0x52;
constexpr uint16_t kCarPlayConfigIndex   = 0x0004;
constexpr uint8_t  kVendorDeviceIn       = 0xc0;
// How many times to resend the vendor request when no CarPlay configuration appears. The budget is for "a human unlocking the phone",
// not "waiting for hardware to catch up" -- if the screen really is locked, waiting longer changes nothing, so after a few tries it errors out and prints the configuration table.
constexpr int      kCarPlayConfigTries   = 3;

// Interface fingerprints
constexpr uint8_t kUsbMuxClass = 0xff, kUsbMuxSubclass = 0xfe, kUsbMuxProtocol = 0x02;
constexpr uint8_t kNcmControlClass = 0x02, kNcmControlSubclass = 0x0d;
constexpr uint8_t kNcmDataClass = 0x0a;
constexpr uint8_t kAppleEthClass = 0xff, kAppleEthSubclass = 0xfd, kAppleEthProtocol = 0x01;

// Max bytes the reader thread takes at once. USBMUX never carries huge packets, so 16K is enough without waste.
constexpr int kBulkInSize = 16 * 1024;
// Reader thread poll interval: both the idle wake-up period and the worst-case exit latency after close().
// The reader does not use async + cancel_transfer -- the small gain is not worth the complexity.
constexpr int kBulkPollMs = 100;
// The write timeout is much longer than the read timeout: if a write cannot go out, the device is basically gone.
constexpr int kBulkWriteTimeoutMs = 2000;

const char* kBusyHint =
    "if any interface is held under the current configuration, the kernel refuses -- the two usual holders must move out of the way first:\n"
    "    sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd   # udev pulls it back up on every re-enumeration\n"
    "    pkill -f gvfsd-gphoto2                                       # GNOME photo import, holds the iPhone's PTP interface\n"
    "  If you want to know who holds it, the kernel log says: journalctl -k -n 5";

// ---------------------------------------------------------------------------
// Descriptor checks
// ---------------------------------------------------------------------------
bool isUsbMux(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kUsbMuxClass && d.bInterfaceSubClass == kUsbMuxSubclass &&
           d.bInterfaceProtocol == kUsbMuxProtocol;
}

bool isNcmControl(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kNcmControlClass && d.bInterfaceSubClass == kNcmControlSubclass;
}

bool isNcmData(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kNcmDataClass;
}

bool isAppleEthernet(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kAppleEthClass && d.bInterfaceSubClass == kAppleEthSubclass &&
           d.bInterfaceProtocol == kAppleEthProtocol;
}

/// A CarPlay configuration = all three of USBMUX (ff/fe/02), **standard CDC-NCM** (02/0d control
/// + 0a data interface), and Apple-private Ethernet (ff/fd/01).
/// Do not identify it by "configuration number": the number varies by model (a real iPhone uses 6); find it by descriptors only.
/// Of the three, only the two NCM interfaces are the source of the "network interface" -- creating it is the job of kernel cdc_ncm, which this file does not touch.
struct CarPlayConfig {
    int index = -1;   ///< descriptor array index (for libusb_get_config_descriptor)
    int value = -1;   ///< bConfigurationValue (for set_configuration)
};

CarPlayConfig findCarPlayConfig(libusb_device* dev)
{
    CarPlayConfig found;
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return found;

    for (int i = 0; i < dd.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(i), &cfg) != 0) continue;
        bool mux = false, ncm = false, eth = false;
        for (int k = 0; k < cfg->bNumInterfaces; ++k) {
            for (int a = 0; a < cfg->interface[k].num_altsetting; ++a) {
                const libusb_interface_descriptor& d = cfg->interface[k].altsetting[a];
                if (isUsbMux(d)) mux = true;
                if (isNcmControl(d)) ncm = true;
                if (isAppleEthernet(d)) eth = true;
            }
        }
        const int value = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);

        if (mux && eth && ncm) {
            found.index = i;
            found.value = value;
            break;
        }
    }
    return found;
}

/// Dump the current configuration table verbatim. **Only called when "no CarPlay configuration" is found**
/// -- this log entry is the dividing line between "phone is locked" and "cable/PTP is in the way":
/// when an iPhone is locked, iOS reports only 1 configuration (PTP); after unlocking and trusting, all six appear.
/// Without it, two completely different situations would share the same "try another cable", wasting effort.
std::string describeConfigs(libusb_device* dev)
{
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return "(cannot even read the device descriptor)";

    std::string out = "it currently reports " + std::to_string(int(dd.bNumConfigurations)) + " configurations:";
    for (int i = 0; i < dd.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(i), &cfg) != 0) continue;
        out += "\n    [" + std::to_string(i) + "] value=" +
               std::to_string(int(cfg->bConfigurationValue)) + ", interfaces " +
               std::to_string(int(cfg->bNumInterfaces)) + ": ";
        for (int k = 0; k < cfg->bNumInterfaces; ++k) {
            // Only look at the main altsetting: alt 0 already shows what the interface does; dumping all would flood the log
            const libusb_interface_descriptor& d = cfg->interface[k].altsetting[0];
            char b[32] = "";
            std::snprintf(b, sizeof b, " %02x/%02x/%02x", d.bInterfaceClass, d.bInterfaceSubClass,
                          d.bInterfaceProtocol);
            out += b;
        }
        libusb_free_config_descriptor(cfg);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

/// Process-wide shared libusb context, **deliberately never libusb_exit'd**:
/// handed-out streams (IByteStream) live until the process exits, and exiting the context invalidates their handles too.
libusb_context* usbContext()
{
    static libusb_context* c = []() -> libusb_context* {
        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) != 0) {
            LOG_ERROR("[%s] libusb_init failed: is libusb-1.0 installed?", kTag);
            return nullptr;
        }
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
        return ctx;
    }();
    return c;
}

/// Owner of the device list: the raw pointer returned by pick() is valid only while this object lives.
struct DevList {
    libusb_device** list = nullptr;
    ssize_t n = 0;

    DevList() = default;
    DevList(const DevList&) = delete;
    DevList& operator=(const DevList&) = delete;
    ~DevList() { if (list) libusb_free_device_list(list, 1); }

    void refresh(libusb_context* ctx)
    {
        if (list) { libusb_free_device_list(list, 1); list = nullptr; }
        const ssize_t got = libusb_get_device_list(ctx, &list);
        if (got < 0) { list = nullptr; n = 0; return; }
        n = got;
    }

    /// Pick an iPhone. **Do not** pick by "has a CarPlay configuration" -- a phone that has never switched
    /// configuration does not have that table in its descriptors yet, so identify it by VID/PID first.
    libusb_device* pick(const WiredUsbOptions& opt) const
    {
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor != opt.vendorId) continue;
            if (opt.productId >= 0) {
                if (dd.idProduct != opt.productId) continue;
            } else if (dd.idProduct == kAppleKeyboardProduct || dd.idProduct == kAppleHubProduct) {
                continue;
            }
            return list[i];
        }
        return nullptr;
    }
};

std::string openErrorHint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS)
        return "cannot open device: insufficient permission. /dev/bus/usb/* is root-only by default -- run with sudo, or add a udev rule.";
    if (rc == LIBUSB_ERROR_NOT_FOUND)
        return "cannot open device: it just disappeared (most likely re-enumerating), retry later.";
    return std::string("cannot open device: ") + libusb_error_name(rc);
}

/// Kernel drivers have to move out of the way too. USBDEVFS_SETCONFIGURATION returns BUSY whenever
/// any interface under the current configuration is bound by a driver (kernel log wording:
/// `interface 2 claimed by ipheth while 'usb_probe' sets config #6`). ipheth binds to the Apple
/// Ethernet interface, so detach it first; after the configuration switch the kernel re-probes by itself, so no rebind is needed.
void detachKernelDrivers(libusb_device_handle* h)
{
    for (int i = 0; i < 32; ++i) {
        if (libusb_kernel_driver_active(h, i) != 1) continue;
        const int rc = libusb_detach_kernel_driver(h, i);
        LOG_INFO("[%s] kernel driver on interface %d -> detach: %s", kTag, i,
                 rc == 0 ? "success" : libusb_error_name(rc));
    }
}

// ---------------------------------------------------------------------------
// USBMUX bulk pipe -> core::IByteStream
// ---------------------------------------------------------------------------
// Same semantics as the BlueZ stream (BlueZRfcommStream in platform/posix/bt_bluez.cpp):
// read returns >0 for data / 0 for timeout / <0 for disconnect. But **ownership differs**: that stream is process-wide and never released,
// while this one is owned by the caller and destroyed when the session ends -- so here we honestly join the reader thread and close the
// handle ourselves instead of just detaching.
class LibUsbBulkStream : public core::IByteStream {
public:
    LibUsbBulkStream(libusb_device_handle* h, uint8_t epIn, uint8_t epOut)
        : handle_(h), epIn_(epIn), epOut_(epOut)
    {
        reader_ = std::thread([this] { readerLoop(); });
    }

    ~LibUsbBulkStream() override
    {
        markClosed();
        if (reader_.joinable()) reader_.join();
        if (handle_) { libusb_close(handle_); handle_ = nullptr; }
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [&] { return !queue_.empty() || closed_; }))
            return 0;                       // timeout
        if (queue_.empty()) return -1;      // closed and nothing buffered
        const size_t take = std::min(n, queue_.size());
        for (size_t i = 0; i < take; ++i) { buf[i] = queue_.front(); queue_.pop_front(); }
        return int(take);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        size_t off = 0;
        while (off < n) {
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle_, epOut_,
                                                const_cast<uint8_t*>(buf + off),
                                                int(n - off), &transferred, kBulkWriteTimeoutMs);
            if (rc != 0 || transferred <= 0) {
                LOG_WARN("[%s] bulk OUT failed: %s", kTag, libusb_error_name(rc));
                markClosed();
                return false;
            }
            off += size_t(transferred);
        }
        return true;
    }

    void close() override { markClosed(); }

private:
    void markClosed()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void readerLoop()
    {
        std::vector<uint8_t> buf(kBulkInSize);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (closed_) break;
            }
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle_, epIn_, buf.data(), kBulkInSize,
                                                &transferred, kBulkPollMs);
            if (rc == LIBUSB_ERROR_TIMEOUT) continue;   // idle, normal
            if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO ||
                rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_OTHER) {
                LOG_WARN("[%s] bulk IN disconnected: %s", kTag, libusb_error_name(rc));
                break;
            }
            if (rc != 0) continue;                      // transient error, retry next round
            if (transferred <= 0) continue;
            {
                std::lock_guard<std::mutex> lk(mu_);
                queue_.insert(queue_.end(), buf.begin(), buf.begin() + transferred);
            }
            cv_.notify_all();
        }
        markClosed();
    }

    libusb_device_handle* handle_;
    const uint8_t epIn_;
    const uint8_t epOut_;

    std::thread             reader_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<uint8_t>     queue_;
    bool                    closed_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public: switch into the CarPlay configuration -> hand out the USBMUX pipe
// ---------------------------------------------------------------------------
std::unique_ptr<core::IByteStream> openWiredCarPlay(const WiredUsbOptions& opt,
                                                    WiredUsbInfo* info,
                                                    std::string* err)
{
    auto fail = [&](const std::string& what) -> std::unique_ptr<core::IByteStream> {
        if (err) *err = what;
        LOG_ERROR("[%s] %s", kTag, what.c_str());
        return nullptr;
    };

    libusb_context* ctx = usbContext();
    if (!ctx) return fail("cannot create libusb context");

    // ---- 1. Find the iPhone ----
    DevList devs;
    devs.refresh(ctx);
    libusb_device* dev = devs.pick(opt);
    if (!dev) return fail("no iPhone found. Is it plugged in? Is it a data cable (charge-only cables won't work)?");

    libusb_device_descriptor dd{};
    libusb_get_device_descriptor(dev, &dd);

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(dev, &handle);
    if (rc != 0) return fail(openErrorHint(rc));

    std::string serial;
    if (dd.iSerialNumber) {
        unsigned char sbuf[256] = {0};
        if (libusb_get_string_descriptor_ascii(handle, dd.iSerialNumber, sbuf,
                                               sizeof(sbuf) - 1) > 0)
            serial = reinterpret_cast<char*>(sbuf);
    }

    // ---- 2/3. Get the device into the state "CarPlay configuration selected, handle still usable" ----
    //
    // **Re-enumeration is routine for this device**: a vendor request always triggers one, and
    // set_configuration sometimes does too. After re-enumeration the old handle is invalid at once, and the
    // first bulk transfer afterwards reports LIBUSB_ERROR_IO (internally "submiturb failed errno=2") -- a
    // subtle trap: every earlier step returned 0, and only real traffic blows up. So after each step we always
    // "find it again" rather than trying to detect whether re-enumeration happened (undetectable, and unnecessary).
    auto reacquire = [&](const char* why) -> bool {
        if (handle != nullptr) {
            libusb_close(handle);
            handle = nullptr;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(opt.switchWaitMs);
        int lastOpen = 0;
        for (;;) {
            devs.refresh(ctx);
            dev = devs.pick(opt);
            if (dev != nullptr) {
                lastOpen = libusb_open(dev, &handle);
                if (lastOpen == 0) return true;
                handle = nullptr;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                fail(std::string(why) + "did not see the iPhone come back (waited " +
                     std::to_string(opt.switchWaitMs) + " ms): " + openErrorHint(lastOpen));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    // Limit on loop iterations: re-enumeration needs another round, as does the configuration table not showing up yet.
    // cp is declared outside the loop -- step 4 still needs it to read descriptors.
    CarPlayConfig cp{};
    int           vendorTries = 0;
    for (int round = 0;; ++round) {
        if (round > 6) return fail("device keeps re-enumerating and never stabilizes");

        // ---- 2. No CarPlay configuration in the table -> send the vendor request to expose a different set ----
        cp = findCarPlayConfig(dev);
        if (cp.index < 0) {
            if (vendorTries++ < kCarPlayConfigTries) {
                uint8_t resp = 0;
                rc = libusb_control_transfer(handle, kVendorDeviceIn, kCarPlayConfigRequest, 0,
                                             kCarPlayConfigIndex, &resp, sizeof(resp), 1000);
                LOG_INFO("[%s] no CarPlay configuration, sending vendor 0x%02x / index %u: rc=%d (read 0x%02x)"
                         ", attempt %d/%d",
                         kTag, kCarPlayConfigRequest, kCarPlayConfigIndex, rc, resp, vendorTries,
                         kCarPlayConfigTries);
                if (rc != static_cast<int>(sizeof(resp)))
                    LOG_INFO("[%s]   note: a stall here is not a failure; re-enumeration happens anyway, so go by whether the device changed",
                             kTag);
                if (!reacquire("after switching the configuration table")) return nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;  // back to the top to re-check the configuration table
            }

            // Tried enough and still nothing -- dump the configuration table verbatim. **Only at this point can "phone locked" and
            // "wrong cable/port" be told apart**: reporting only 1 PTP configuration = the phone is locked; reporting several but
            // not all three parts = it really is time to change the cable. These two situations used to share one "try another cable", wasting effort.
            libusb_device_descriptor dd2{};
            libusb_get_device_descriptor(dev, &dd2);
            const std::string how =
                dd2.bNumConfigurations <= 1
                    ? "it reports only the PTP configuration -- exactly what a **locked iPhone** looks like (before unlocking, iOS"
                      " does not expose the CarPlay configurations). Unlock the phone, keep the screen on, and rerun;"
                      " the first time you must also tap 'Trust This Computer' on the prompt."
                    : "the configuration table has no 'USBMUX + standard CDC-NCM + Apple Ethernet' CarPlay"
                      " triple -- try a different genuine data cable, or a different USB port.";
            return fail("this iPhone is not exposing a CarPlay configuration right now.\n  " + how + "\n  " +
                        describeConfigs(dev));
        }

        // ---- 3. Select the CarPlay configuration ourselves (cannot be skipped; see file header) ----
        int active = -1;
        if (libusb_get_configuration(handle, &active) != 0 || active != cp.value) {
            detachKernelDrivers(handle);
            rc = libusb_set_configuration(handle, cp.value);
            if (rc != 0) {
                std::string msg = "set_configuration(" + std::to_string(cp.value) + ") failed: " +
                                  libusb_error_name(rc);
                if (rc == LIBUSB_ERROR_BUSY) msg += "\n  " + std::string(kBusyHint);
                libusb_close(handle);
                return fail(msg);
            }
            // Reacquire the handle; next round re-reads the configuration to confirm it really took effect (do not just trust the return value)
            if (!reacquire("after selecting the CarPlay configuration")) return nullptr;
            continue;
        }

        LOG_INFO("[%s] already on CarPlay configuration %d", kTag, cp.value);
        break;
    }

    // ---- 4. Identify each interface and claim USBMUX ----
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(cp.index), &cfg) != 0)
        return fail("cannot read the CarPlay configuration descriptor");
    struct CfgGuard {
        libusb_config_descriptor* c;
        ~CfgGuard() { libusb_free_config_descriptor(c); }
    } cfgGuard{cfg};

    const libusb_interface* mux = nullptr;
    int ncmControl = -1;
    int ncmData = -1;
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        for (int a = 0; a < cfg->interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg->interface[i].altsetting[a];
            if (isUsbMux(d) && !mux) mux = &cfg->interface[i];
            if (isNcmControl(d) && ncmControl < 0) ncmControl = d.bInterfaceNumber;
            if (isNcmData(d) && ncmData < 0) ncmData = d.bInterfaceNumber;
        }
    }
    if (!mux) {
        libusb_close(handle);
        return fail("no USBMUX interface found in the CarPlay configuration (ff/fe/02)");
    }

    // Endpoints are not hardcoded: find the bulk pair from descriptors. On the tested unit it is out=0x04 / in=0x85,
    // but those are values for "this unit", not the protocol.
    uint8_t epIn = 0;
    uint8_t epOut = 0;
    for (int a = 0; a < mux->num_altsetting; ++a) {
        const libusb_interface_descriptor& d = mux->altsetting[a];
        for (int e = 0; e < d.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = d.endpoint[e];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                if (!epIn) epIn = ep.bEndpointAddress;
            } else if (!epOut) {
                epOut = ep.bEndpointAddress;
            }
        }
    }
    const uint8_t muxNumber = mux->altsetting[0].bInterfaceNumber;
    if (!epIn || !epOut) {
        libusb_close(handle);
        return fail("no paired bulk endpoints on the USBMUX interface");
    }

    // If usbmuxd or similar still holds it, this returns BUSY -- the same class of holder as with set_configuration
    if (libusb_kernel_driver_active(handle, muxNumber) == 1)
        libusb_detach_kernel_driver(handle, muxNumber);
    rc = libusb_claim_interface(handle, muxNumber);
    if (rc != 0) {
        std::string msg = std::string("failed to claim USBMUX interface: ") + libusb_error_name(rc);
        if (rc == LIBUSB_ERROR_BUSY) msg += "\n  most likely held by usbmuxd.\n  " + std::string(kBusyHint);
        libusb_close(handle);
        return fail(msg);
    }

    if (info) {
        *info = WiredUsbInfo{};
        info->vendorId = dd.idVendor;
        info->productId = dd.idProduct;
        info->serial = serial;
        info->bus = libusb_get_bus_number(dev);
        info->address = libusb_get_device_address(dev);
        info->configValue = cp.value;
        info->usbMuxInterface = muxNumber;
        info->bulkIn = epIn;
        info->bulkOut = epOut;
        info->ncmControlInterface = static_cast<uint8_t>(std::max(ncmControl, 0));
        info->ncmDataInterface = static_cast<uint8_t>(std::max(ncmData, 0));
    }

    // This log line names who will create the network interface later (kernel cdc_ncm) -- it is the first thing to check when things go wrong:
    // if it says cdc_ncm here but `ip link` shows no interface, the CDC part did not bind.
    LOG_INFO("[%s] USBMUX ready: %u:%u serial=%s config=%d interface=%u endpoints out=0x%02x in=0x%02x"
             " (NCM control %d / data %d -- the network interface is left to the kernel cdc_ncm)",
             kTag, libusb_get_bus_number(dev), libusb_get_device_address(dev),
             serial.empty() ? "?" : serial.c_str(), cp.value, muxNumber, epOut, epIn,
             ncmControl, ncmData);

    return std::unique_ptr<core::IByteStream>(new LibUsbBulkStream(handle, epIn, epOut));
}

}  // namespace allplay::posix
