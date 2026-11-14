// ============================================================================
// main.cpp -- desktop host (composition root): core + decode-and-present + launcher page + touch forwarding
// ============================================================================
//
// This process is the head-unit-side host. The assembly order follows tools/core_selftest.cpp and
// host/windows_main.cpp: platform services -> ReceiverConfig -> Receiver::start -> SDL main loop.
//
// Thread model:
//   The main thread owns SDL exclusively (events + rendering + audio device open/close).
//   The core's screen-stream/audio-stream/RTSP threads only touch SdlVideoSink / SdlAudioSink,
//   two self-locking objects; the main thread picks up results via present()/tick().
//   Cross-thread display state goes through SharedStatus (atomic).
//
// Two UI states:
//   Not connected = the launcher page fills the window (white background + Bluetooth ripple + two-tone logo + status text)
//   Connected = video fills the window (letterbox); the status bar becomes a debug overlay enabled explicitly via --statusbar
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
    bool        barOn     = false;   // --statusbar: debug overlay (off by default, to keep true full-screen video)
    bool        fullscreen = false;
    int         frames    = 0;       // >0: render N frames then exit (container smoke test)
    std::string font;                // --font, may be empty (empty = use the candidate list)
    // -- Core side --
    uint16_t    port      = 7000;
    std::string ifname;
    std::string name      = "AllPlay";
    std::string stateDir;
    std::string identityDir = "identity";
    bool        noAvahi   = false;
    bool        noVideo   = false;
    bool        noAudio   = false;
    std::string dump;                // --dump: save the last frame as PPM before exiting
    // -- Wired attach (enabled only by --wired / --dual; --wired and --bt are mutually exclusive, see the check in main) --
    bool        wiredOn   = false;
    bool        dualOn    = false;       // --dual: listen on wired + wireless at once; whichever connects first wins
    std::string ncmLinkLocal;            // --ncm-link-local: empty = fe80::2
    int         wiredPid  = -1;          // --wired-pid: restrict the product ID (unrestricted by default)
    // -- Bluetooth trigger (enabled only by --bt; without it this stays a pure receiver, behavior identical to before) --
    bool        btOn     = false;
    std::string btAddr;                 // --bt-addr: connect directly by address; empty = find by name
    std::string btDevice = "iPhone";    // --bt-device: substring match on the name
    bool        btServer = false;       // --bt-mode: defaults to client (the one that works with real devices on Linux/macOS)
    std::string btSsid;                 // --ssid: empty = use the currently connected SSID
    std::string btPass;                 // --wifi-pass: the Wi-Fi password to send the phone in 0x5703
    int         btRetry  = 10;          // --bt-retry: whole-chain retry count
    // --inject-touch U,V: after connecting, push a press/release pair into the SDL event queue; coordinates are normalized to the **video rect**.
    // In headless mode (SDL_VIDEODRIVER=dummy) there is no real input; this is the only way to test the touch path automatically.
    float       injectU   = -1.f;
    float       injectV   = -1.f;
    bool        help      = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "Usage: %s [options]\n"
        "  --screen WxH      Video surface size (default 1280x720, also the target resolution negotiated with the phone)\n"
        "  --fullscreen      Fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP)\n"
        "  --statusbar       Overlay the status bar (debugging; off by default, video fills the screen once connected)\n"
        "  --statusbar-h N   Status bar height (default 26)\n"
        "  --font PATH       Font (default DejaVu; ALLPLAY_FONT env var also works)\n"
        "  --frames N        Render N frames then exit (for headless smoke tests, e.g. SDL_VIDEODRIVER=dummy)\n"
        "  --port N          RTSP port (default 7000, auto +1 if taken)\n"
        "  --ifname IFACE     Network interface to use (auto-selected by default)\n"
        "  --name S          Name shown in Bonjour (default AllPlay)\n"
        "  --state-dir DIR   State directory (defaults to core's defaultStateDir())\n"
        "  --identity DIR    MFi identity directory (default identity)\n"
        "  --no-avahi        Do not advertise via Bonjour\n"
        "  --no-video        Do not create video output (diagnostics: protocol only, no display)\n"
        "  --no-audio        Do not create audio output (diagnostics; use SDL_AUDIODRIVER=dummy when there is no sound card)\n"
        "  --dump PATH       Save the last frame to a PPM file before exit (verify the UI unattended)\n"
        "  --inject-touch U,V Inject one touch automatically after connecting (U/V normalized 0..1 within the video rect) —\n"
        "                    for headless touch-path verification; pair with tools/fake_phone to see the printed coordinates\n"
        "  --wired           Use **wired** (USB) CarPlay: connect a cabled iPhone directly, no Bluetooth/Wi-Fi.\n"
        "                    Before running you must (all required, otherwise switching config returns BUSY):\n"
        "                      sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd\n"
        "                      pkill -f gvfsd-gphoto2\n"
        "                    Also, /dev/bus/usb is root-only, so **run the whole program with sudo**\n"
        "                    (sudo resets the environment: use sudo env ALLPLAY_LOG=… for logs,\n"
        "                      and pass --state-dir explicitly, otherwise HOME becomes /root)\n"
        "  --dual            Dual channel: listen on wired + wireless **at the same time**, whichever connects first wins (the other path\n"
        "                    blocks later connections until the session ends, then releases them). The wireless half, like wireless-only mode,\n"
        "                    relies on mDNS advertising, so do not add --no-avahi\n"
        "  --ncm-link-local A Wired: IPv6 link-local pinned on the NCM NIC (default fe80::2).\n"
        "                    The phone uses this address from 0x4301 to connect; changing it just swaps the value, but both ends must agree\n"
        "  --wired-pid N      Wired: restrict the product ID (unrestricted by default; Apple keyboards/hubs are excluded automatically)\n"
        "  --bt              Actively trigger the iPhone after startup (Bluetooth iAP2 → 0x4301), then stream the display\n"
        "  --bt-addr A:B:C:D:E:F  Connect directly by Bluetooth address (more reliable than by name; skips name matching)\n"
        "  --bt-device NAME   Device name to trigger (substring match, default iPhone)\n"
        "  --bt-mode client|server  Default client (on Linux/macOS the working mode with a real device is client: we actively\n"
        "                    connect to the phone's iAP2 SPP service; server registers SDP and waits for the phone)\n"
        "  --ssid S           Wi-Fi name to tell the phone (defaults to the one this machine is currently connected to)\n"
        "  --wifi-pass P      Wi-Fi password to tell the phone (iAP2 0x5703)\n"
        "  --bt-retry N       Whole-chain retry count after a trigger failure (default 10, 3 seconds apart)\n"
        "  -h, --help        Show this help\n"
        "\n"
        "Exit: window close / ESC / Ctrl-C.\n"
        "At runtime: with --bt press B to trigger again; with --wired press W to retry after a prepare failure.\n",
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
                std::fprintf(stderr, "Missing value for argument %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--screen")) {
            if (!parseScreen(need(a), &o->screenW, &o->screenH)) {
                std::fprintf(stderr, "--screen requires WxH, e.g. 1280x720\n");
                return false;
            }
        } else if (!std::strcmp(a, "--fullscreen")) {
            o->fullscreen = true;
        } else if (!std::strcmp(a, "--statusbar")) {
            o->barOn = true;
        } else if (!std::strcmp(a, "--statusbar-h")) {
            o->barH = std::atoi(need(a));
            if (o->barH < 10 || o->barH > 200) {
                std::fprintf(stderr, "--statusbar-h out of range (10..200)\n");
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
                std::fprintf(stderr, "--bt-mode accepts only client / server\n");
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
                std::fprintf(stderr, "--inject-touch requires U,V (each 0..1), e.g. 0.25,0.75\n");
                return false;
            }
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            o->help = true;
        } else {
            std::fprintf(stderr, "Unknown argument: %s (see -h for help)\n", a);
            return false;
        }
    }
    return true;
}

/// Touch: core's event_channel has exactly 2 slots (event_channel.cpp:20-37),
/// so we also maintain 2 here and **always send all 2** -- stable slot numbering keeps the phone from seeing
/// "slot1 suddenly appearing".
/// On desktop, mouse and touchscreen are rarely used together: the mouse always takes slot 0, fingers start from a free slot.
class TouchPad {
public:
    /// Point -> pixel ratio (high DPI). SDL mouse/FINGER events give **window point coordinates**,
    /// while videoRect in flush() is the **pixel** rect from SDL_GetRendererOutputSize.
    /// On Retina they differ by 2x: without conversion, a tap at the screen center normalizes to (0.25, 0.25),
    /// which the phone sees as "too high and too far left". (Measured: touch y is off toward the top on Mac.)
    void setScale(float sx, float sy)
    {
        scaleX_ = sx > 0.f ? sx : 1.f;
        scaleY_ = sy > 0.f ? sy : 1.f;
    }

    void handle(const SDL_Event& e, int winW, int winH)
    {
        switch (e.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.which == SDL_TOUCH_MOUSEID) break;   // mouse event synthesized by touch; ignore
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
                // FINGER coordinates are window-normalized 0..1; convert to pixels and handle uniformly with the mouse
                const int i = fingerSlot(e.tfinger.fingerId);
                if (i < 0) break;   // already 2 touches; ignore extras
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

    /// Send the current touch state to the phone. rect = the video rect after letterboxing (coordinates are normalized to it).
    /// Points landing on the black bars are not sent -- clamping would make the edges tap by accident.
    void flush(core::airplay::Receiver& rx, const SDL_Rect& rect, bool active)
    {
        if (!active) {
            if (wasActive_) {   // Lift the finger before disconnecting, so the phone does not get stuck in the pressed state
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
            // The release frame must also carry the **last position**; it cannot just send down=false with coordinates left at (0,0).
            // Otherwise the phone sees "press at the icon -> release at the top-left", which is a swipe, not a tap,
            // and tapping icons does nothing -- while dragging the map still works (the drag already happened, so no difference is visible).
            // On Android, getX() at ACTION_UP returns the last position (MainActivity.kt:222); match that here.
            if (!slots_[i].used || rect.w <= 0 || rect.h <= 0) continue;
            const float u = (slots_[i].x - float(rect.x)) / float(rect.w);
            const float v = (slots_[i].y - float(rect.y)) / float(rect.h);
            if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) continue;   // on the black bar; ignore
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
    /// used = this slot has been used (pressed / moved). Coordinates must keep being reported after release, so
    /// "should this slot take part in this report" cannot use down -- clearing down would send nothing at all.
    struct Slot { bool down = false, used = false; float x = 0, y = 0; SDL_FingerID id = -1; };

    /// Existing finger -> its original slot; otherwise find a free one (start at 1 when the mouse holds 0)
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
    float scaleX_ = 1.f;   ///< window points -> renderer pixels (see setScale)
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
    std::printf("[host] allplay_host — video surface %dx%d%s\n",
                opt.screenW, opt.screenH, opt.fullscreen ? ", fullscreen" : "");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // The launcher page is drawn in 1540x850 design coordinates and then scaled as a whole; the default nearest-neighbor sampling turns
    // rounded corners/arcs into stair steps when enlarged (especially on 1.6x screens). Switch to linear sampling so edges blend continuously.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    // ALLOW_HIGHDPI cannot be omitted: without it, macOS lets the renderer draw only as large as the "logical points"
    // backbuffer (1280x720), and the system then upscales it to Retina physical pixels (2x) -- on screen,
    // every rounded corner and every glyph is upscaled by an integer factor, which is "super blurry, completely unusable".
    // With it set, SDL_GetRendererOutputSize returns physical pixels and all drawing works in physical pixels.
    Uint32 winFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    // The status bar is drawn at y = opt.screenH (see y_ used in StatusBar::draw and bar.init below),
    // so the window must be **taller than the video area by this strip**. Passing only screenH causes two problems at once:
    //   ① the status bar falls entirely below the window's bottom edge and is invisible;
    //   ② the render loop still lays the video out using areaH = winH - barH, squeezing the 16:9 picture into 1280x(screenH-barH),
    //      adding a black bar on each side -- that is the "black bars appear as soon as --statusbar is on".
    const int winPxH = opt.screenH + (opt.barOn ? opt.barH : 0);
    SDL_Window* win = SDL_CreateWindow(
        "AllPlay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        opt.screenW, winPxH, winFlags);
    if (!win) {
        std::fprintf(stderr, "[host] Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Prefer hardware rendering + vsync; fall back to software in a display-less environment (SDL_VIDEODRIVER=dummy smoke test)
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    bool vsync = true;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        vsync = false;
    }
    if (!ren) {
        std::fprintf(stderr, "[host] Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // High-DPI self-check: only a ratio > 1 shows we are really drawing in physical pixels rather than letting the system upscale
    {
        int wpt = 0, hpt = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(win, &wpt, &hpt);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        std::printf("[host] Window %dx%d points / render target %dx%d pixels (%.2gx)\n",
                    wpt, hpt, ow, oh, wpt > 0 ? double(ow) / double(wpt) : 1.0);
    }

    allplay::SharedStatus status;
    allplay::StatusBar   bar;
    bar.init(ren, opt.screenW, opt.screenH, opt.barH);
    allplay::Launcher    launcher;
    if (!launcher.init()) {
        std::fprintf(stderr, "[host] Launcher font unavailable, drawing shapes only\n");
    }
    {
        // The launcher page prints **the name seen in the phone's Bluetooth list**, i.e. the Bluetooth adapter's name,
        // not the Bonjour advertised name (--name). Fall back to that only when the adapter name is unavailable (no Bluetooth / no permission).
        std::string btName = allplay::posix::bluetoothAdapterName();
        if (btName.empty()) btName = opt.name;
        launcher.setDeviceName(btName);
    }

    // -- Decode output: must be declared before Receiver so it outlives it --
    // The Receiver's screen-stream threads hold the video pointer until stop() returns.
    core::IVideoOutput* vid = nullptr;
    core::IAudioOutput* aud = nullptr;
#ifdef ALLPLAY_HAVE_FFMPEG
    allplay::SdlVideoSink videoSink;
    allplay::SdlAudioSink audioSink;
    if (!opt.noVideo) vid = &videoSink;
    if (!opt.noAudio) aud = &audioSink;
#else
    if (!opt.noVideo || !opt.noAudio)
        std::fprintf(stderr, "[host] This build has no ffmpeg — video/audio degraded (launcher page only)\n");
#endif

    // -- Start the protocol core --
    // Wired and wireless differ here in **ordering**, not configuration:
    //   Wireless: the Receiver starts first (wildcard bind + mDNS advertising) and lets the phone come find us -- the "address" is what we advertise.
    //   Wired: the phone **reads** the address and port from 0x4301, so the Receiver must wait until the USB side has resolved
    //         "what the NCM NIC is named and which address is pinned" before it can start (see the phase notes in wired_task.h).
    //         rxUsb.start() is deferred to that branch in the main loop.
    if (opt.wiredOn && opt.btOn) {
        std::fprintf(stderr, "[host] --wired and --bt are mutually exclusive paths (one cabled, one Bluetooth);"
                             " do not enable both\n");
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 2;
    }

    // -- Two channels --
    //   rxNet: wireless (wildcard bind + mDNS advertising); the phone comes to us;
    //   rxUsb: wired (link-local pinned on the NCM NIC; the address is given to the phone directly via 0x4301).
    // With only one path the other stays "not started"; state merging / touch forwarding both test running(),
    // so single-channel behavior is exactly as before.
    core::airplay::Receiver rxNet;
    core::airplay::Receiver rxUsb;
    const bool useWired    = opt.wiredOn || opt.dualOn;
    const bool useWireless = !opt.wiredOn || opt.dualOn;   // wireless stays off when --wired is used alone

    core::airplay::ReceiverConfig cfg;                     // the wireless one
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

    core::airplay::ReceiverConfig cfgUsb = cfg;            // the wired one: fill in ifname/bindAddr at start time
    cfgUsb.enableAvahi = false;   // no advertising on wired: the address is already given to the phone directly via 0x4301

    // Platform services must live for all of main's lifetime: the Receiver only copies them, and a Session first uses them
    // at SETUP to create its data/event channels (storing the address of a temporary is the dangling pointer from back then).
    auto services = allplay::posix::makePlatformServices();
    std::string err;

    // -- Wired attach task (--wired) --
    allplay::WiredTask        wiredTask;
    allplay::WiredTaskOptions wopt;
    bool                      wiredReceiverUp = false;  // whether the Receiver is up (assigned only here)
    std::string               wiredLastErr;             // last reported failure reason: do not spam the same reason
    // How long to wait before preparing again after the link drops. 3 s matches bt_task's retry cadence -- unplug/replug,
    // unlocking, and tapping "Trust" all take a few seconds; going faster just wastes USB enumerations.
    const uint64_t            kWiredRearmMs = 3000;
    uint64_t                  wiredRearmAt  = SDL_GetTicks64() + kWiredRearmMs;
    wopt.name         = opt.name;
    wopt.mfiDir       = opt.identityDir;
    wopt.stateDir     = opt.stateDir;   // pairing material lives here: without it the trust prompt pops up every time
    wopt.ncmLinkLocal = opt.ncmLinkLocal;
    wopt.productId    = opt.wiredPid;
    auto wiredLog = [](const std::string& s) {
        std::printf("[wired] %s\n", s.c_str());
        std::fflush(stdout);
    };

    // Dual-channel mutual exclusion: first to connect wins. Each gate looks at the **other** side's state -- if the other is in use (or handshaking)
    // the new connection is closed outright. Once the session ends, the other side returns to idle and the gate opens automatically.
    // In single-channel mode the other path is never started (connecting/sessionActive are always false), so it is effectively not installed.
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
            // In dual-channel mode a failed wireless start is not fatal (e.g. only wired is available, or there is no Wi-Fi NIC):
            // the wired path can still connect. When wireless is the only path, this is a startup failure.
            if (!opt.dualOn) {
                std::fprintf(stderr, "[host] Core startup failed: %s\n", err.c_str());
                SDL_DestroyRenderer(ren);
                SDL_DestroyWindow(win);
                SDL_Quit();
                return 1;
            }
            std::printf("[host] Dual channel: wireless half failed to start (%s) — continuing wired only\n", err.c_str());
        } else {
            std::printf("[host] Wireless RTSP :%u  ifname=%s ip=%s deviceid=%s\n",
                        rxNet.port(), rxNet.ifname().c_str(), rxNet.ip().c_str(),
                        rxNet.info().deviceId.c_str());
            if (cfg.enableAvahi) {
                std::printf("[host] Bonjour: %s\n",
                            rxNet.avahiError().empty()
                                ? "advertised _airplay._tcp"
                                : ("unavailable (" + rxNet.avahiError() + ")").c_str());
            }
            std::printf("[host] READY — waiting for iPhone connection (fake phone: tools/fake_phone)\n");
        }
        installGates();
    }
    if (useWired) {
        std::printf("[host] %s: preparing USB link…\n"
                    "       The phone shows a \"Trust This Computer\" prompt on first connect — tap it; while locked it reports only 1 configuration,\n"
                    "       so unlock it first (this is not a cable problem).\n",
                    opt.dualOn ? "dual channel (wired half)" : "wired mode");
        if (!wiredTask.beginBringUp(wopt, wiredLog)) {
            std::fprintf(stderr, "[host] Wired task failed to start\n");
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }
    std::fflush(stdout);

    // -- Bluetooth trigger (--bt) --
    // Must come after rx.start(): the trigger parameters use rx.ip()/port()/info(), which are only valid after startup.
    // The trigger itself runs on BtTask's own thread (runBtTrigger blocks fully; on the main thread it would freeze the picture).
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
        std::fprintf(stderr, "[host] Bluetooth trigger failed to start\n");
    }

    TouchPad pad;

    bool     running   = true;
    int      rendered  = 0;
    int      injectStep = 0;   ///< frame counter after connecting; --inject-touch's press/release land on fixed frames
    bool     wasStreaming = false;
    Uint64   lastFpsAt = SDL_GetTicks64();
    uint64_t lastFrames = 0;

    // The mouse press that just gained focus does not count as "clicking a button".
    // macOS's SDLView implements acceptsFirstMouse -- a click that activates the window is also
    // delivered here; if it happens to land on "Cancel", the launcher flashes by and it looks like "it never started".
    Uint64   focusAt     = SDL_GetTicks64();
    constexpr Uint64 kClickGuardMs = 300;

    // --bt persistent reconnect: after the phone leaves/drops, call it again after a while (head-unit behavior).
    // The trigger itself runs at most o.retry rounds; when done, btTask.done() becomes true and only then may it start again.
    constexpr Uint64 kBtRearmMs = 5000;
    Uint64   btRearmAt = SDL_GetTicks64() + kBtRearmMs;
    Uint64   btWaitingSince = 0;   ///< how long the trigger thread has stayed in WaitingPair (watchdog)

    while (running && !g_stop) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_w &&
                     useWired && !wiredReceiverUp) {
                // The phone got unlocked, the cable was replugged, nobody tapped "Trust" earlier... no need to restart the program.
                // No redo once the Receiver is up: that link was already reported via 0x4301 and cannot be swapped out behind the phone's back.
                if (wiredTask.beginBringUp(wopt, wiredLog)) {
                    wiredLastErr.clear();   // manual redo: the failure reason deserves reporting again from scratch
                    std::printf("[host] Re-preparing wired link…\n");
                    std::fflush(stdout);
                } else {
                    std::printf("[host] Previous prepare is still running, try W again shortly\n");
                    std::fflush(stdout);
                }
            }
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_b && opt.btOn) {
                // after a failed trigger or the phone dropping, no need to restart the program
                if (btTask.start(rxNet, btOpt, btLog)) {
                    std::printf("[host] Re-triggering Bluetooth…\n");
                    std::fflush(stdout);
                } else {
                    std::printf("[host] Previous trigger is still running, try B again shortly\n");
                    std::fflush(stdout);
                }
            }
            else if (e.type == SDL_WINDOWEVENT &&
                     e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                focusAt = SDL_GetTicks64();
            }
            else if (!wasStreaming && e.type == SDL_MOUSEBUTTONDOWN &&
                     SDL_GetTicks64() - focusAt < kClickGuardMs) {
                // The click that activates the window: swallow it, do not treat it as clicking "Cancel" (and do not feed it to the touch pad)
            }
            else if (!wasStreaming && launcher.handleEvent(e, opt.screenW, opt.screenH)) {
                std::printf("[host] Launcher: \"Cancel\" clicked, exiting\n");
                std::fflush(stdout);
                running = false;
            }
            else pad.handle(e, opt.screenW, opt.screenH);
        }

#ifdef ALLPLAY_HAVE_FFMPEG
        // The audio device may only be opened/closed on the main thread (SDL's rule); configure() from the RTSP thread only records a pending change.
        // It returns immediately when there is no change, so calling it every frame here costs nothing.
        audioSink.serviceDevice();
        status.audioFrames.store(audioSink.decoded());
        status.audioUnderruns.store(audioSink.underruns());
#endif

        // -- Wired: the link must recover by itself --
        // After a session ends (cable unplugged, phone locked, re-enumeration) runPump sets the phase to Done, while
        // wiredReceiverUp is still set -- leave it set and the only way out is restarting:
        // even W is blocked by !wiredReceiverUp. Clear it + cool down a few seconds to retry automatically; just replug.
        if (useWired && wiredReceiverUp && wiredTask.pumpDone()) {
            std::printf("[host] Wired link dropped — shutting down wired RTSP, auto re-preparing in %llu seconds\n",
                        static_cast<unsigned long long>(kWiredRearmMs / 1000));
            std::fflush(stdout);
            rxUsb.stop();              // the NIC may have vanished entirely (re-enumeration), so the listener must restart too
            wiredReceiverUp = false;
            wiredRearmAt    = SDL_GetTicks64() + kWiredRearmMs;
        }

        // -- Wired: start the Receiver only after the USB side is ready (**the only** place in the program that calls rxUsb.start) --
        if (useWired && !wiredReceiverUp) {
            const allplay::WiredPhase wp = wiredTask.phase();
            if (wp == allplay::WiredPhase::Ready) {
                const allplay::WiredBringUp& bu = wiredTask.result();
                cfgUsb.ifname   = bu.ifname;
                cfgUsb.bindAddr = bu.bindAddr;   // link-local; server.cpp appends %iface itself
                if (!rxUsb.start(cfgUsb, services, &err)) {
                    // Failure here is basically always one thing: port 7000 is taken on that NIC.
                    // In dual-channel mode the wireless half is still running, so do not exit; wait for the next retry.
                    std::fprintf(stderr, "[host] Wired: Receiver failed to start: %s\n", err.c_str());
                    if (!useWireless || !rxNet.running()) running = false;
                    else wiredRearmAt = SDL_GetTicks64() + kWiredRearmMs;
                } else {
                    wiredReceiverUp = true;
                    std::printf("[host] Wired RTSP [%s%%%s]:%u (deviceid %s)\n",
                                rxUsb.ip().c_str(), rxUsb.ifname().c_str(), unsigned(rxUsb.port()),
                                rxUsb.info().deviceId.c_str());
                    // The port/deviceid/public key reported in 0x4301 all come from rxUsb itself -- see the startPump comment
                    installGates();
                    if (!wiredTask.startPump(rxUsb, wiredLog)) {
                        std::printf("[host] Wired: the 0x4301 branch failed to start\n");
                    }
                    std::fflush(stdout);
                }
            } else if (wp == allplay::WiredPhase::Failed && wiredTask.error() != wiredLastErr) {
                // Report the same reason only once: with no phone plugged in it retries every 3 s, and spam is pointless.
                // A changed reason (e.g. from "no device found" to "pairing timed out") means things are moving; report it again.
                wiredLastErr = wiredTask.error();
                std::printf("[host] Wired prepare failed: %s\n"
                            "       (will retry automatically; unlock the phone if locked and tap \"Trust\" if you have not)\n",
                            wiredLastErr.c_str());
                std::fflush(stdout);
            }
            // If not ready, retry by itself after the cooldown -- after replugging, unlocking, or tapping "Trust" no keypress is needed.
            // Placed after the block above: let the failure reason be seen first, then retry.
            if (!wiredReceiverUp &&
                (wp == allplay::WiredPhase::Failed || wp == allplay::WiredPhase::Done) &&
                SDL_GetTicks64() >= wiredRearmAt) {
                if (wiredTask.beginBringUp(wopt, wiredLog))
                    wiredRearmAt = SDL_GetTicks64() + kWiredRearmMs;
            }
        }

        // -- Core state -> LinkState (both channels merged into one state) --
        // Rule: if either is presenting video it is Streaming; if either is handshaking it is Connecting;
        // if at least one is listening it is WaitingPair (even if the other is still preparing).
        allplay::LinkState st;
        if (rxUsb.sessionActive() || rxNet.sessionActive())
            st = allplay::LinkState::Streaming;
        else if (rxUsb.connecting() || rxNet.connecting())
            st = allplay::LinkState::Connecting;
        else if (rxUsb.running() || rxNet.running())
            st = allplay::LinkState::WaitingPair;
        else if (useWired && wiredTask.phase() == allplay::WiredPhase::Failed)
            // Not up yet is not an "error" but "still preparing" -- reporting Error would make the launcher page say "Error",
            // turning "phone locked, waiting for someone to tap Trust" into a fault. Report Error only on a real failure.
            st = allplay::LinkState::Error;
        else
            st = allplay::LinkState::Connecting;
        status.state.store(static_cast<int>(st));

        // -- Bluetooth persistent reconnect (--bt): call again automatically after the phone leaves/drops --
        // Head-unit behavior is "the accessory stays on and the phone reconnects on return", with no button press needed.
        // Call only when all three hold: the previous trigger thread has finished, both channels are idle (WaitingPair),
        // and the cooldown has passed -- otherwise it fights an in-progress handshake (calling again while connected disrupts the link).
        // But the trigger thread is now "persistent": after sending 0x4301 it keeps guarding the iAP2 link, and when the phone leaves
        // it does not necessarily finish. If it sits in WaitingPair too long (the phone took 0x4301 but never connects
        // to AirPlay), cancel it and call again -- otherwise it would never trigger again.
        constexpr Uint64 kBtParkedMs = 45000;
        if (opt.btOn && !btTask.done() && st == allplay::LinkState::WaitingPair) {
            if (btWaitingSince == 0) btWaitingSince = SDL_GetTicks64();
            else if (SDL_GetTicks64() - btWaitingSince > kBtParkedMs) {
                btTask.stop();
                btRearmAt = SDL_GetTicks64() + kBtRearmMs;
                btWaitingSince = 0;
                std::printf("[host] Bluetooth link idle too long — cancelling, preparing to re-trigger\n");
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
                std::printf("[host] Phone gone — Bluetooth re-triggered automatically, waiting for it to return\n");
                std::fflush(stdout);
            }
        }

        // Send touch to the **active** channel (in dual-channel mode only one can connect)
        auto activeRx = [&]() -> core::airplay::Receiver& {
            if (rxUsb.running() && (rxUsb.sessionActive() || rxUsb.connecting())) return rxUsb;
            return rxNet;
        };

        // Go fullscreen the moment Streaming starts (matching Android: fill the screen on connect)
        const bool streaming = (st == allplay::LinkState::Streaming);
        if (streaming != wasStreaming) {
            wasStreaming = streaming;
            pad.reset();
            std::printf("[host] Session %s\n", streaming ? "started — switching to video" : "ended — back to launcher");
            std::fflush(stdout);
            if (opt.btOn && !streaming) {
                // Session ended: take down the trigger thread still attached to the iAP2 link, so the rearm logic
                // can call 0x4301 again when the phone returns (otherwise it guards the old link forever).
                btTask.stop();
                btRearmAt = SDL_GetTicks64() + kBtRearmMs;
                btWaitingSince = 0;
            }
            if (streaming && opt.fullscreen) {
                SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
#ifdef ALLPLAY_HAVE_FFMPEG
            // The last frame must be dropped when the session ends: the sink keeps redrawing the frame in its texture
            // ("the picture freezes instead of disappearing" is designed for mid-session frame drops),
            // keeping it would make the "back to the launcher page" line a lie -- the picture would stay stuck on the last frame.
            if (!streaming) videoSink.dropFrame();
#endif
        }

        int winW = 0, winH = 0;
        SDL_GetRendererOutputSize(ren, &winW, &winH);

        // High DPI: the render surface is physical pixels and the window size is logical points; their ratio is used by the launcher page to convert mouse coordinates
        {
            int wpt = winW, hpt = winH;
            SDL_GetWindowSize(win, &wpt, &hpt);
            launcher.setPixelRatio(wpt > 0 ? double(winW) / double(wpt) : 1.0);
            // Same for touch: events are in "points", videoRect is in "pixels" (see TouchPad::setScale)
            pad.setScale(wpt > 0 ? float(double(winW) / double(wpt)) : 1.f,
                         hpt > 0 ? float(double(winH) / double(hpt)) : 1.f);
        }

        // The status bar takes up space only when explicitly enabled
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

        // The readback must happen **before** Present. GL's backbuffer contents are undefined after Present,
        // and on llvmpipe/Wayland it reads back all black (on Mac it happens to still read fine, which is why this bug stayed hidden).
        if (!opt.dump.empty() && opt.frames > 0 && rendered == opt.frames - 1) {
            allplay::dumpRender(ren, opt.dump);
        }
        SDL_RenderPresent(ren);

        pad.flush(activeRx(), videoRect, streaming);

        // -- Headless touch injection (--inject-touch) --
        // Pushed into the SDL event queue; when the next SDL_PollEvent picks it up it takes the **exactly same**
        // TouchPad::handle path as a real mouse; the only difference is it bypasses the X11/Wayland input driver.
        // Timing: press on frame 20 after connecting, release on frame 50 -- 30 frames apart, so the phone can clearly see
        // "press" and "release" as two independent reports.
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
                ev.button.which    = 0;          // a real mouse (only SDL_TOUCH_MOUSEID is touch-synthesized)
                ev.button.clicks   = 1;
                ev.button.x        = px;
                ev.button.y        = py;
                SDL_PushEvent(&ev);
                std::printf("[host] Injecting touch %s: window(%d,%d) video rect %dx%d@(%d,%d) → normalized %.3f,%.3f\n",
                            down ? "down" : "up",
                            int(px), int(py), videoRect.w, videoRect.h, videoRect.x, videoRect.y,
                            double(opt.injectU), double(opt.injectV));
                std::fflush(stdout);
            }
        } else if (!streaming) {
            injectStep = 0;   // restart the count after a disconnect/reconnect so the second session does not skip the injection points
        }

        // Display fps over a 1-second window
        const Uint64 now = SDL_GetTicks64();
        if (now - lastFpsAt >= 1000) {
            const uint64_t f = status.frames.load();
            status.fps.store(int(f - lastFrames));
            lastFrames = f;
            lastFpsAt  = now;
            status.lastFrameMs.store(now);
        }

        if (opt.frames > 0 && ++rendered >= opt.frames) {
            std::printf("[host] Smoke test: rendered %d frames, exiting normally\n", rendered);
            break;
        }
        if (!vsync) SDL_Delay(16);
    }

    std::printf("[host] Shutting down: stopping core…\n");
    std::fflush(stdout);
    btTask.stop();      // detach the trigger thread (runBtTrigger has no cancellation point; see host/bt_task.cpp)
    // Wired: stop the attach task first (join pump; it checks the stop flag every 200 ms), then stop the core.
    // Order must not be reversed -- the pump thread still holds the iAP2 link (link in Impl); let it let go first.
    wiredTask.stop();
    rxNet.stop();       // Stop the core first (join the screen-stream/audio-stream threads), then let the sinks destruct
    rxUsb.stop();       // Stop both channels: the accept threads read the other side's state (the gate); leave no dangling references
#ifdef ALLPLAY_HAVE_FFMPEG
    // Audio is not judged automatically for "is there sound" (in headless runs the device is dummy and silent);
    // what is given here is **machine-checkable** evidence: how many frames decoded (matching the phone's sent frame count proves the decode chain works),
    // how many buffer underruns and how many forced drops. ⚠️ Headless underruns are not a sound-quality metric: the dummy sound card does not
    // call back in real time, so all jitter is charged to it; to judge sound quality, run on a machine with a real sound card.
    std::printf("[host] Audio: decoded %llu frames, buffer underruns %llu (not a sound-quality metric on a dummy card),"
                " dropped oldest on overflow %llu times%s\n",
                (unsigned long long)audioSink.decoded(),
                (unsigned long long)audioSink.underruns(),
                (unsigned long long)audioSink.dropped(),
                audioSink.deviceOpen() ? "" : " (device not open)");
#endif
    launcher.releaseTextures();   // text textures are children of the renderer; release them before destroying the renderer
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::printf("[host] Exited\n");
    return 0;
}
