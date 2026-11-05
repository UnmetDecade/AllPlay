// ============================================================================
// main.cpp —— 桌面宿主（组合根）：核心 + 解码上屏 + 启动页 + 触摸回传
// ============================================================================
//
// 这个进程就是"车机"侧宿主。装配顺序照 tools/core_selftest.cpp 与
// host/windows_main.cpp：平台服务 → ReceiverConfig → Receiver::start → SDL 主循环。
//
// 线程模型：
//   主线程独占 SDL（事件 + 渲染 + 音频设备开关）。
//   core 的屏幕流/音频流/RTSP 线程只碰 SdlVideoSink / SdlAudioSink，
//   那是两个自带锁的对象，主线程通过 present()/tick() 取结果。
//   跨线程的展示状态走 SharedStatus（atomic）。
//
// 界面两态：
//   未连接 = 启动页铺满整窗（白底 + 蓝牙水纹 + 双色 logo + 状态文字）
//   连接后 = 视频铺满整窗（letterbox），状态栏改为 --statusbar 显式开启的调试叠加
#include "appstate.h"
#include "bt_task.h"
#include "launcher.h"
#include "screenshot.h"
#include "statusbar.h"
#include "wired_task.h"

#include "airplay/server.h"
#include "common/log.h"
#include "posix/posix.h"

#ifdef ALLPLAY_HAVE_FFMPEG
#  include "media/sdl_audio_sink.h"
#  include "media/sdl_video_sink.h"
#endif

#include <SDL.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
    int         screenW   = 1280;
    int         screenH   = 720;
    int         barH      = 26;
    bool        barOn     = false;   // --statusbar：调试叠加（默认关，保证真·全屏视频）
    bool        fullscreen = false;
    int         frames    = 0;       // >0：渲染 N 帧后退出（容器冒烟用）
    std::string font;                // --font，可空（空则用候选表）
    // —— 核心侧 ——
    uint16_t    port      = 7000;
    std::string ifname;
    std::string name      = "AllPlay";
    std::string stateDir;
    std::string identityDir = "identity";
    bool        noAvahi   = false;
    bool        noVideo   = false;
    bool        noAudio   = false;
    std::string dump;                // --dump：退出前把最后一帧存成 PPM
    // —— 有线接入（--wired / --dual 才启用；--wired 和 --bt 互斥，见 main 里的检查）——
    bool        wiredOn   = false;
    bool        dualOn    = false;       // --dual：有线 + 无线同时监听，谁先连上算谁
    std::string ncmLinkLocal;            // --ncm-link-local：空 = fe80::2
    int         wiredPid  = -1;          // --wired-pid：限定产品号（默认不限）
    // —— 蓝牙触发（--bt 才启用；不加就维持纯接收端，行为与之前完全一致）——
    bool        btOn     = false;
    std::string btAddr;                 // --bt-addr：按地址直连，空则按名字找
    std::string btDevice = "iPhone";    // --bt-device：名字包含匹配
    bool        btServer = false;       // --bt-mode：默认 client（Linux/macOS 真机可用的是它）
    std::string btSsid;                 // --ssid：空则取当前连接的 SSID
    std::string btPass;                 // --wifi-pass：0x5703 里发给手机的 Wi-Fi 密码
    int         btRetry  = 10;          // --bt-retry：整链重试次数
    // --inject-touch U,V：连上后往 SDL 事件队列塞一对按下/抬起，坐标按**视频矩形**归一化。
    // 无头（SDL_VIDEODRIVER=dummy）时没有任何真实输入，这是唯一能自动验触摸链路的办法。
    float       injectU   = -1.f;
    float       injectV   = -1.f;
    bool        help      = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "用法: %s [选项]\n"
        "  --screen WxH      视频面尺寸（默认 1280x720，也是向手机协商的目标分辨率）\n"
        "  --fullscreen      全屏（SDL_WINDOW_FULLSCREEN_DESKTOP）\n"
        "  --statusbar       叠加状态栏（调试用；默认关，接了视频就是全屏画面）\n"
        "  --statusbar-h N   状态栏高度（默认 26）\n"
        "  --font PATH       字体（默认 DejaVu；可用 ALLPLAY_FONT 环境变量）\n"
        "  --frames N        渲染 N 帧后退出（无头冒烟测试用，如 SDL_VIDEODRIVER=dummy）\n"
        "  --port N          RTSP 端口（默认 7000，被占自动 +1）\n"
        "  --ifname IFACE     指定网卡（默认自动挑）\n"
        "  --name S          Bonjour 里显示的名字（默认 AllPlay）\n"
        "  --state-dir DIR   状态目录（默认 core 的 defaultStateDir()）\n"
        "  --identity DIR    MFi 身份目录（默认 identity）\n"
        "  --no-avahi        不广播 Bonjour\n"
        "  --no-video        不建视频输出（排查用：只跑协议不上屏）\n"
        "  --no-audio        不建音频输出（排查用；无声卡时也可用 SDL_AUDIODRIVER=dummy）\n"
        "  --dump PATH       退出前把最后一帧画面存成 PPM（无人值守时校验界面）\n"
        "  --inject-touch U,V 连上后自动注入一次触摸（U/V 按视频矩形归一化 0..1）——\n"
        "                    无头验证触摸链路用，配 tools/fake_phone 看它打印的坐标\n"
        "  --wired           走**有线**（USB）CarPlay：插着数据线的 iPhone 直连，不靠蓝牙/Wi-Fi。\n"
        "                    跑之前必须（缺一不可，否则一到切配置就 BUSY）：\n"
        "                      sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd\n"
        "                      pkill -f gvfsd-gphoto2\n"
        "                    而且 /dev/bus/usb 是 root 独占的，**整个程序要 sudo 跑**\n"
        "                    （sudo 会 reset 环境：日志用 sudo env ALLPLAY_LOG=… ，\n"
        "                      状态目录要显式给 --state-dir，否则 HOME 变成 /root）\n"
        "  --dual            双通道：有线 + 无线**同时**监听，谁先连上算谁（另一条路把\n"
        "                    后来的连接挡在门外，会话结束自动放行）。无线那半和只用无线时\n"
        "                    一样要靠 mDNS 广播，所以不要加 --no-avahi\n"
        "  --ncm-link-local A 有线：钉在 NCM 网卡上的 IPv6 链路本地（默认 fe80::2）。\n"
        "                    手机是拿 0x4301 里这个地址来连的，改了只是换个值，但两端得一致\n"
        "  --wired-pid N      有线：限定产品号（默认不限，苹果键盘/集线器会被自动排除）\n"
        "  --bt              启动后主动触发 iPhone（蓝牙 iAP2 → 0x4301），再把画面投出来\n"
        "  --bt-addr A:B:C:D:E:F  按蓝牙地址直连（比按名字可靠，跳过名字匹配）\n"
        "  --bt-device NAME   要触发的设备名（包含匹配，默认 iPhone）\n"
        "  --bt-mode client|server  默认 client（Linux/macOS 上真机可用的是 client：我们主动\n"
        "                    连手机的 iAP2 SPP 服务；server 是注册 SDP 等手机连）\n"
        "  --ssid S           告诉手机的 Wi-Fi 名（默认取本机当前连接的那个）\n"
        "  --wifi-pass P      告诉手机的 Wi-Fi 密码（iAP2 0x5703）\n"
        "  --bt-retry N       触发失败的整链重试次数（默认 10，每次间隔 3 秒）\n"
        "  -h, --help        显示本帮助\n"
        "\n"
        "退出：窗口关闭 / ESC / Ctrl-C。\n"
        "运行时：--bt 按 B 重新触发一次；--wired 准备失败时按 W 重来一次。\n",
        argv0);
}

bool parseScreen(const char* s, int* w, int* h)
{
    return std::sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--screen")) {
            if (!parseScreen(need(a), &o->screenW, &o->screenH)) {
                std::fprintf(stderr, "--screen 需要 WxH，如 1280x720\n");
                return false;
            }
        } else if (!std::strcmp(a, "--fullscreen")) {
            o->fullscreen = true;
        } else if (!std::strcmp(a, "--statusbar")) {
            o->barOn = true;
        } else if (!std::strcmp(a, "--statusbar-h")) {
            o->barH = std::atoi(need(a));
            if (o->barH < 10 || o->barH > 200) {
                std::fprintf(stderr, "--statusbar-h 超范围（10..200）\n");
                return false;
            }
        } else if (!std::strcmp(a, "--font")) {
            o->font = need(a);
        } else if (!std::strcmp(a, "--frames")) {
            o->frames = std::atoi(need(a));
        } else if (!std::strcmp(a, "--port")) {
            o->port = uint16_t(std::atoi(need(a)));
        } else if (!std::strcmp(a, "--ifname")) {
            o->ifname = need(a);
        } else if (!std::strcmp(a, "--name")) {
            o->name = need(a);
        } else if (!std::strcmp(a, "--state-dir")) {
            o->stateDir = need(a);
        } else if (!std::strcmp(a, "--identity")) {
            o->identityDir = need(a);
        } else if (!std::strcmp(a, "--no-avahi")) {
            o->noAvahi = true;
        } else if (!std::strcmp(a, "--no-video")) {
            o->noVideo = true;
        } else if (!std::strcmp(a, "--no-audio")) {
            o->noAudio = true;
        } else if (!std::strcmp(a, "--dump")) {
            o->dump = need(a);
        } else if (!std::strcmp(a, "--wired")) {
            o->wiredOn = true;
        } else if (!std::strcmp(a, "--dual")) {
            o->dualOn = true;
        } else if (!std::strcmp(a, "--ncm-link-local")) {
            o->ncmLinkLocal = need(a);
        } else if (!std::strcmp(a, "--wired-pid")) {
            o->wiredPid = std::atoi(need(a));
        } else if (!std::strcmp(a, "--bt")) {
            o->btOn = true;
        } else if (!std::strcmp(a, "--bt-addr")) {
            o->btAddr = need(a);
        } else if (!std::strcmp(a, "--bt-device")) {
            o->btDevice = need(a);
        } else if (!std::strcmp(a, "--bt-mode")) {
            const char* v = need(a);
            if (!std::strcmp(v, "client"))      o->btServer = false;
            else if (!std::strcmp(v, "server")) o->btServer = true;
            else {
                std::fprintf(stderr, "--bt-mode 只认 client / server\n");
                return false;
            }
        } else if (!std::strcmp(a, "--ssid")) {
            o->btSsid = need(a);
        } else if (!std::strcmp(a, "--wifi-pass")) {
            o->btPass = need(a);
        } else if (!std::strcmp(a, "--bt-retry")) {
            o->btRetry = std::atoi(need(a));
        } else if (!std::strcmp(a, "--inject-touch")) {
            const char* v = need(a);
            if (std::sscanf(v, "%f,%f", &o->injectU, &o->injectV) != 2 ||
                o->injectU < 0.f || o->injectU > 1.f ||
                o->injectV < 0.f || o->injectV > 1.f) {
                std::fprintf(stderr, "--inject-touch 需要 U,V（各 0..1），如 0.25,0.75\n");
                return false;
            }
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            o->help = true;
        } else {
            std::fprintf(stderr, "未知参数：%s（-h 看帮助）\n", a);
            return false;
        }
    }
    return true;
}

/// 触摸：core 的 event_channel 就是固定 2 个 slot（event_channel.cpp:20-37），
/// 所以这里也维护 2 个，**永远发满 2 个** —— slot 编号稳定，手机侧才不会看到
/// "slot1 忽然冒出来"。
/// 桌面上鼠标和触屏一般不会同时用：鼠标固定占 slot 0，手指从空位开始占。
class TouchPad {
public:
    /// 点 → 像素的比例（高 DPI）。SDL 的鼠标/FINGER 事件给的是**窗口点坐标**，
    /// 而 flush() 里的 videoRect 是 SDL_GetRendererOutputSize 的**像素**矩形。
    /// Retina 上两者差 2 倍：不换算的话点在屏幕正中会归一化成 (0.25, 0.25)，
    /// 手机那边看到的就是"偏上偏左"。（实测：Mac 触摸 y 轴偏上。）
    void setScale(float sx, float sy)
    {
        scaleX_ = sx > 0.f ? sx : 1.f;
        scaleY_ = sy > 0.f ? sy : 1.f;
    }

    void handle(const SDL_Event& e, int winW, int winH)
    {
        switch (e.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;   // 触屏合成的鼠标事件，忽略
                if (e.button.button != SDL_BUTTON_LEFT) break;
                slots_[0].down = true;
                slots_[0].used = true;
                slots_[0].x = float(e.button.x) * scaleX_;
                slots_[0].y = float(e.button.y) * scaleY_;
                dirty_ = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                if (e.button.button != SDL_BUTTON_LEFT) break;
                slots_[0].down = false;
                dirty_ = true;
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.which == SDL_TOUCH_MOUSEID) break;
                if (!slots_[0].down) break;
                slots_[0].used = true;
                slots_[0].x = float(e.motion.x) * scaleX_;
                slots_[0].y = float(e.motion.y) * scaleY_;
                dirty_ = true;
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP: {
                // FINGER 的坐标是窗口归一化的 0..1，换算成像素跟鼠标统一处理
                const int i = fingerSlot(e.tfinger.fingerId);
                if (i < 0) break;   // 已经 2 个触点了，多的忽略
                slots_[i].down = (e.type != SDL_FINGERUP);
                slots_[i].used = true;
                slots_[i].x = e.tfinger.x * float(winW) * scaleX_;
                slots_[i].y = e.tfinger.y * float(winH) * scaleY_;
                dirty_ = true;
                break;
            }
            default:
                break;
        }
    }

    /// 把当前触点状态发给手机。rect = letterbox 后的视频矩形（坐标按它归一化）。
    /// 落在黑边上的点直接不发 —— clamp 会让边缘误触。
    void flush(core::airplay::Receiver& rx, const SDL_Rect& rect, bool active)
    {
        if (!active) {
            if (wasActive_) {   // 断开前把手指"抬起来"，免得手机侧卡在按下态
                rx.sendTouch(std::vector<core::airplay::TouchPoint>(2));
                wasActive_ = false;
            }
            return;
        }
        if (!dirty_) return;
        dirty_ = false;
        wasActive_ = true;

        std::vector<core::airplay::TouchPoint> pts(2);
        for (int i = 0; i < 2; ++i) {
            // 抬起的那一帧也必须带上**最后的位置**，不能只发 down=false 把坐标留成 (0,0)。
            // 否则手机看到的是"在图标处按下 → 在左上角抬起"，那是一次滑动而不是点击，
            // 结果就是点图标毫无反应 —— 而拖地图照常有效（拖动效果早就发生了，看不出差别）。
            // 安卓那边 ACTION_UP 时 getX() 返回的就是最后位置（MainActivity.kt:222），这里对齐它。
            if (!slots_[i].used || rect.w <= 0 || rect.h <= 0) continue;
            const float u = (slots_[i].x - float(rect.x)) / float(rect.w);
            const float v = (slots_[i].y - float(rect.y)) / float(rect.h);
            if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) continue;   // 黑边上，忽略
            pts[i].x = u;
            pts[i].y = v;
            pts[i].down = slots_[i].down;
        }
        rx.sendTouch(pts);
    }

    void reset()
    {
        slots_[0] = slots_[1] = Slot{};
        dirty_ = false;
    }

private:
    /// used = 这个 slot 被用过（按下过 / 移动过）。抬起后坐标要继续上报，所以
    /// 判断"这个 slot 要不要参与本次上报"不能用 down —— down 一清零就什么都不发了。
    struct Slot { bool down = false, used = false; float x = 0, y = 0; SDL_FingerID id = -1; };

    /// 已有该手指 → 原 slot；否则找空位（鼠标占着 0 时从 1 开始找）
    int fingerSlot(SDL_FingerID id)
    {
        for (int i = 0; i < 2; ++i)
            if (slots_[i].id == id) return i;
        for (int i = (slots_[0].down && slots_[0].id < 0) ? 1 : 0; i < 2; ++i)
            if (!slots_[i].down) { slots_[i].id = id; return i; }
        return -1;
    }

    Slot slots_[2];
    bool dirty_ = false;
    bool wasActive_ = false;
    float scaleX_ = 1.f;   ///< 窗口点 → 渲染像素（见 setScale）
    float scaleY_ = 1.f;
};

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 2;
    if (opt.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (!opt.font.empty()) setenv("ALLPLAY_FONT", opt.font.c_str(), 1);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    core::logInitFromEnv();
    std::printf("[host] allplay_host — 视频面 %dx%d%s\n",
                opt.screenW, opt.screenH, opt.fullscreen ? "，全屏" : "");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[host] SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    // 启动页是在 1540×850 的设计稿坐标上画完再整体缩放的；默认的最近邻采样会把
    // 圆角/圆弧放大成台阶（1.6 倍屏上尤其明显）。改成线性采样，边缘是连续过渡的。
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    // ALLOW_HIGHDPI 不能省：不给这个标志，macOS 只让渲染器画"逻辑点"那么大的
    // 后备缓冲（1280×720），再由系统放大到 Retina 物理像素（2 倍）—— 界面上
    // 每一个圆角、每一根字都会被整倍放大，就是"超级糊、完全没法看"。
    // 给了之后 SDL_GetRendererOutputSize 返回物理像素，所有绘制按物理像素走。
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    // 状态栏画在 y = opt.screenH 处（见 StatusBar::draw 用的 y_，以及下面的 bar.init），
    // 所以窗口必须比视频面**高出这一条**。只给 screenH 会同时出两个毛病：
    //   ① 状态栏整个落在窗口下沿之外，根本看不见；
    //   ② 渲染循环仍按 areaH = winH - barH 排视频，16:9 的画面被缩进 1280x(screenH-barH)，
    //      左右各多出一条黑边 —— 就是"开了 --statusbar 就有黑边"。
    const int winPxH = opt.screenH + (opt.barOn ? opt.barH : 0);
    SDL_Window* win = SDL_CreateWindow(
        "AllPlay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        opt.screenW, winPxH, winFlags);
    if (!win) {
        std::fprintf(stderr, "[host] 建窗口失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 优先硬件渲染 + vsync；无显示环境（SDL_VIDEODRIVER=dummy 冒烟）回落软件
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    bool vsync = true;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        vsync = false;
    }
    if (!ren) {
        std::fprintf(stderr, "[host] 建渲染器失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // 高 DPI 自检：比值 > 1 才说明我们真的按物理像素在画，而不是让系统放大
    {
        int wpt = 0, hpt = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(win, &wpt, &hpt);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        std::printf("[host] 窗口 %dx%d 点 / 渲染面 %dx%d 像素（%.2gx）\n",
                    wpt, hpt, ow, oh, wpt > 0 ? double(ow) / double(wpt) : 1.0);
    }

    allplay::SharedStatus status;
    allplay::StatusBar   bar;
    bar.init(ren, opt.screenW, opt.screenH, opt.barH);
    allplay::Launcher    launcher;
    if (!launcher.init()) {
        std::fprintf(stderr, "[host] 启动页字体不可用，只画图形\n");
    }
    {
        // 启动页印的是**手机蓝牙列表里看到的那个名字**，也就是蓝牙适配器的名字，
        // 不是 Bonjour 广播名（--name）。拿不到适配器名（没蓝牙 / 没权限）才退回它。
        std::string btName = allplay::posix::bluetoothAdapterName();
        if (btName.empty()) btName = opt.name;
        launcher.setDeviceName(btName);
    }

    // —— 解码输出：必须在 Receiver 之前声明，才能比它活得久 ——
    // Receiver 的屏幕流线程会一直持有 video 指针直到 stop() 返回。
    core::IVideoOutput* vid = nullptr;
    core::IAudioOutput* aud = nullptr;
#ifdef ALLPLAY_HAVE_FFMPEG
    allplay::SdlVideoSink videoSink;
    allplay::SdlAudioSink audioSink;
    if (!opt.noVideo) vid = &videoSink;
    if (!opt.noAudio) aud = &audioSink;
#else
    if (!opt.noVideo || !opt.noAudio)
        std::fprintf(stderr, "[host] 本次构建没带 ffmpeg —— 视频/音频降级（只有启动页）\n");
#endif

    // —— 启动协议核心 ——
    // 有线和无线在这儿差的是**谁先谁后**，不是配置：
    //   无线：Receiver 先起（绑通配 + mDNS 广播），让手机自己找过来 —— "地址"是我们广播的。
    //   有线：手机是从 0x4301 里**读**地址和端口的，所以 Receiver 必须等 USB 侧拆到
    //         "NCM 网卡叫什么、地址钉成哪个"之后才能起（见 wired_task.h 的相位说明）。
    //         这里把 rx.start() 推迟到主循环里那一支去调。
    if (opt.wiredOn && opt.btOn) {
        std::fprintf(stderr, "[host] --wired 和 --bt 是两条互斥的路（一个插线、一个走蓝牙），"
                             "别一起开\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 2;
    }

    // —— 两条通道 ——
    //   rxNet：无线（绑通配 + mDNS 广播），手机自己找过来；
    //   rxUsb：有线（钉在 NCM 网卡的链路本地，地址由 0x4301 直接告诉手机）。
    // 只有一条路时另一条保持"没起"，状态合并 / 触摸转发都按 running() 判，
    // 所以单通道模式的行为和以前完全一样。
    core::airplay::Receiver rxNet;
    core::airplay::Receiver rxUsb;
    const bool useWired    = opt.wiredOn || opt.dualOn;
    const bool useWireless = !opt.wiredOn || opt.dualOn;   // --wired 单开时不开无线

    core::airplay::ReceiverConfig cfg;                     // 无线那份
    cfg.name      = opt.name;
    cfg.ifname    = opt.ifname;
    cfg.port      = opt.port;
    cfg.width     = opt.screenW;
    cfg.height    = opt.screenH;
    cfg.stateDir  = opt.stateDir;
    cfg.mfiDir    = opt.identityDir;
    cfg.enableAvahi = !opt.noAvahi;
    cfg.video     = vid;
    cfg.audio     = aud;

    core::airplay::ReceiverConfig cfgUsb = cfg;            // 有线那份：起的时候再补 ifname/bindAddr
    cfgUsb.enableAvahi = false;   // 有线不广播：地址已经通过 0x4301 直接告诉手机了

    // 平台服务要在 main 的整个生命周期里活着：Receiver 只拷了一份，而 Session 到 SETUP
    // 才第一次用它去建数据/事件通道（存临时对象的地址就是当年那个悬垂指针）。
    auto services = allplay::posix::makePlatformServices();
    std::string err;

    // —— 有线接入任务（--wired）——
    allplay::WiredTask        wiredTask;
    allplay::WiredTaskOptions wopt;
    bool                      wiredReceiverUp = false;  // Receiver 起来没有（只此一处赋值）
    std::string               wiredLastErr;             // 报过的失败原因：同一个原因别刷屏
    // 链路断掉之后隔多久自己再准备一次。3 秒和 bt_task 的重试节奏一致 —— 拔线重插、
    // 解锁、点"信任"都得几秒，更快只是白跑 USB 枚举。
    const uint64_t            kWiredRearmMs = 3000;
    uint64_t                  wiredRearmAt  = SDL_GetTicks64() + kWiredRearmMs;
    wopt.name         = opt.name;
    wopt.mfiDir       = opt.identityDir;
    wopt.stateDir     = opt.stateDir;   // 配对材料存这儿：不存就每次都弹信任框
    wopt.ncmLinkLocal = opt.ncmLinkLocal;
    wopt.productId    = opt.wiredPid;
    auto wiredLog = [](const std::string& s) {
        std::printf("[wired] %s\n", s.c_str());
        std::fflush(stdout);
    };

    // 双通道互斥：先连上的赢。两条闸门都看**对面**的状态 —— 对面在用（或正在握手）
    // 就把这条新连接直接关掉。会话一结束，对面回到空闲，闸门自动放行。
    // 单通道模式下对面那条永远没起（connecting/sessionActive 恒为 false），等于没装。
    auto installGates = [&] {
        rxNet.setConnectionGate([&rxUsb] {
            return !(rxUsb.connecting() || rxUsb.sessionActive());
        });
        rxUsb.setConnectionGate([&rxNet] {
            return !(rxNet.connecting() || rxNet.sessionActive());
        });
    };

    if (useWireless) {
        if (!rxNet.start(cfg, services, &err)) {
            // 双通道下无线起不来不算致命（比如车机只有有线可用、或者没 Wi-Fi 网卡）：
            // 有线那条照样能连。单开无线时就是启动失败。
            if (!opt.dualOn) {
                std::fprintf(stderr, "[host] 核心启动失败：%s\n", err.c_str());
                SDL_DestroyRenderer(ren);
                SDL_DestroyWindow(win);
                SDL_Quit();
                return 1;
            }
            std::printf("[host] 双通道：无线那半起不来（%s）—— 继续只用有线\n", err.c_str());
        } else {
            std::printf("[host] 无线 RTSP :%u  ifname=%s ip=%s deviceid=%s\n",
                        rxNet.port(), rxNet.ifname().c_str(), rxNet.ip().c_str(),
                        rxNet.info().deviceId.c_str());
            if (cfg.enableAvahi) {
                std::printf("[host] Bonjour: %s\n",
                            rxNet.avahiError().empty()
                                ? "已广播 _airplay._tcp"
                                : ("不可用（" + rxNet.avahiError() + "）").c_str());
            }
            std::printf("[host] READY —— 等 iPhone 连接（假手机：tools/fake_phone）\n");
        }
        installGates();
    }
    if (useWired) {
        std::printf("[host] %s：准备 USB 链路…\n"
                    "       手机首次会弹\"信任此电脑\"，去点一下；锁屏时它只报 1 个配置，\n"
                    "       得先解锁（这不是线的问题）。\n",
                    opt.dualOn ? "双通道（有线那半）" : "有线模式");
        if (!wiredTask.beginBringUp(wopt, wiredLog)) {
            std::fprintf(stderr, "[host] 有线任务没能启动\n");
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }
    std::fflush(stdout);

    // —— 蓝牙触发（--bt）——
    // 必须放在 rx.start() 之后：触发参数要用 rx.ip()/port()/info()，它们启动完才有效。
    // 触发本身跑在 BtTask 自己的线程上（runBtTrigger 是全阻塞的，放主线程会冻住画面）。
    allplay::BtTask        btTask;
    allplay::BtTaskOptions btOpt;
    btOpt.name          = opt.name;
    btOpt.deviceName    = opt.btDevice;
    btOpt.deviceAddress = opt.btAddr;
    btOpt.serverMode    = opt.btServer;
    btOpt.ssid          = opt.btSsid;
    btOpt.passphrase    = opt.btPass;
    btOpt.identityDir   = opt.identityDir;
    btOpt.retry         = opt.btRetry;
    auto btLog = [](const std::string& s) {
        std::printf("[bt] %s\n", s.c_str());
        std::fflush(stdout);
    };
    if (opt.btOn && !btTask.start(rxNet, btOpt, btLog)) {
        std::fprintf(stderr, "[host] 蓝牙触发没能启动\n");
    }

    TouchPad pad;

    bool     running   = true;
    int      rendered  = 0;
    int      injectStep = 0;   ///< 连上后数帧，--inject-touch 的按下/抬起就落在固定帧上
    bool     wasStreaming = false;
    Uint64   lastFpsAt = SDL_GetTicks64();
    uint64_t lastFrames = 0;

    // 刚获得焦点的那一下鼠标按下不算"点按钮"。
    // macOS 的 SDLView 实现了 acceptsFirstMouse —— 点一下窗口把它激活，这一下也会
    // 送进来；要是正好落在「取消」上，启动页就一闪而过，看起来像"根本没起来"。
    Uint64   focusAt     = SDL_GetTicks64();
    constexpr Uint64 kClickGuardMs = 300;

    // --bt 的常驻重连：手机离开/掉线之后隔一会儿自动再叫一次（车机行为）。
    // 触发本身最多跑 o.retry 轮，跑完 btTask.done() 变 true，这里才允许再起。
    constexpr Uint64 kBtRearmMs = 5000;
    Uint64   btRearmAt = SDL_GetTicks64() + kBtRearmMs;
    Uint64   btWaitingSince = 0;   ///< 触发线程在 WaitingPair 里连续待了多久（看门狗用）

    while (running && !g_stop) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_w &&
                     useWired && !wiredReceiverUp) {
                // 手机解锁了、线重插了、刚才没人点"信任"…… 都不用重启程序。
                // Receiver 已经起来之后不给重来：那条链路是 0x4301 报出去过的，不能偷偷换。
                if (wiredTask.beginBringUp(wopt, wiredLog)) {
                    wiredLastErr.clear();   // 手动重来：失败原因值得从头再报一次
                    std::printf("[host] 重新准备有线链路…\n");
                    std::fflush(stdout);
                } else {
                    std::printf("[host] 上一次准备还在跑，稍候再按 W\n");
                    std::fflush(stdout);
                }
            }
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_b && opt.btOn) {
                // 触发失败、或手机掉线之后，不用重启程序
                if (btTask.start(rxNet, btOpt, btLog)) {
                    std::printf("[host] 重新触发蓝牙…\n");
                    std::fflush(stdout);
                } else {
                    std::printf("[host] 上一次触发还在跑，稍候再按 B\n");
                    std::fflush(stdout);
                }
            }
            else if (e.type == SDL_WINDOWEVENT &&
                     e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                focusAt = SDL_GetTicks64();
            }
            else if (!wasStreaming && e.type == SDL_MOUSEBUTTONDOWN &&
                     SDL_GetTicks64() - focusAt < kClickGuardMs) {
                // 激活窗口的那一下：吞掉，别当成点「取消」（也别塞给触摸板）
            }
            else if (!wasStreaming && launcher.handleEvent(e, opt.screenW, opt.screenH)) {
                std::printf("[host] 启动页：点了「取消」，退出\n");
                std::fflush(stdout);
                running = false;
            }
            else pad.handle(e, opt.screenW, opt.screenH);
        }

#ifdef ALLPLAY_HAVE_FFMPEG
        // 音频设备只能主线程开关（SDL 的约定），RTSP 线程来的 configure() 只记待办。
        // 没变化时它立刻返回，放这里每帧调没有代价。
        audioSink.serviceDevice();
        status.audioFrames.store(audioSink.decoded());
        status.audioUnderruns.store(audioSink.underruns());
#endif

        // —— 有线：链路断了要能自己活过来 ——
        // 会话结束（拔线、手机锁屏、重枚举）之后 runPump 把相位落到 Done，而
        // wiredReceiverUp 还立着 —— 不把它放下来，这程序就只剩"重启"一条路：
        // 连 W 都被 !wiredReceiverUp 挡在门外。放下来 + 冷却几秒自己重来，拔了再插就行。
        if (useWired && wiredReceiverUp && wiredTask.pumpDone()) {
            std::printf("[host] 有线链路断了 —— 收起有线 RTSP，%llu 秒后自动重新准备\n",
                        static_cast<unsigned long long>(kWiredRearmMs / 1000));
            std::fflush(stdout);
            rxUsb.stop();              // 网卡可能整块消失过（重枚举），监听得跟着重来
            wiredReceiverUp = false;
            wiredRearmAt    = SDL_GetTicks64() + kWiredRearmMs;
        }

        // —— 有线：USB 侧就绪之后才起 Receiver（全程序**只有这一处**调 rxUsb.start）——
        if (useWired && !wiredReceiverUp) {
            const allplay::WiredPhase wp = wiredTask.phase();
            if (wp == allplay::WiredPhase::Ready) {
                const allplay::WiredBringUp& bu = wiredTask.result();
                cfgUsb.ifname   = bu.ifname;
                cfgUsb.bindAddr = bu.bindAddr;   // 链路本地，server.cpp 会自己补 %网卡
                if (!rxUsb.start(cfgUsb, services, &err)) {
                    // 这里失败基本只有一个原因：那张网卡的 7000 被占了。
                    // 双通道下无线那半还在，所以不退出，等下一轮重试。
                    std::fprintf(stderr, "[host] 有线：Receiver 起不来：%s\n", err.c_str());
                    if (!useWireless || !rxNet.running()) running = false;
                    else wiredRearmAt = SDL_GetTicks64() + kWiredRearmMs;
                } else {
                    wiredReceiverUp = true;
                    std::printf("[host] 有线 RTSP [%s%%%s]:%u（deviceid %s）\n",
                                rxUsb.ip().c_str(), rxUsb.ifname().c_str(), unsigned(rxUsb.port()),
                                rxUsb.info().deviceId.c_str());
                    // 0x4301 里报的端口/deviceid/公钥，全部取自 rxUsb 自己 —— 见 startPump 注释
                    installGates();
                    if (!wiredTask.startPump(rxUsb, wiredLog)) {
                        std::printf("[host] 有线：0x4301 那一支没起来\n");
                    }
                    std::fflush(stdout);
                }
            } else if (wp == allplay::WiredPhase::Failed && wiredTask.error() != wiredLastErr) {
                // 同一个原因只报一次：手机没插时每 3 秒重试一轮，刷屏没有意义。
                // 原因变了（比如从"找不到设备"变成"配对超时"）说明情况在动，再报一次。
                wiredLastErr = wiredTask.error();
                std::printf("[host] 有线准备失败：%s\n"
                            "       （会自动重试；手机锁屏就先解锁、没点过\"信任\"就去点一下）\n",
                            wiredLastErr.c_str());
                std::fflush(stdout);
            }
            // 没准备好就按冷却自己再来一次 —— 拔了再插、解锁、点过"信任"之后都不用按键。
            // 放在上面那条之后：先让人看见失败原因，再重试。
            if (!wiredReceiverUp &&
                (wp == allplay::WiredPhase::Failed || wp == allplay::WiredPhase::Done) &&
                SDL_GetTicks64() >= wiredRearmAt) {
                if (wiredTask.beginBringUp(wopt, wiredLog))
                    wiredRearmAt = SDL_GetTicks64() + kWiredRearmMs;
            }
        }

        // —— 核心状态 → LinkState（两条通道合并成一个状态）——
        // 规则：有任意一条在出画面就是 Streaming；有任意一条在握手就是 Connecting；
        // 只要还有一条在听，就是 WaitingPair（哪怕另一条还在准备）。
        allplay::LinkState st;
        if (rxUsb.sessionActive() || rxNet.sessionActive())
            st = allplay::LinkState::Streaming;
        else if (rxUsb.connecting() || rxNet.connecting())
            st = allplay::LinkState::Connecting;
        else if (rxUsb.running() || rxNet.running())
            st = allplay::LinkState::WaitingPair;
        else if (useWired && wiredTask.phase() == allplay::WiredPhase::Failed)
            // 还没起来不算"错误"，是"还在准备" —— 报 Error 会让启动页写"Error"，
            // 把"手机锁着、等人点信任"说成故障。只有真失败了才报 Error。
            st = allplay::LinkState::Error;
        else
            st = allplay::LinkState::Connecting;
        status.state.store(static_cast<int>(st));

        // —— 蓝牙常驻重连（--bt）：手机离开/掉线后自动再叫一次 ——
        // 车机的行为是"配件一直在，手机回来自己连上"，不需要人去按键。
        // 三个条件都满足才叫：上一次触发线程已收工、两条通道都空闲（WaitingPair）、
        // 过了冷却期 —— 否则会和正在进行的握手打架（手机连着时又叫一次会把链路搅乱）。
        // 但触发线程现在是"常驻"的：0x4301 发出后它会一直守着 iAP2 链路，手机走了
        // 也不一定结束。若它在 WaitingPair 里停了太久（手机拿了 0x4301 却没连
        // AirPlay），就主动取消，重新叫一次 —— 否则永远不会再触发。
        constexpr Uint64 kBtParkedMs = 45000;
        if (opt.btOn && !btTask.done() && st == allplay::LinkState::WaitingPair) {
            if (btWaitingSince == 0) btWaitingSince = SDL_GetTicks64();
            else if (SDL_GetTicks64() - btWaitingSince > kBtParkedMs) {
                btTask.stop();
                btRearmAt = SDL_GetTicks64() + kBtRearmMs;
                btWaitingSince = 0;
                std::printf("[host] 蓝牙链路空等太久 —— 取消，准备重新触发\n");
                std::fflush(stdout);
            }
        } else {
            btWaitingSince = 0;
        }

        if (opt.btOn && btTask.done() && st == allplay::LinkState::WaitingPair &&
            SDL_GetTicks64() >= btRearmAt) {
            if (btTask.start(rxNet, btOpt, btLog)) {
                btRearmAt = SDL_GetTicks64() + kBtRearmMs;
                btWaitingSince = SDL_GetTicks64();
                std::printf("[host] 手机不在了 —— 已自动重新触发蓝牙，等它回来\n");
                std::fflush(stdout);
            }
        }

        // 触摸发给**正在用的那条**通道（双通道下只有一条能连上）
        auto activeRx = [&]() -> core::airplay::Receiver& {
            if (rxUsb.running() && (rxUsb.sessionActive() || rxUsb.connecting())) return rxUsb;
            return rxNet;
        };

        // 进入 Streaming 的那一刻切全屏（对齐安卓：连上就铺满）
        const bool streaming = (st == allplay::LinkState::Streaming);
        if (streaming != wasStreaming) {
            wasStreaming = streaming;
            pad.reset();
            std::printf("[host] 会话%s\n", streaming ? "开始 —— 切视频面" : "结束 —— 回启动页");
            std::fflush(stdout);
            if (opt.btOn && !streaming) {
                // 会话结束了：把还挂在 iAP2 链路上的触发线程撤掉，好让重排逻辑
                // 在手机回来时重新叫 0x4301（否则它会一直守在旧链路上）。
                btTask.stop();
                btRearmAt = SDL_GetTicks64() + kBtRearmMs;
                btWaitingSince = 0;
            }
            if (streaming && opt.fullscreen) {
                SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
#ifdef ALLPLAY_HAVE_FFMPEG
            // 会话结束必须把末帧丢掉：sink 会一直重画纹理里那一帧
            // （"画面停住而不是消失"是为会话中途掉帧设计的），
            // 留着它就让上面那句"回启动页"变成谎话——画面卡死在最后一帧。
            if (!streaming) videoSink.dropFrame();
#endif
        }

        int winW = 0, winH = 0;
        SDL_GetRendererOutputSize(ren, &winW, &winH);

        // 高 DPI：渲染面是物理像素，窗口尺寸是逻辑点，两者比值给启动页做鼠标换算
        {
            int wpt = winW, hpt = winH;
            SDL_GetWindowSize(win, &wpt, &hpt);
            launcher.setPixelRatio(wpt > 0 ? double(winW) / double(wpt) : 1.0);
            // 触摸同一件事：事件是"点"，videoRect 是"像素"（见 TouchPad::setScale）
            pad.setScale(wpt > 0 ? float(double(winW) / double(wpt)) : 1.f,
                         hpt > 0 ? float(double(winH) / double(hpt)) : 1.f);
        }

        // 状态栏只在显式开启时占位置
        const int barH = opt.barOn ? bar.height() : 0;
        const int areaY = 0;
        const int areaH = winH - barH;

        SDL_Rect videoRect{0, 0, 0, 0};
#ifdef ALLPLAY_HAVE_FFMPEG
        if (streaming || videoSink.hasFrame()) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            int vw = 0, vh = 0;
            videoSink.intrinsicSize(&vw, &vh);
            allplay::fitLetterbox(vw, vh, winW, areaH, &videoRect);
            videoRect.y += areaY;
            videoSink.present(ren, videoRect);
            status.videoW.store(vw);
            status.videoH.store(vh);
            status.frames.store(videoSink.presented());
        } else
#endif
        {
            launcher.draw(ren, winW, winH, st, SDL_GetTicks64());
        }

        if (opt.barOn) bar.draw(ren, status);

        // 读回必须在 Present **之前**。GL 的 backbuffer 在 Present 之后内容未定义，
        // llvmpipe/Wayland 上读回来是全黑（Mac 上碰巧还能读到，所以这个错一直没暴露）。
        if (!opt.dump.empty() && opt.frames > 0 && rendered == opt.frames - 1) {
            allplay::dumpRender(ren, opt.dump);
        }
        SDL_RenderPresent(ren);

        pad.flush(activeRx(), videoRect, streaming);

        // —— 无头触摸注入（--inject-touch）——
        // 塞进 SDL 事件队列，下一轮 SDL_PollEvent 取出后走的是**和真实鼠标完全相同**的
        // TouchPad::handle 路径；差别只在没经过 X11/Wayland 输入驱动。
        // 时机：连上后第 20 帧按下、第 50 帧抬起 —— 中间隔着 30 帧，好让手机侧看清
        // "按下"与"抬起"是两次独立上报。
        if (opt.injectU >= 0.f && streaming && videoRect.w > 0 && videoRect.h > 0) {
            ++injectStep;
            if (injectStep == 20 || injectStep == 50) {
                const bool down = (injectStep == 20);
                const Sint32 px = videoRect.x + Sint32(opt.injectU * float(videoRect.w));
                const Sint32 py = videoRect.y + Sint32(opt.injectV * float(videoRect.h));
                SDL_Event ev{};
                ev.type            = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                ev.button.type     = ev.type;
                ev.button.state    = down ? SDL_PRESSED : SDL_RELEASED;
                ev.button.button   = SDL_BUTTON_LEFT;
                ev.button.which    = 0;          // 真鼠标（SDL_TOUCH_MOUSEID 才是触屏合成的）
                ev.button.clicks   = 1;
                ev.button.x        = px;
                ev.button.y        = py;
                SDL_PushEvent(&ev);
                std::printf("[host] 注入触摸%s：窗口(%d,%d) 视频矩形 %dx%d@(%d,%d) → 归一化 %.3f,%.3f\n",
                            down ? "按下" : "抬起",
                            int(px), int(py), videoRect.w, videoRect.h, videoRect.x, videoRect.y,
                            double(opt.injectU), double(opt.injectV));
                std::fflush(stdout);
            }
        } else if (!streaming) {
            injectStep = 0;   // 断开重连后重新数，别让第二次会话直接跳过注入点
        }

        // 1 秒窗口的显示帧率
        const Uint64 now = SDL_GetTicks64();
        if (now - lastFpsAt >= 1000) {
            const uint64_t f = status.frames.load();
            status.fps.store(int(f - lastFrames));
            lastFrames = f;
            lastFpsAt  = now;
            status.lastFrameMs.store(now);
        }

        if (opt.frames > 0 && ++rendered >= opt.frames) {
            std::printf("[host] 冒烟：已渲染 %d 帧，正常退出\n", rendered);
            break;
        }
        if (!vsync) SDL_Delay(16);
    }

    std::printf("[host] 收尾：停核心…\n");
    std::fflush(stdout);
    btTask.stop();      // 触发线程 detach 掉（runBtTrigger 没有取消点，见 host/bt_task.cpp）
    // 有线：先停接入任务（join pump；它每 200ms 查一次停止位），再停核心。
    // 顺序不能反 —— pump 线程还攥着 iAP2 链路（Impl 里的 link），得让它先收手。
    wiredTask.stop();
    rxNet.stop();       // 先停核心（join 屏幕流/音频流线程），再让 sink 析构
    rxUsb.stop();       // 两条通道都要停：accept 线程会读对面的状态（闸门），别留悬垂引用
#ifdef ALLPLAY_HAVE_FFMPEG
    // 音频不做"有没有声音"的自动判定（无头跑时设备是 dummy，听不见）；
    // 这里给出的是**可机检**的证据：解出多少帧（对上手机发的帧数就说明解码链通）、
    // 缓冲欠载几次、被挤掉几次。⚠️ 无头下的欠载数不可当音质指标：dummy 声卡不按
    // 实时节奏回调，抖动都记在它头上，要看音质得跑到真有声卡的机器上。
    std::printf("[host] 音频：解码 %llu 帧、缓冲欠载 %llu 次（dummy 声卡下不可当音质指标）、"
                "溢出丢最旧 %llu 次%s\n",
                (unsigned long long)audioSink.decoded(),
                (unsigned long long)audioSink.underruns(),
                (unsigned long long)audioSink.dropped(),
                audioSink.deviceOpen() ? "" : "（设备没开）");
#endif
    launcher.releaseTextures();   // 文字纹理是渲染器的子对象，先放掉再毁渲染器
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("[host] 已退出\n");
    return 0;
}
