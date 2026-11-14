// ============================================================================
// host/windows_main.cpp -- Windows desktop host (1280x720 Win32 window)
// ============================================================================
// Starts the Receiver (RTSP + screen-stream/audio-stream network layer) and draws the same launcher page as Android:
// white background + two-tone AllPlay logo + blue Bluetooth badge (ripple) + status text.
// Video decode/presentation still needs ffmpeg wired in; video/audio output currently uses stubs (count only, no rendering).
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include "windows/net_windows.h"

#include "airplay/server.h"
#include "common/log.h"
#include "port/media.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace {

constexpr int kWinW = 1280;
constexpr int kWinH = 720;

core::airplay::Receiver* g_receiver = nullptr;
std::atomic<bool>       g_running{true};

// Ripple animation progress 0..1 (timer-driven)
double g_ripple = 0.0;

// ---------------------------------------------------------------------------
// Video/audio stubs: let the screen stream (TCP) and audio stream (UDP) actually establish, but do not decode or render
// ---------------------------------------------------------------------------
class StubVideo : public core::IVideoOutput {
public:
    void configure(core::VideoCodec, int w, int h, const std::vector<uint8_t>&) override
    {
        LOG_INFO("Video output stub: configure %dx%d", w, h);
    }
    bool feedAnnexB(const uint8_t*, size_t n) override
    {
        static std::atomic<uint64_t> frames{0};
        const uint64_t f = ++frames;
        if (f == 1 || f % 300 == 0) LOG_INFO("Video output stub: frame %llu, %zu bytes", (unsigned long long)f, n);
        return true;
    }
    void reset() override {}
    void flush() override {}
};

class StubAudio : public core::IAudioOutput {
public:
    void configure(core::AudioCodec c, int rate, int ch) override
    {
        LOG_INFO("Audio output stub: configure codec=%d %dHz %dch", int(c), rate, ch);
    }
    void feed(const uint8_t*, size_t n, uint32_t) override
    {
        static std::atomic<uint64_t> pkts{0};
        const uint64_t p = ++pkts;
        if (p == 1 || p % 500 == 0) LOG_INFO("Audio output stub: packet %llu, %zu bytes", (unsigned long long)p, n);
    }
    void reset() override {}
};

StubVideo g_video;
StubAudio g_audio;

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawCenteredText(HDC dc, const std::string& text, int cx, int cy, COLORREF color,
                      int height, int weight)
{
    HFONT f = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT old = static_cast<HFONT>(SelectObject(dc, f));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT r{cx - 300, cy - height, cx + 300, cy + height};
    DrawTextA(dc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(f);
}

void drawLauncher(HDC dc)
{
    // White background
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT full{0, 0, kWinW, kWinH};
    FillRect(dc, &full, white);
    DeleteObject(white);

    const int cx = kWinW / 2;
    const int titleY = 180;
    const int badgeCx = cx;
    const int badgeCy = 360;
    const int badgeR = 42;

    // Two-tone AllPlay logo: draw "All" dark first, then "Play" blue (two TextOut calls, for simplicity)
    drawCenteredText(dc, "AllPlay", cx, titleY, RGB(30, 30, 30), 40, FW_BOLD);

    // Ripple rings (two rings, expanding and fading)
    HPEN ringPen = CreatePen(PS_SOLID, 3, RGB(47, 124, 246));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, ringPen));
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, hollow));
    for (int i = 0; i < 2; ++i) {
        double p = g_ripple + i * 0.5;
        p -= std::floor(p);   // 0..1
        int r = badgeR + int(p * (badgeR * 2));
        Ellipse(dc, badgeCx - r, badgeCy - r, badgeCx + r, badgeCy + r);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(ringPen);

    // Blue circle + white Bluetooth symbol
    HBRUSH blue = CreateSolidBrush(RGB(47, 124, 246));
    oldBrush = static_cast<HBRUSH>(SelectObject(dc, blue));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen2 = static_cast<HPEN>(SelectObject(dc, noPen));
    Ellipse(dc, badgeCx - badgeR, badgeCy - badgeR, badgeCx + badgeR, badgeCy + badgeR);
    SelectObject(dc, oldPen2);
    SelectObject(dc, oldBrush);
    DeleteObject(noPen);
    DeleteObject(blue);
    drawCenteredText(dc, "B", badgeCx, badgeCy, RGB(255, 255, 255), 36, FW_BOLD);

    // Status text
    std::string status = "Waiting for connection";
    if (g_receiver && g_receiver->sessionActive()) status = "Connected";
    else if (g_receiver && g_receiver->connecting()) status = "Connecting…";
    drawCenteredText(dc, status, cx, 470, RGB(102, 102, 102), 20, FW_NORMAL);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        drawLauncher(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {           // ripple animation
            g_ripple += 0.016;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == 2) {    // status refresh
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main()
{
    std::string err;
    if (!allplay::windows::initWinsock(&err)) {
        LOG_ERROR("Winsock initialization failed: %s", err.c_str());
        return 1;
    }

    // Composition root: Windows platform capabilities
    auto services = allplay::windows::makePlatformServices();

    core::airplay::ReceiverConfig cfg;
    cfg.name      = "AllPlay";
    cfg.width     = 1280;
    cfg.height    = 720;
    cfg.fps       = 60;
    cfg.stateDir  = ".";
    cfg.mfiDir    = "identity";      // relative to cwd; may be changed to an absolute path
    cfg.btMac     = "";
    cfg.video     = &g_video;
    cfg.audio     = &g_audio;
    cfg.enableAvahi = false;         // mDNS stub; disabled to avoid noise

    core::airplay::Receiver receiver;
    if (!receiver.start(cfg, services, &err)) {
        LOG_ERROR("Receiver failed to start: %s", err.c_str());
        return 1;
    }
    g_receiver = &receiver;
    LOG_INFO("AllPlay Windows host started: RTSP port %u, IP %s", receiver.port(), receiver.ip().c_str());

    // Window
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AllPlayHost";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClassW(&wc);

    RECT rc{0, 0, kWinW, kWinH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"AllPlayHost", L"AllPlay", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LOG_ERROR("Window creation failed"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 1, 33, nullptr);    // ripple ~30fps
    SetTimer(hwnd, 2, 500, nullptr);   // status 500ms

    // Message loop
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_running = false;
    receiver.stop();
    return 0;
}
