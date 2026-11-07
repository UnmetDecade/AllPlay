// ============================================================================
// tools/wired_probe.cpp —— 有线 CarPlay 控制面探针（切配置之后的那一段）
// ============================================================================
// tools/usb_probe.cpp 验的是"能不能切进 CarPlay 配置"（USB 侧，Stage 0）。
// 这一支验的是**切进去之后能不能说话**：
//
//   USBMUX 口(bulk) → usbmux v2 握手 → 迷你 TCP → 手机:62078(lockdown)
//                                             → 发一条 QueryType → 读回 plist
//
// 为什么单立一支：这一段（usbmux v2 + 迷你 TCP + lockdown plist 通道）是**唯一
// 没法靠读源码确认**的部分 —— 帧长、序号、端口复用全是字节级约定，差一个字节就
// 卡在等应答上，而且卡住的表象（超时）和"设备没插"一模一样。所以先把它单独打通，
// 后面写 iAP2 时才不会把两类问题混在一起查。
//
// 用法：
//   wired_probe                打开链路 → usbmux 握手 → 连 62078 → 打印手机的反应
//   wired_probe --query        连上后再发一条 lockdown QueryType，把回话原样打出来
//   wired_probe --pair         跑完整控制面：配对 → 会话 → TLS → carkit 服务，
//                              然后把手机在 iAP2 流上发来的字节 dump 出来
//   wired_probe --link         配合 --pair：拿到 carkit 服务流后，跑**真正的** iAP2 链路协商
//                              （marker + SYN，主机主动发起，见 core/iap2/wired_link.h）
//   wired_probe --marker       配合 --pair：只发那 6 字节 iAP2 起始标记（FF 55 02 00 EE 10），
//                              然后干等。**这是最早的naive 做法，真机实测手机一声不吭** ——
//                              留着是因为它正好反证"光发 marker 不够，还得自己发 SYN"
//   wired_probe --port N       换端口（默认 62078）
//   wired_probe --hold         通了之后停住等回车（方便另开一个终端看 ip -br link）
//   wired_probe --stay         通了之后一直读，把手机后续发来的字节 dump 出来
//
// 跑之前（Linux 桌面，**三件都必须做**，否则一到 set_configuration 就 BUSY）：
//   sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd
//   pkill -f gvfsd-gphoto2
//   （第三件 —— 内核驱动 ipheth —— 本程序会自己 detach，不用管）
// 另外 /dev/bus/usb/* 是 root 独占的，**整个程序要 sudo 跑**。
//
// macOS 上**什么都不用停**：控制面走系统 usbmuxd（就是 Finder 用的那条隧道），
// 媒体面网卡由 AppleUSBNCMData 建好，程序不用 sudo。前提是这台 Mac 在 Finder 里
// 被手机信任过。对应地，--pid / --wait 这两个"切配置"选项在 macOS 上不起作用。
#include "airplay/info.h"
#include "airplay/server.h"
#include "iap2/wired_link.h"
#include "mfi/mfi_identity.h"
#include "platform/device_identity.h"
#include "posix/posix.h"
#include "posix/usb.h"
#include "usb/usbmux.h"
#include "usb/wired_control.h"

#include "common/log.h"

#include "port/media.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/// tools/wired_selftest.cpp：不需要手机的离线自检
int wiredSelfTest();

namespace {

/// 只点数、不上屏的视频输出。
/// 探针这一支要证明的是"**码流真的从手机过来了**"，不是"画面好不好看" ——
/// 解码/上屏是宿主（host/media/sdl_video_sink）的事，这儿接了反而要拖进 SDL。
/// 不接 IVideoOutput 的后果很具体：SETUP 里手机要屏幕流，我们不给 dataPort，
/// 手机会一直 POST /feedback 干等（session.cpp:379 那条 WARN 就是它）。
class CountingVideoSink : public core::IVideoOutput {
public:
    void configure(core::VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& codecData) override
    {
        log("视频配置：%s %dx%d，codecData %zu 字节",
            codec == core::VideoCodec::HEVC ? "HEVC" : "H.264", width, height, codecData.size());
    }

    bool feedAnnexB(const uint8_t* data, size_t len) override
    {
        (void)data;   // 只点数不解码，指针本身用不上（真解码在 host/media）
        ++frames_;
        bytes_ += len;
        if (frames_ == 1 || frames_ % 60 == 0) {
            // 首帧立刻报，之后每 60 帧报一次 —— 每一帧都打会把控制台的 iAP2 日志淹掉
            log("收到第 %llu 帧（本帧 %zu 字节，累计 %llu）", frames_, len, bytes_);
        }
        return true;
    }

    void reset() override { log("视频输出被 reset（丢关键帧后的恢复路径）"); }
    void flush() override {}

private:
    static void log(const char* fmt, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        std::printf("  ★ %s\n", buf);
    }

    unsigned long long frames_ = 0;
    unsigned long long bytes_  = 0;
};

}  // namespace

namespace {

constexpr uint16_t kLockdownPort = 62078;

// lockdown 的 QueryType 请求。**注意是 XML plist，不是本项目 core/bplist 那套二进制
// plist** —— lockdown 这条通道只吃 XML（DiPlay 也是这么发的）。
const char* kQueryType =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key><string>allplay.wired_probe</string>\n"
    "  <key>Request</key><string>QueryType</string>\n"
    "</dict>\n"
    "</plist>\n";

/// 把 XML 原样发出去，前面加 4 字节大端长度（lockdown 的成帧方式）
bool sendLockdown(core::IByteStream& ch, const std::string& xml)
{
    std::vector<uint8_t> buf(4 + xml.size());
    const uint32_t n = uint32_t(xml.size());
    buf[0] = uint8_t(n >> 24);
    buf[1] = uint8_t(n >> 16);
    buf[2] = uint8_t(n >> 8);
    buf[3] = uint8_t(n);
    std::memcpy(buf.data() + 4, xml.data(), xml.size());
    return ch.writeAll(buf.data(), buf.size());
}

/// 读一条 lockdown 消息（4 字节大端长度 + 内容）。超时/断开发返回空串。
std::string recvLockdown(core::IByteStream& ch, int timeoutMs)
{
    uint8_t hdr[4];
    size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < 4) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) return {};
        const int n = ch.read(hdr + got, 4 - got, int(left));
        if (n < 0) return {};
        if (n == 0) continue;
        got += size_t(n);
    }
    const uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) |
                         (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    if (len == 0 || len > 4 * 1024 * 1024) {
        std::printf("  ✗ 长度字段不合理：%u\n", len);
        return {};
    }

    std::string out(len, '\0');
    size_t off = 0;
    while (off < len) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) break;
        const int n = ch.read(reinterpret_cast<uint8_t*>(&out[off]), len - off, int(left));
        if (n < 0) break;
        if (n == 0) continue;
        off += size_t(n);
    }
    out.resize(off);
    return out;
}

void usage()
{
    std::printf(
        "用法：\n"
        "  wired_probe                打开链路 → usbmux 握手 → 连 62078 → 看手机反应\n"
        "  wired_probe --query        连上后发一条 lockdown QueryType，打印回话\n"
        "  wired_probe --pair         跑完整控制面（配对→会话→TLS→carkit 服务）\n"
        "  wired_probe --link         跑完整控制面：链路协商 → 识别 → MFi → 电源/订阅\n"
        "                             → 等 0x4300 → 回 0x4301（要带 NCM 网卡的 IPv6）\n"
        "  wired_probe --mfi-dir D    配合 --link：MFi 身份目录（identity.pk8 + certificate.p7b）\n"
        "  wired_probe --state-dir D  配对材料存哪（默认 defaultStateDir()；不存就每次都弹信任框）\n"
        "  wired_probe --airplay-port N  0x4301 报给手机的 AirPlay 端口（默认 7000）\n"
        "  wired_probe --ncm-link-local A  钉在 NCM 网卡上的 IPv6 链路本地地址\n"
        "                             （默认 fe80::2，同 DiPlay 的 config.linkLocal）\n"
        "  wired_probe --marker       配合 --pair：只发 6 字节 iAP2 起始标记（手机不会理）\n"
        "  wired_probe --port N       换端口（默认 62078）\n"
        "  wired_probe --hold         通了之后停住等回车\n"
        "  wired_probe --stay         通了之后一直读，把后续字节 dump 出来\n"
        "  wired_probe --pid 12a8     限定产品号\n"
        "  wired_probe --selftest     离线自检（plist / 证书 / TLS 回环），不用插手机、不用 sudo\n");
}

}  // namespace

int main(int argc, char** argv)
{
    core::logInitFromEnv();
    // stdout 走管道时默认是全缓冲，被 timeout/SIGKILL 打断就整段丢掉 ——
    // 这几支探针全是"跑到一半被杀"，丢掉的那一段恰恰是最想看的。改成行缓冲。
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // 离线自检不碰 USB，所以在解析参数之前先拦下来 —— 也不需要 sudo
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) return wiredSelfTest();
    }

    allplay::posix::WiredUsbOptions opt;
    uint16_t port = kLockdownPort;
    uint16_t airPlayPort = 7000;   ///< 0x4301 报给手机的 AirPlay 端口
    std::string ncmLinkLocal;      ///< 钉在 NCM 网卡上的链路本地地址（空 = 默认 fe80::2）
    bool     doQuery = false;
    bool     doHold = false;
    bool     doStay = false;
    bool     doPair = false;
    bool     doMarker = false;
    bool     doLink = false;
    std::string mfiDir;
    std::string stateDir;          ///< 配对材料存哪（空 = defaultStateDir()）

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--query") == 0) doQuery = true;
        else if (std::strcmp(a, "--hold") == 0) doHold = true;
        else if (std::strcmp(a, "--stay") == 0) doStay = true;
        else if (std::strcmp(a, "--pair") == 0) doPair = true;
        else if (std::strcmp(a, "--marker") == 0) doMarker = true;
        else if (std::strcmp(a, "--link") == 0) { doLink = true; doPair = true; }
        else if (std::strcmp(a, "--mfi-dir") == 0 && i + 1 < argc) mfiDir = argv[++i];
        else if (std::strcmp(a, "--state-dir") == 0 && i + 1 < argc) stateDir = argv[++i];
        else if (std::strcmp(a, "--port") == 0 && i + 1 < argc) port = uint16_t(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--airplay-port") == 0 && i + 1 < argc)
            airPlayPort = uint16_t(std::atoi(argv[++i]));
        else if (std::strcmp(a, "--ncm-link-local") == 0 && i + 1 < argc) ncmLinkLocal = argv[++i];
        else if (std::strcmp(a, "--pid") == 0 && i + 1 < argc) opt.productId = std::atoi(argv[++i]);
        else if (std::strcmp(a, "--wait") == 0 && i + 1 < argc) opt.switchWaitMs = std::atoi(argv[++i]) * 1000;
        else { usage(); return 2; }
    }

    // ---- ① 打开有线链路 ----
    std::printf("=== ① 打开有线链路 ===\n");
    allplay::posix::WiredUsbInfo info;
    std::string err;
#ifdef __APPLE__
    (void)opt;   // macOS 不枚举 USB，产品号/切配置等待都没有意义
    // macOS 不切配置、不抢接口：控制面借系统 usbmuxd 的隧道（跟 Finder/Xcode 用的是
    // 同一条），媒体面网卡由 AppleUSBNCMData 自己建好。前提：这台 Mac 被手机信任过。
    std::unique_ptr<core::usb::IMux> mux = allplay::posix::openSystemUsbMuxd(&info, &err);
    if (!mux) {
        std::printf("✗ 打开失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ 序列号 %s（系统 usbmuxd）\n", info.serial.c_str());
    std::printf("  ✓ USBMUX 接口 %u（iAP2 的 USBHostTransport 号；NCM 网卡归 AppleUSBNCMData）\n",
                info.usbMuxInterface);
#else
    auto pipe = allplay::posix::openWiredCarPlay(opt, &info, &err);
    if (!pipe) {
        std::printf("✗ 打开失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ 序列号 %s  bus %u addr %u  配置 %d\n", info.serial.c_str(), info.bus,
                info.address, info.configValue);
    // 网卡那摊归内核 cdc_ncm，这里只报它要绑的那两个口
    std::printf("  ✓ USBMUX 接口 %u，bulk OUT 0x%02x / IN 0x%02x；NCM 控制口 %u 数据口 %u"
                "（建网卡的是内核 cdc_ncm）\n",
                info.usbMuxInterface, info.bulkOut, info.bulkIn, info.ncmControlInterface,
                info.ncmDataInterface);
#endif

    // ---- ② 另一条路：整条控制面（--pair）----
    if (doPair) {
        std::printf("\n=== ② 控制面：配对 → 会话 → TLS → carkit 服务 ===\n");
#ifdef __APPLE__
        core::usb::WiredControl wc(std::move(mux));
#else
        core::usb::WiredControl wc(*pipe);
#endif
        // 配对材料按手机序列号存下来复用 —— 不存的话每轮都换一套 HostID/证书，
        // 手机每次都当陌生人，**每次都弹"要信任此电脑吗？"**。
        wc.setPairStore(info.serial, stateDir);
        if (!wc.connect(&err)) {
            std::printf("✗ 控制面没跑通：%s\n", err.c_str());
            return 1;
        }
        core::IByteStream* svc = wc.service();
        std::printf("  ✓ iAP2 流到手（HostID=%s）\n", wc.hostId().c_str());

        if (doMarker) {
            // 链路层是**主机发起**的：不发这 6 个字节，手机在服务流上一直沉默。
            // 注意它前面**没有**任何长度前缀或封装，就是裸 6 字节。
            static const uint8_t kInit[6] = {0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
            if (!svc->writeAll(kInit, sizeof kInit)) {
                std::printf("✗ 起始标记没发出去\n");
                return 1;
            }
            std::printf("  ✓ 已发 iAP2 起始标记 FF 55 02 00 EE 10\n");
        }

        // ---- ③ 链路协商（--link）：marker + SYN，主机主动发起 ----
        if (doLink) {
            std::printf("\n=== ③ iAP2 链路协商（主机发起）===\n");
            core::iap2::WiredLink link(*svc);
            std::string            lerr;
            if (!link.negotiate(30000, &lerr)) {
                std::printf("✗ 链路没协商起来：%s\n", lerr.c_str());
                std::printf("  对端参数：%s\n", link.engine().peerSummary().c_str());
                return 1;
            }
            std::printf("  ✓ 链路 NORMAL\n");
            std::printf("  ✓ 对端参数：%s\n", link.engine().peerSummary().c_str());
            std::printf("  ✓ 对端可承载的控制载荷上限：%zu 字节\n", link.engine().peerMaxPayload());

            std::printf("\n=== ④ 识别（0x1D00 → 0x1D01 → 0x1D02）===\n");
            core::iap2::WiredConfig wcfg;
            // USBHostTransport 那一组里要填的是**手机那侧 CarPlay 用的 USB 接口号**，
            // 也就是我们从描述符里认出来的那个 USBMUX 口的号 —— 别写死。
            wcfg.carPlayUsbInterfaceNumber = int(info.usbMuxInterface);
            if (!link.identify(wcfg, &lerr)) {
                std::printf("✗ 识别没通过：%s\n", lerr.c_str());
                return 1;
            }
            std::printf("  ✓ 识别通过（USBHostTransport 接口号 %d）\n", wcfg.carPlayUsbInterfaceNumber);

            std::printf("\n=== ⑤ MFi 认证（0xAA00 → 0xAA01 → 0xAA02 → 0xAA03 → 0xAA05）===\n");
            std::unique_ptr<core::mfi::CertificateMfiIdentity> mfi;
            if (!mfiDir.empty()) {
                mfi = core::mfi::CertificateMfiIdentity::load(mfiDir, &err);
                if (!mfi) {
                    std::printf("✗ 装不上 MFi 身份（%s）：%s\n", mfiDir.c_str(), err.c_str());
                    return 1;
                }
                std::printf("  ✓ MFi 身份装好了（证书 %zu 字节，来自 %s）\n", mfi->certificate().size(),
                            mfiDir.c_str());
            } else {
                std::printf("  · 没给 --mfi-dir，发空证书（真机必失败）\n");
            }
            if (!link.authenticate(mfi.get(), 30000, &lerr)) {
                std::printf("✗ 认证没通过：%s\n", lerr.c_str());
                return 1;
            }
            std::printf("  ✓ MFi 认证通过\n");

            // ---- ⑥ 电源 + 5 条订阅 ----
            std::printf("\n=== ⑥ 电源与订阅（0xae03 + 5 条）===\n");
            link.sendPowerSourceUpdate(wcfg.availableCurrentMilliAmps, /*charging=*/true);
            link.sendSubscriptions();
            std::printf("  ✓ 已发 0xae03 PowerSourceUpdate（%u mA）与 5 条订阅\n",
                        unsigned(wcfg.availableCurrentMilliAmps));

            // ---- ⑦ 有线媒体面：找内核 cdc_ncm 那张网卡 ----
            core::iap2::WiredCarPlayEndpoint ep;
            ep.sourceVersion = core::airplay::kSourceVersion;

            allplay::posix::WiredNcmLink ncm;
#ifdef __APPLE__
            std::printf("\n=== ⑦ 有线媒体面（AppleUSBNCMData 建的网卡）===\n");
            if (!allplay::posix::prepareWiredNcmDarwin(info.serial, &ncm, &err)) {
                std::printf("✗ 有线媒体面起不来：%s\n", err.c_str());
                return 1;
            }
#else
            std::printf("\n=== ⑦ 有线媒体面（内核 cdc_ncm）===\n");
            if (!allplay::posix::prepareWiredNcm(int(info.bus), int(info.address), ncmLinkLocal, &ncm,
                                                 &err)) {
                std::printf("✗ 有线媒体面起不来：%s\n", err.c_str());
                return 1;
            }
#endif
            ep.ipv6Addresses = ncm.ipv6;
            std::printf("  ✓ NCM 网卡 %s：MAC %s，IPv6 %s%s\n", ncm.ifname.c_str(), ncm.mac.c_str(),
                        ncm.ipv6.front().c_str(), ncm.pinned ? "（地址是这次钉的）" : "");
            if (ncm.unmanaged) std::printf("  · NetworkManager 已摘开（不然它会去要 DHCP）\n");

            // ---- ⑦b 起 AirPlay 服务端，绑在这张网卡上 ----
            // **必须在发 0x4301 之前**：手机拿到的端口是 0x4301 里写的那个，
            // 而"实际绑到几号"要等监听器起来了才知道（被占会自己往后退）。
            // 地址也得是绑的这个 —— 0x4301 说 fe80::2、却监听在通配上，手机照样连不上。
            core::airplay::ReceiverConfig rcfg;
            rcfg.name        = "AllPlay";
            rcfg.ifname      = ncm.ifname;
            rcfg.bindAddr    = ncm.ipv6.front();
            rcfg.port        = airPlayPort;
            rcfg.btMac       = allplay::posix::controllerAddress();
            rcfg.mfiDir      = mfiDir.empty() ? "identity" : mfiDir;
            rcfg.enableAvahi = false;   // 有线不用广播：手机是从 0x4301 里拿到地址的
            CountingVideoSink sink;
            rcfg.video       = &sink;   // 不接的话手机拿不到 dataPort，会一直干等
            core::airplay::Receiver rx;
            if (!rx.start(rcfg, allplay::posix::makePlatformServices(), &err)) {
                std::printf("✗ AirPlay 服务端起不来：%s\n", err.c_str());
                return 1;
            }
            std::printf("  ✓ AirPlay 服务端已在 [%s]:%u 上监听（deviceid %s）\n",
                        rcfg.bindAddr.c_str(), unsigned(rx.port()), rx.info().deviceId.c_str());

            // 0x4301 里报的必须是**服务端自己认的那一套**：同一个 deviceid、同一把公钥、
            // 同一个端口。发一套自己监听另一套，手机连上来第一件事就是对不上。
            ep.airPlayPort      = rx.port();
            ep.deviceIdentifier = rx.info().deviceId;
            ep.publicKeyHex     = rx.info().pkHex;
            std::printf("  ✓ 身份 %s（pk %s…）\n", rx.info().pi.c_str(),
                        rx.info().pkHex.substr(0, 16).c_str());

            // ---- ⑧ 等 0x4300，回 0x4301 ----
            std::printf("\n=== ⑧ 等 0x4300 CarPlayAvailability → 回 0x4301，Ctrl-C 结束 ===\n");
            bool     sent4301 = false;
            uint64_t beat     = core::iap2::monoMs();
            for (;;) {
                std::vector<core::iap2::WiredLink::Message> msgs;
                if (!link.pump(200, &msgs)) { std::printf("  · 断开\n"); break; }
                for (const auto& m : msgs) {
                    if (m.id == 0x4300) {
                        core::iap2::CarPlayAvailability av;
                        const bool ok = core::iap2::parseCarPlayAvailability(m.body, &av);
                        std::printf("  · 收到 0x4300：%s\n", ok ? core::iap2::describeCarPlayAvailability(av).c_str()
                                                               : "（解析失败，原样 dump）");
                        if (!ok) {
                            for (size_t i = 0; i < m.body.size() && i < 96; ++i)
                                std::printf("%02x ", m.body[i]);
                            std::printf("\n");
                        }
                        // **不按 available 过滤**：DiPlay 也是收到就回，手机自己判断。
                        if (!sent4301) {
                            std::string serr;
                            if (!link.sendCarPlayStartSession(ep, &serr)) {
                                std::printf("✗ 0x4301 发不出去：%s\n", serr.c_str());
                                return 1;
                            }
                            sent4301 = true;
                            std::printf("  ✓ 已回 0x4301 CarPlayStartSession —— "
                                        "等手机往 [%s]:%u 上连 AirPlay\n",
                                        ep.ipv6Addresses.front().c_str(), unsigned(ep.airPlayPort));
                        } else {
                            std::printf("  · 已经回过 0x4301 了，这条只记不答\n");
                        }
                    } else {
                        // **原样 dump**：看对端"想说什么"全靠这一行 —— 认出来的消息是少数，
                        // 剩下的（尤其是带地址/凭据的那种）得靠肉眼认结构。
                        std::printf("  · 控制帧 0x%04x，%zu 字节：", m.id, m.body.size());
                        for (size_t i = 0; i < m.body.size() && i < 128; ++i)
                            std::printf("%02x", m.body[i]);
                        std::printf("\n");
                        std::printf("      ascii: ");
                        for (size_t i = 0; i < m.body.size() && i < 128; ++i) {
                            const unsigned char c = m.body[i];
                            std::printf("%c", (c >= 0x20 && c < 0x7f) ? char(c) : '.');
                        }
                        std::printf("\n");
                    }
                }
                if (core::iap2::monoMs() - beat >= 5000) {
                    beat = core::iap2::monoMs();
                    std::printf("  · %s，还没收到 0x4300\n", sent4301 ? "已回过 0x4301" : "在等手机开口");
                }
            }
            return 0;
        }

        std::printf("\n=== ③ 读 iAP2 流，Ctrl-C 结束 ===\n");
        uint8_t buf[4096];
        for (;;) {
            const int n = svc->read(buf, sizeof buf, 2000);
            if (n < 0) { std::printf("  · 断开\n"); break; }
            if (n == 0) continue;
            std::printf("  · 收 %d 字节：", n);
            for (int i = 0; i < n; ++i) std::printf("%02x ", buf[i]);
            std::printf("\n");
        }
        return 0;
    }

    // ---- ② 拿一条到手机端口的通道 ----
#ifdef __APPLE__
    // macOS 上没有我们自己的 usbmux v2：系统 daemon 就是那一层，直接要隧道。
    std::printf("\n=== ② 经系统 usbmuxd 连手机端口 ===\n");
    auto ch = mux->connectTcp(port, 5000, &err);
#else
    std::printf("\n=== ② usbmux v2 握手 ===\n");
    core::usb::UsbMux mux(*pipe);
    if (!mux.begin(&err)) {
        std::printf("✗ 握手失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ 版本协商 + setup 都过了\n");

    // ---- ③ 开迷你 TCP 到 lockdown ----
    std::printf("\n=== ③ 连手机端口 %u ===\n", port);
    auto ch = mux.connectTcp(port, 5000, &err);
#endif
    if (!ch) {
        std::printf("✗ 连不上：%s\n", err.c_str());
        return 1;
    }
    std::printf("  ✓ 隧道通了（端口 %u）\n", unsigned(port));

    // ---- ④ 可选：发一条 lockdown QueryType ----
    if (doQuery) {
        std::printf("\n=== ④ lockdown QueryType ===\n");
        if (!sendLockdown(*ch, kQueryType)) {
            std::printf("✗ 请求没发出去\n");
            return 1;
        }
        const std::string reply = recvLockdown(*ch, 5000);
        if (reply.empty()) {
            std::printf("✗ 5 秒内没等到回话（通道通了但 lockdown 不理我们）\n");
            return 1;
        }
        std::printf("  ✓ 回了 %zu 字节：\n----\n%s\n----\n", reply.size(), reply.c_str());
    }

    if (doStay) {
        std::printf("\n=== 一直读，Ctrl-C 结束 ===\n");
        uint8_t buf[4096];
        for (;;) {
            const int n = ch->read(buf, sizeof(buf), 1000);
            if (n < 0) { std::printf("  · 断开\n"); break; }
            if (n == 0) continue;
            std::printf("  · 收 %d 字节：", n);
            for (int i = 0; i < n; ++i) std::printf("%02x ", buf[i]);
            std::printf("\n");
        }
    }

    if (doHold) {
        std::printf("\n按回车结束…\n");
        std::getchar();
    }

    ch->close();
    std::printf("\n完成。\n");
    return 0;
}
