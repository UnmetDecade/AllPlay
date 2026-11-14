// ============================================================================
// tools/usb_probe.cpp -- USB-side probe for wired CarPlay (the first check on the whole path)
// ============================================================================
// The first gate on the wired path is "switching the configuration": when an iPhone is plugged in it presents an ordinary configuration, and **only once
// it is switched into the CarPlay configuration** do USBMUX(ff/fe/02) + CDC-NCM(02/0d) + Apple Ethernet(ff/fd/01)
// appear -- iAP2 runs over the USBMUX port, AirPlay over the NIC that NCM turns into.
//
// How to switch (following DiPlay / LIVI behavior): send the device a vendor request
//   bmRequestType=0xC0  bRequest=0x52  wValue=0  wIndex=4   read 1 byte
// On receipt the iPhone **re-enumerates**, and the interfaces above only appear in the new configuration. The config number varies by
// iPhone model, so it can't be hard-coded; find it by descriptor.
//
// **Pitfall (seen on real hardware)**: sending vendor 0x52 alone isn't enough. It only makes the iPhone re-enumerate with a new
// configuration table, but **after re-enumeration the host picks which one to activate** -- the Linux kernel picks "whichever config has a
// driver that can bind", so it lands on the generic configuration (PTP + USBMUX + Apple Ethernet, with USB tethering)
// rather than the CarPlay one. The symptom is very deceptive: `ip -br link` **does show an extra NIC**,
// but it's the one ipheth created for tethering; the media plane doesn't exist at all.
// So we must call `libusb_set_configuration()` ourselves to explicitly select the CarPlay config value
// (DiPlay also calls setConfiguration itself after open).
//
// This tool does three things only: enumerate, switch config + select config, then look at the descriptors again. It **doesn't touch iAP2 or the NCM data plane**.
// Its value is turning "can we?" from speculation into measurement, answering three questions:
//   ① Before/after switching, is the CarPlay config actually there? (If not -> the wired path is a dead end)
//   ② After explicitly selecting it, did the Linux kernel recognize the NCM NIC? (After running, check `ip -br link` and
//      `journalctl -k`; if it did, the AirPlay part gets the kernel's TCP/IP for free and we needn't write an NCM data plane)
//   ③ Can we get an IPv6 link-local address on this NIC (AirPlay needs to bind to it)
//
// Usage:
//   usb_probe --list                   list devices only (Apple ones expand configs/interfaces/endpoints)
//   usb_probe --carplay [--wait 10]    switch config -> explicitly select the CarPlay config (most common)
//   usb_probe --switch [--wait 10]     switch config only -> wait for re-enumeration -> list again (no selection)
//   usb_probe --setcfg 6               explicitly select a config only, without sending the vendor request
//   usb_probe --dump 1:5               dump only the given bus:addr
// Options:
//   --vid 05ac        vendor ID filter (default: Apple only)
//   --pid 12a8        product ID filter (default: any; keyboards/hubs are auto-excluded)
//   --any-apple       include keyboards/hubs (excluded by default to avoid noise)
//
// Permissions: on Linux /dev/bus/usb/* is root-only by default (755). Opening as non-root returns
// LIBUSB_ERROR_ACCESS -- run with sudo, or write a udev rule. This tool prints a hint.
//
// Another blocker: **usbmuxd**. udev starts it on every re-enumeration and it immediately grabs
// the USBMUX interface (sysfs shows driver=usbfs); then set_configuration returns BUSY.
// Before running, `sudo systemctl stop usbmuxd`; to make it permanent, `systemctl mask usbmuxd`.
//
// One more pitfall: switching config re-enumerates, **the old handle immediately goes stale**, and the device path changes too
// (different bus/addr). So after switching you must re-enumerate and find it again by VID/PID; never reuse the old handle.
#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Constants: Apple's interface fingerprint (matching DiPlay's IphoneCarPlayConfiguration)
// ---------------------------------------------------------------------------
constexpr uint16_t kAppleVendorId = 0x05ac;

// USBMUX: the carrier for iAP2. Note it's ff/fe/02, not the usual iAP interface.
constexpr uint8_t kUsbMuxClass = 0xff;
constexpr uint8_t kUsbMuxSubclass = 0xfe;
constexpr uint8_t kUsbMuxProtocol = 0x02;
// CDC-NCM control interface: what the kernel's cdc_ncm binds to.
constexpr uint8_t kNcmControlClass = 0x02;
constexpr uint8_t kNcmControlSubclass = 0x0d;
// Apple's proprietary Ethernet function: the third piece of the CarPlay config.
constexpr uint8_t kAppleEthernetClass = 0xff;
constexpr uint8_t kAppleEthernetSubclass = 0xfd;
constexpr uint8_t kAppleEthernetProtocol = 0x01;

// Vendor request to switch config (DiPlay's value, sourced from a fixed LIVI commit)
constexpr uint8_t kCarPlayConfigRequest = 0x52;
constexpr uint16_t kCarPlayConfigIndex = 0x0004;
constexpr uint8_t kVendorDeviceIn = 0xc0;

// These Apple devices aren't iPhones; listing them only muddies the picture (this user's setup is an Apple keyboard + its hub)
constexpr uint16_t kAppleKeyboardProduct = 0x024f;
constexpr uint16_t kAppleHubProduct = 0x1006;

struct Options {
    bool        list = false;      // --list
    bool        doSwitch = false;  // --switch
    bool        doCarPlay = false; // --carplay
    int         setCfg = -1;       // --setcfg N
    bool        anyApple = false;  // --any-apple
    std::string dump;              // --dump bus:addr
    int         waitSeconds = 10;  // --wait: seconds to wait for re-enumeration
    uint16_t    vid = kAppleVendorId;
    int         pid = -1;          // -1 = any
};

void printUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  usb_probe --carplay [--wait 10]    switch config → explicitly select the CarPlay config → report results (most common)\n"
        "  usb_probe --switch  [--wait 10]    only send vendor 0x52 to switch config → wait for re-enumeration → list again\n"
        "  usb_probe --setcfg 6               only explicitly select a config (no vendor request)\n"
        "  usb_probe --list                   list USB devices (expanded configs/interfaces/endpoints for Apple)\n"
        "  usb_probe --dump 1:5               only dump the given bus:addr\n"
        "Options:\n"
        "  --vid 05ac     vendor ID filter (default Apple)\n"
        "  --pid 12a8     product ID filter (default any)\n"
        "  --any-apple    also list keyboards/hubs\n"
        "Before running, `sudo systemctl stop usbmuxd`; otherwise it holds the USBMUX interface and setcfg returns BUSY.\n");
}

// ---------------------------------------------------------------------------
// Descriptor printing: format matches DiPlay's describe() so the two can be compared side by side
//   "interface/alt-setting:class.subclass.protocol x endpoints"
// ---------------------------------------------------------------------------
struct ConfigSummary {
    int     id = -1;
    bool    hasUsbMux = false;
    bool    hasCdcNcm = false;
    bool    hasAppleEthernet = false;
    uint8_t usbMuxInterface = 0;
    /// CarPlay config = USBMUX + standard CDC-NCM + Apple private Ethernet, all three present.
    /// Criterion matches findCarPlayConfig in platform/posix/usb_libusb.cpp.
    bool    carPlay() const { return hasUsbMux && hasCdcNcm && hasAppleEthernet; }
};

std::string interfaceLine(const libusb_interface_descriptor& d) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%u/%u:%02x.%02x.%02xx%u",
                  d.bInterfaceNumber, d.bAlternateSetting,
                  d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol,
                  d.bNumEndpoints);
    return buf;
}

std::string endpointLine(const libusb_endpoint_descriptor& e) {
    const char* dir = (e.bEndpointAddress & LIBUSB_ENDPOINT_IN) ? "IN " : "OUT";
    const char* type = "?";
    switch (e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     type = "control"; break;
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: type = "isoc"; break;
        case LIBUSB_TRANSFER_TYPE_BULK:        type = "bulk"; break;
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   type = "int"; break;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "      ep 0x%02x %s %s maxPacket=%u",
                  e.bEndpointAddress, dir, type, e.wMaxPacketSize);
    return buf;
}

ConfigSummary summarize(const libusb_config_descriptor& cfg) {
    ConfigSummary s;
    s.id = cfg.bConfigurationValue;
    for (int i = 0; i < cfg.bNumInterfaces; ++i) {
        for (int a = 0; a < cfg.interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg.interface[i].altsetting[a];
            if (d.bInterfaceClass == kUsbMuxClass && d.bInterfaceSubClass == kUsbMuxSubclass &&
                d.bInterfaceProtocol == kUsbMuxProtocol) {
                s.hasUsbMux = true;
                s.usbMuxInterface = d.bInterfaceNumber;
            }
            if (d.bInterfaceClass == kNcmControlClass && d.bInterfaceSubClass == kNcmControlSubclass) {
                s.hasCdcNcm = true;
            }
            if (d.bInterfaceClass == kAppleEthernetClass &&
                d.bInterfaceSubClass == kAppleEthernetSubclass &&
                d.bInterfaceProtocol == kAppleEthernetProtocol) {
                s.hasAppleEthernet = true;
            }
        }
    }
    return s;
}

void printConfig(const libusb_config_descriptor& cfg) {
    const ConfigSummary s = summarize(cfg);
    std::printf("    config %d: %d interfaces  [usbmux=%s cdc-ncm=%s apple-eth=%s]%s\n",
                cfg.bConfigurationValue, cfg.bNumInterfaces,
                s.hasUsbMux ? "yes" : "no", s.hasCdcNcm ? "yes" : "no",
                s.hasAppleEthernet ? "yes" : "no",
                s.carPlay() ? "   <<< CarPlay config (kernel cdc_ncm creates the NIC)" : "");
    for (int i = 0; i < cfg.bNumInterfaces; ++i) {
        for (int a = 0; a < cfg.interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg.interface[i].altsetting[a];
            std::printf("      %s\n", interfaceLine(d).c_str());
            for (int e = 0; e < d.bNumEndpoints; ++e) {
                std::printf("%s\n", endpointLine(d.endpoint[e]).c_str());
            }
        }
    }
}

// Open the device and read **all** configurations. An iPhone declares several at once, only one of which is CarPlay.
bool describeDevice(libusb_device* dev, bool verbose) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return false;

    const uint8_t bus = libusb_get_bus_number(dev);
    const uint8_t addr = libusb_get_device_address(dev);
    std::printf("  %u:%u  %04x:%04x  %d configs\n", bus, addr, dd.idVendor, dd.idProduct,
                dd.bNumConfigurations);
    if (!verbose) return true;

    // All configs are readable straight from the device descriptor (no open needed); reading descriptors needs no permission.
    bool anyCarPlay = false;
    for (int c = 0; c < dd.bNumConfigurations; ++c) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(c), &cfg) != 0) continue;
        printConfig(*cfg);
        if (summarize(*cfg).carPlay()) anyCarPlay = true;
        libusb_free_config_descriptor(cfg);
    }
    if (dd.bNumConfigurations == 0) {
        std::printf("    (cannot read config descriptors)\n");
    } else if (!anyCarPlay) {
        std::printf("    → no CarPlay config (USBMUX+NCM+Apple Ethernet triple incomplete)\n");
    }
    return true;
}

bool isCandidate(const libusb_device_descriptor& dd, const Options& opt) {
    if (opt.vid != 0 && dd.idVendor != opt.vid) return false;
    if (opt.pid >= 0 && dd.idProduct != static_cast<uint16_t>(opt.pid)) return false;
    if (!opt.anyApple && dd.idVendor == kAppleVendorId &&
        (dd.idProduct == kAppleKeyboardProduct || dd.idProduct == kAppleHubProduct)) {
        return false;
    }
    return true;
}

std::vector<libusb_device*> collect(libusb_context* ctx, const Options& opt) {
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    std::vector<libusb_device*> out;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (!isCandidate(dd, opt)) continue;
        out.push_back(libusb_ref_device(list[i]));  // take our own reference so the list can be freed
    }
    libusb_free_device_list(list, 1);
    return out;
}

void releaseAll(std::vector<libusb_device*>& devs) {
    for (libusb_device* d : devs) libusb_unref_device(d);
    devs.clear();
}

// "1:5" → bus=1 addr=5
bool parseBusAddr(const std::string& s, uint8_t* bus, uint8_t* addr) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    *bus = static_cast<uint8_t>(std::atoi(s.substr(0, colon).c_str()));
    *addr = static_cast<uint8_t>(std::atoi(s.substr(colon + 1).c_str()));
    return true;
}

std::string accessHint(int rc) {
    if (rc == LIBUSB_ERROR_ACCESS) {
        return "  ← permission denied: /dev/bus/usb is root-only by default; rerun with sudo or add a udev rule";
    }
    if (rc == LIBUSB_ERROR_NOT_FOUND) {
        return "  ← device has disappeared (probably just re-enumerated)";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Switch config: send the vendor request -> wait for re-enumeration
// ---------------------------------------------------------------------------

// Returns the CarPlay config's bConfigurationValue (the one with USBMUX+NCM+Apple Ethernet all present).
// The config number differs across models, so find it by descriptor rather than hard-coding; return -1 if none.
int findCarPlayConfig(libusb_device* dev) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return -1;
    for (int c = 0; c < dd.bNumConfigurations; ++c) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(c), &cfg) != 0) continue;
        const bool ok = summarize(*cfg).carPlay();
        const int value = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);
        if (ok) return value;
    }
    return -1;
}

void describeAll(libusb_context* ctx, const Options& opt, const char* title) {
    std::printf("%s\n", title);
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) std::printf("  (none -- the device hasn't come back yet; wait a moment and rerun)\n");
    for (libusb_device* d : devs) describeDevice(d, true);
    releaseAll(devs);
}

// Send vendor 0x52 to make the iPhone re-enumerate. Returns true = re-enumeration actually observed.
// **It only makes the phone switch to a new configuration table; it doesn't select which one** -- see the comment above setActiveConfig.
bool requestCarPlayConfig(libusb_context* ctx, const Options& opt) {
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("no matching device found (vid=%04x pid=%s). Is the iPhone plugged in? Is the cable actually a data cable?\n",
                    opt.vid, opt.pid < 0 ? "any" : "specified");
        return false;
    }

    libusb_device* target = devs.front();
    libusb_device_descriptor dd{};
    libusb_get_device_descriptor(target, &dd);
    const uint8_t bus = libusb_get_bus_number(target);
    const uint8_t addr = libusb_get_device_address(target);
    std::printf("target: %u:%u  %04x:%04x\n", bus, addr, dd.idVendor, dd.idProduct);

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    if (rc != 0) {
        std::printf("cannot open device: %s%s\n", libusb_error_name(rc), accessHint(rc).c_str());
        releaseAll(devs);
        return false;
    }
    releaseAll(devs);  // the handle holds its own reference; the device list can be freed

    uint8_t response = 0;
    rc = libusb_control_transfer(handle, kVendorDeviceIn, kCarPlayConfigRequest, 0,
                                 kCarPlayConfigIndex, &response, sizeof(response), 1000);
    std::printf("\nsending vendor 0x%02x / index %u: rc=%d", kCarPlayConfigRequest, kCarPlayConfigIndex, rc);
    if (rc == static_cast<int>(sizeof(response))) {
        std::printf(" (read 0x%02x)\n", response);
    } else {
        std::printf("%s\n", accessHint(rc).c_str());
        std::printf("  Note: some firmware stalls here (LIBUSB_ERROR_IO/PIPE), but **re-enumeration still happens**; "
                    "go by whether the device changed.\n");
    }
    libusb_close(handle);

    // Wait for re-enumeration: old (bus,addr) gone + a new device with the same VID/PID seen; only then is the switch considered successful.
    std::printf("\nwaiting for re-enumeration (up to %d s)...\n", opt.waitSeconds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.waitSeconds);
    bool oldGone = false;
    bool freshSeen = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<libusb_device*> now = collect(ctx, opt);
        bool sawOld = false;
        bool sawNew = false;
        for (libusb_device* d : now) {
            if (libusb_get_bus_number(d) == bus && libusb_get_device_address(d) == addr) sawOld = true;
            else sawNew = true;
        }
        oldGone = oldGone || !sawOld;
        freshSeen = freshSeen || sawNew;
        releaseAll(now);
        if (oldGone && freshSeen) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return oldGone && freshSeen;
}

// Explicitly select a configuration value.
// Why this is mandatory: vendor 0x52 only makes the iPhone re-enumerate; **which config is activated after re-enumeration is up to the host**.
// The Linux kernel picks "whichever config has an interface a driver can bind", so it picks the generic config with USB tethering,
// and the CarPlay config is never activated -- the kernel only builds a tethering NIC via ipheth to fool you.
// DiPlay likewise calls setConfiguration itself.
int setActiveConfig(libusb_context* ctx, const Options& opt, int value) {
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("no device found, cannot select a config\n");
        return 1;
    }
    libusb_device* target = devs.front();
    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    if (rc != 0) {
        std::printf("cannot open device: %s%s\n", libusb_error_name(rc), accessHint(rc).c_str());
        releaseAll(devs);
        return 1;
    }
    releaseAll(devs);

    // First make the kernel drivers step aside. USBDEVFS_SETCONFIGURATION returns BUSY as long as **any interface in the current config** is claimed,
    // regardless of who claims it. Measured on this machine there are two claimants:
    //   - ipheth (kernel driver, bound to the Apple Ethernet interface) -> detached by this function; it's only USB tethering,
    //     and the kernel re-probes after switching, so no need to care.
    //   - usbmuxd / gvfsd-gphoto2 (userspace processes) -> we can't detach them; the user must stop them first;
    //     the kernel log names them. That's where the hint below comes from.
    int detached[32];
    int nDetached = 0;
    for (int i = 0; i < 32 && nDetached < 32; ++i) {
        if (libusb_kernel_driver_active(handle, i) != 1) continue;
        const int d = libusb_detach_kernel_driver(handle, i);
        std::printf("  interface %d has a kernel driver → detach: %s\n", i, d == 0 ? "success" : libusb_error_name(d));
        if (d == 0) detached[nDetached++] = i;
    }

    rc = libusb_set_configuration(handle, value);
    if (rc != 0) {
        std::printf("set_configuration(%d) failed: %s\n", value, libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_BUSY) {
            std::printf("  ← an interface in the current config is claimed. This tool already detached kernel drivers, "
                        "so the remaining claimants are userspace processes:\n"
                        "     sudo systemctl stop usbmuxd      # udev restarts it on every re-enumeration; "
                        "to make it stick, systemctl mask usbmuxd\n"
                        "     pkill -f gvfsd-gphoto2           # GNOME's photo-import daemon, "
                        "which holds the iPhone's PTP interface\n"
                        "  The kernel log says who is holding it: journalctl -k -n 5\n");
        }
        for (int i = 0; i < nDetached; ++i) libusb_attach_kernel_driver(handle, detached[i]);
        libusb_close(handle);
        return 1;
    }
    int active = -1;
    libusb_get_configuration(handle, &active);  // read back to confirm, don't just trust the return value
    std::printf("set_configuration(%d) succeeded, active config = %d\n", value, active);
    libusb_close(handle);
    return 0;
}

int doSwitch(libusb_context* ctx, const Options& opt) {
    describeAll(ctx, opt, "before switching: ");
    const bool ok = requestCarPlayConfig(ctx, opt);
    std::printf("  %s\n", ok ? "device re-enumerated ✓" : "no re-enumeration observed (switch may have failed)");
    describeAll(ctx, opt, "\nafter switching: ");

    std::printf("\nNote: re-enumeration only means the phone switched to a different config table; **which one is active is still up to you to select** (use --carplay).\n"
                "Then check whether the kernel recognized the NCM NIC:\n"
                "  ip -br link; ip -br addr; lsusb -t\n"
                "  journalctl -k -n 30 | grep -E 'ipheth|cdc_ncm|usb 1-'\n");
    return ok ? 0 : 1;
}

// --carplay: answer "can we get into the CarPlay config" end to end in one go
int doCarPlay(libusb_context* ctx, const Options& opt) {
    describeAll(ctx, opt, "before switching: ");
    const bool ok = requestCarPlayConfig(ctx, opt);
    std::printf("  %s\n", ok ? "device re-enumerated ✓" : "no re-enumeration observed (switch may have failed)");

    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("\nthe device hasn't come back; wait a moment and run again.\n");
        return 1;
    }
    const int cp = findCarPlayConfig(devs.front());
    releaseAll(devs);
    if (cp < 0) {
        std::printf("\nthis iPhone has no CarPlay config (the USBMUX + CDC-NCM + Apple Ethernet triple)\n"
                    "→ the wired path won't work.\n");
        describeAll(ctx, opt, "\nactual descriptors: ");
        return 1;
    }
    std::printf("\nCarPlay config = %d (found by descriptor, don't hard-code)\n", cp);

    if (setActiveConfig(ctx, opt, cp) != 0) return 1;

    describeAll(ctx, opt, "\nafter selecting: ");
    std::printf("\nThere should now be two kinds of NICs: the ipheth one is for USB tethering (still present, harmless); "
                "the one cdc_ncm creates for CarPlay is the media path:\n"
                "  ip -br link; lsusb -t; journalctl -k -n 30\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s must be followed by a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--list"))            opt.list = true;
        else if (!std::strcmp(a, "--switch"))     opt.doSwitch = true;
        else if (!std::strcmp(a, "--carplay"))    opt.doCarPlay = true;
        else if (!std::strcmp(a, "--setcfg"))     opt.setCfg = std::atoi(next("--setcfg"));
        else if (!std::strcmp(a, "--any-apple"))  opt.anyApple = true;
        else if (!std::strcmp(a, "--dump"))       opt.dump = next("--dump");
        else if (!std::strcmp(a, "--wait"))       opt.waitSeconds = std::atoi(next("--wait"));
        else if (!std::strcmp(a, "--vid"))        opt.vid = static_cast<uint16_t>(std::strtoul(next("--vid"), nullptr, 16));
        else if (!std::strcmp(a, "--pid"))        opt.pid = static_cast<int>(std::strtoul(next("--pid"), nullptr, 16));
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { printUsage(); return 0; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            printUsage();
            return 2;
        }
    }
    if (!opt.list && !opt.doSwitch && !opt.doCarPlay && opt.setCfg < 0 && opt.dump.empty()) {
        printUsage();
        return 2;
    }

    libusb_context* ctx = nullptr;
    const int rc = libusb_init(&ctx);
    if (rc != 0) {
        std::fprintf(stderr, "libusb_init failed: %s\n", libusb_error_name(rc));
        return 1;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    int ret = 0;
    if (opt.doCarPlay) {
        ret = doCarPlay(ctx, opt);
    } else if (opt.doSwitch) {
        ret = doSwitch(ctx, opt);
    } else if (opt.setCfg >= 0) {
        ret = setActiveConfig(ctx, opt, opt.setCfg);
    } else {
        std::vector<libusb_device*> devs = collect(ctx, opt);
        if (!opt.dump.empty()) {
            uint8_t bus = 0;
            uint8_t addr = 0;
            if (!parseBusAddr(opt.dump, &bus, &addr)) {
                std::fprintf(stderr, "--dump must be written as bus:addr, e.g. 1:5\n");
                releaseAll(devs);
                libusb_exit(ctx);
                return 2;
            }
            bool found = false;
            for (libusb_device* d : devs) {
                if (libusb_get_bus_number(d) == bus && libusb_get_device_address(d) == addr) {
                    describeDevice(d, true);
                    found = true;
                }
            }
            if (!found) std::printf("no device at %u:%u (note that the address changes after switching config)\n", bus, addr);
        } else {
            std::printf("matched %zu devices:\n", devs.size());
            for (libusb_device* d : devs) describeDevice(d, true);
        }
        releaseAll(devs);
    }

    libusb_exit(ctx);
    return ret;
}
