#include "launcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846   // MSVC's <cmath> does not define it
#endif

namespace allplay {

namespace {

// -- Design coordinate system: 1540x850 (= the 1540x850 design web page); scaled and centered as a whole at render time --
constexpr int kDesignW = 1540;
constexpr int kDesignH = 850;

// Colors come from the design's CSS variables; do not change them casually
constexpr SDL_Color kInk        = {0x17, 0x19, 0x1F, 255};   // --ink
constexpr SDL_Color kMuted      = {0x73, 0x79, 0x85, 255};   // --muted
constexpr SDL_Color kBlue       = {0x28, 0x78, 0xF0, 255};   // --blue
constexpr SDL_Color kEyebrow    = {0x5D, 0x64, 0x70, 255};
constexpr SDL_Color kHint       = {0x85, 0x8B, 0x96, 255};
constexpr SDL_Color kFooterInk  = {0x4E, 0x55, 0x60, 255};   // Footer must not be too light: 14px gray text is hard to read (both segments share this color)
constexpr SDL_Color kLangInk    = {0x30, 0x36, 0x42, 255};
constexpr SDL_Color kRule       = {0xF0, 0xF1, 0xF4, 255};
constexpr SDL_Color kCardLine   = {0xE8, 0xEB, 0xF1, 255};
constexpr SDL_Color kLangLine   = {0xE5, 0xE8, 0xEE, 255};
constexpr SDL_Color kVisualLine = {0xED, 0xF0, 0xF4, 255};
constexpr SDL_Color kVisualBg   = {0xFA, 0xFB, 0xFD, 255};
constexpr SDL_Color kTileBg     = {0xED, 0xF5, 0xFF, 255};
constexpr SDL_Color kGreen      = {0x25, 0xB8, 0x78, 255};   // Footer status dot
constexpr SDL_Color kPhoneGreen = {0x25, 0xC7, 0x7A, 255};   // Play icon inside the phone
constexpr SDL_Color kRed        = {0xE5, 0x48, 0x4D, 255};
// The Bluetooth icon's own blue (sampled from the image the user provided: sky blue, lighter than --blue)
constexpr SDL_Color kIconBlue   = {0x1E, 0x9B, 0xE0, 255};

// Layout constants (design pixels)
constexpr int kHeaderH   = 88;
constexpr int kMarginX   = 76;
constexpr int kMainW     = 1280;
constexpr int kLeftPad   = 24;
constexpr int kColGap    = 100;
constexpr int kVisualH   = 330;
constexpr int kCardH     = 88;
constexpr int kFooterH   = 54;

// -- Basic primitives --
//
// Everything is drawn with SDL_RenderGeometry (**floating-point vertices**).
// Why not per-row integer scanlines: that approach quantizes every coordinate to 1 design pixel, which on Retina
// becomes 1.66 device pixels -- so rounded corners, rings and small icons are all stair-stepped. Supersampling can only
// smooth the step edges a little, not remove the steps themselves (this is what the user saw as "the Bluetooth icon is very blurry").
// Handed to the GPU, vertices are first converted to device coordinates by the render scale, giving device-pixel-accurate edges.

/// Current draw color (callers still set it with SDL_SetRenderDrawColor; read back here as the vertex color)
SDL_Color curColor(SDL_Renderer* r)
{
    SDL_Color c{0, 0, 0, 255};
    SDL_GetRenderDrawColor(r, &c.r, &c.g, &c.b, &c.a);
    return c;
}

/// Triangle collection buffer: scene rebuilds are single-threaded, so reuse one block instead of mallocing every time
std::vector<SDL_Vertex>& triBuf()
{
    static std::vector<SDL_Vertex> buf;
    buf.clear();
    return buf;
}

SDL_Vertex vert(double x, double y, SDL_Color c)
{
    SDL_Vertex v;
    v.position.x = float(x);
    v.position.y = float(y);
    v.color      = c;
    v.tex_coord.x = 0;
    v.tex_coord.y = 0;
    return v;
}

void flushTris(SDL_Renderer* r, const std::vector<SDL_Vertex>& v)
{
    if (v.size() >= 3)
        SDL_RenderGeometry(r, nullptr, v.data(), int(v.size()), nullptr, 0);
}

/// Push a filled circle into the buffer (does not flush; the caller flushes -- the Bluetooth icon draws hundreds of small circles at once)
void emitCircle(std::vector<SDL_Vertex>& v, double cx, double cy, double R, SDL_Color c)
{
    const int n = std::clamp(int(R * 4.0), 10, 96);
    for (int i = 0; i < n; ++i) {
        const double a0 = 2 * M_PI * i / n, a1 = 2 * M_PI * (i + 1) / n;
        v.push_back(vert(cx, cy, c));
        v.push_back(vert(cx + R * std::cos(a0), cy + R * std::sin(a0), c));
        v.push_back(vert(cx + R * std::cos(a1), cy + R * std::sin(a1), c));
    }
}

void fillCircle(SDL_Renderer* r, int cx, int cy, int rad)
{
    if (rad <= 0) return;
    auto& v = triBuf();
    emitCircle(v, cx, cy, rad + 0.5, curColor(r));   // +0.5: matches the old scanline coverage
    flushTris(r, v);
}

void fillEllipse(SDL_Renderer* r, int cx, int cy, int rx, int ry)
{
    if (rx <= 0 || ry <= 0) return;
    const SDL_Color c = curColor(r);
    const int n = std::clamp(int(std::max(rx, ry) * 4.0), 12, 128);
    auto& v = triBuf();
    for (int i = 0; i < n; ++i) {
        const double a0 = 2 * M_PI * i / n, a1 = 2 * M_PI * (i + 1) / n;
        v.push_back(vert(cx, cy, c));
        v.push_back(vert(cx + (rx + 0.5) * std::cos(a0), cy + (ry + 0.5) * std::sin(a0), c));
        v.push_back(vert(cx + (rx + 0.5) * std::cos(a1), cy + (ry + 0.5) * std::sin(a1), c));
    }
    flushTris(r, v);
}

/// Ring (rad = outer radius, thick = ring width)
void strokeRing(SDL_Renderer* r, int cx, int cy, int rad, int thick)
{
    if (rad <= 0 || thick <= 0) return;
    const double ro = rad + 0.5;
    const double ri = rad - thick - 0.5;
    if (ri <= 0.5) {
        fillCircle(r, cx, cy, rad);
        return;
    }
    const SDL_Color c = curColor(r);
    const int n = std::clamp(int(ro * 4.0), 16, 128);
    auto& v = triBuf();
    for (int i = 0; i < n; ++i) {
        const double a0 = 2 * M_PI * i / n, a1 = 2 * M_PI * (i + 1) / n;
        const double c0 = std::cos(a0), s0 = std::sin(a0);
        const double c1 = std::cos(a1), s1 = std::sin(a1);
        v.push_back(vert(cx + ro * c0, cy + ro * s0, c));
        v.push_back(vert(cx + ri * c0, cy + ri * s0, c));
        v.push_back(vert(cx + ro * c1, cy + ro * s1, c));
        v.push_back(vert(cx + ri * c0, cy + ri * s0, c));
        v.push_back(vert(cx + ri * c1, cy + ri * s1, c));
        v.push_back(vert(cx + ro * c1, cy + ro * s1, c));
    }
    flushTris(r, v);
}

void fillRoundRect(SDL_Renderer* r, int x, int y, int w, int h, int rad)
{
    if (w <= 0 || h <= 0) return;
    const SDL_Color c = curColor(r);
    const double x0 = x - 0.5, y0 = y - 0.5;      // old scanline pixel boundary
    const double x1 = x0 + w, y1 = y0 + h;
    double R = rad;
    if (R * 2 > w) R = w / 2.0;
    if (R * 2 > h) R = h / 2.0;

    auto& v = triBuf();
    if (R <= 0.5) {                                // square-cornered rectangle
        v.push_back(vert(x0, y0, c)); v.push_back(vert(x1, y0, c));
        v.push_back(vert(x1, y1, c));
        v.push_back(vert(x0, y0, c)); v.push_back(vert(x1, y1, c));
        v.push_back(vert(x0, y1, c));
        flushTris(r, v);
        return;
    }

    // Outline: each of the four corners draws a 90-degree arc, and the ends naturally connect into four straight edges (convex shape, fan-triangulated)
    struct Corner { double cx, cy, a0; };
    const Corner corners[4] = {
        {x0 + R, y0 + R, M_PI},          // top-left: pi -> 3pi/2
        {x1 - R, y0 + R, -M_PI / 2},     // top-right: -pi/2 -> 0
        {x1 - R, y1 - R, 0},             // bottom-right: 0 -> pi/2
        {x0 + R, y1 - R, M_PI / 2},      // bottom-left: pi/2 -> pi
    };
    const int seg = std::clamp(int(R * 1.5), 3, 24);
    std::vector<SDL_FPoint> ring;
    ring.reserve(size_t(4 * (seg + 1)));
    for (const Corner& k : corners)
        for (int i = 0; i <= seg; ++i) {
            const double a = k.a0 + (M_PI / 2) * i / seg;
            ring.push_back(SDL_FPoint{float(k.cx + R * std::cos(a)),
                                      float(k.cy + R * std::sin(a))});
        }

    const double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    for (size_t i = 0; i < ring.size(); ++i) {
        const SDL_FPoint& p = ring[i];
        const SDL_FPoint& q = ring[(i + 1) % ring.size()];
        v.push_back(vert(mx, my, c));
        v.push_back(vert(p.x, p.y, c));
        v.push_back(vert(q.x, q.y, c));
    }
    flushTris(r, v);
}

/// Rounded-rect stroke: fill with the border color, then cover the inside with the fill color (backgrounds are solid, so this suffices).
void strokeRoundRect(SDL_Renderer* r, int x, int y, int w, int h, int rad, int t,
                     SDL_Color border, SDL_Color fill)
{
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    fillRoundRect(r, x, y, w, h, rad);
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    fillRoundRect(r, x + t, y + t, w - 2 * t, h - 2 * t, rad > t ? rad - t : 0);
}

/// Card shadow: a stack of faint rounded rects, same width, growing only downward -- the farther from the bottom edge, the fewer layers,
/// giving a shadow that fades downward. It does not spill left or right, to avoid a gray edge peeking out beside the card (SDL has no blur).
void softShadow(SDL_Renderer* r, int x, int y, int w, int h, int rad)
{
    SDL_SetRenderDrawColor(r, 0x1A, 0x26, 0x3A, 3);
    for (int i = 9; i >= 1; --i)
        fillRoundRect(r, x, y, w, h + i, rad);
}

/// Blue soft glow behind the diagram (the radial-gradient in the design)
void softGlow(SDL_Renderer* r, int cx, int cy, int rx, int ry)
{
    SDL_SetRenderDrawColor(r, 0x28, 0x78, 0xF0, 2);
    for (int i = 0; i < 14; ++i) {
        const double k = 1.0 - i * 0.055;
        fillEllipse(r, cx, cy, int(rx * k), int(ry * k));
    }
}

/// An arc (in degrees, 0 = +x direction, clockwise positive)
/// The diagram currently uses full-ring expanding ripples, so nothing calls this for now; kept for a "signal arcs on both sides" style.
[[maybe_unused]] void strokeArc(SDL_Renderer* r, int cx, int cy, int rad,
                                double a0, double a1, int thick)
{
    const double step = 1.5;
    const int    hr   = thick > 1 ? thick / 2 : 0;
    for (double a = a0; a <= a1; a += step) {
        const int px = cx + int(rad * std::cos(a * M_PI / 180.0));
        const int py = cy + int(rad * std::sin(a * M_PI / 180.0));
        if (hr == 0) SDL_RenderDrawPoint(r, px, py);
        else         fillCircle(r, px, py, hr);
    }
}

void fillTriangle(SDL_Renderer* r, SDL_Point a, SDL_Point b, SDL_Point c)
{
    const SDL_Color col = curColor(r);
    auto& v = triBuf();
    v.push_back(vert(a.x, a.y, col));
    v.push_back(vert(b.x, b.y, col));
    v.push_back(vert(c.x, c.y, col));
    flushTris(r, v);
}

/// A thick arc: place a string of small circles along the line, giving naturally round ends (matching the icon's rounded strokes)
void emitArc(std::vector<SDL_Vertex>& v, double cx, double cy, double R,
             double a0, double a1, double thick, SDL_Color c)
{
    const double len   = std::fabs(a1 - a0) * R;
    const int    steps = std::max(2, int(std::ceil(len / std::max(0.6, thick * 0.5))));
    for (int i = 0; i <= steps; ++i) {
        const double a = a0 + (a1 - a0) * double(i) / steps;
        emitCircle(v, cx + R * std::cos(a), cy + R * std::sin(a), thick / 2, c);
    }
}

/// A thick straight segment (round caps)
void emitCapsule(std::vector<SDL_Vertex>& v, double ax, double ay,
                 double bx, double by, double thick, SDL_Color c)
{
    const double len   = std::hypot(bx - ax, by - ay);
    const int    steps = std::max(1, int(std::ceil(len / std::max(0.6, thick * 0.5))));
    for (int i = 0; i <= steps; ++i) {
        const double t = double(i) / steps;
        emitCircle(v, ax + (bx - ax) * t, ay + (by - ay) * t, thick / 2, c);
    }
}

/// Bluetooth badge (the icon the user specified): thick round-capped glyph + two signal arcs on the right, **no outer ring**.
/// Both badge spots in the UI call this, so they look identical.
/// The glyph traces the official logo's six-point polyline (bottom-left end -> top-right tip -> ridge top -> ridge bottom ->
/// bottom-right tip -> top-left end), proportions measured from the icon; both arcs are centered on the glyph, inner arc at 0.81 half-height, outer at 1.33,
/// opening +/-36 degrees.
///
/// pulse in [0,1): both arcs move outward together and fade -- this drives the expanding animation on the badge in the right-column diagram;
/// -1 is the static state (the left card's icon).
void drawBluetoothBadge(SDL_Renderer* r, int cx, int cy, int outerR, SDL_Color c,
                        double pulse = -1.0)
{
    if (outerR < 5) return;

    constexpr double kArcIn  = 0.88;                   // inner arc radius / glyph half-height
    constexpr double kArcOut = 1.42;                   // outer arc radius / glyph half-height
    constexpr double kArcSpan = 0.55;                  // arc opening +/-31.5 degrees (measured from the icon)

    const double h   = outerR * 0.82;                  // glyph half-height
    const double rod = std::max(1.0, outerR * 0.115);  // stroke width
    const double sx  = cx - (kArcOut - 0.636) / 2 * h; // center the glyph + right arc together
    const double sy  = cy;

    auto& v = triBuf();

    static const double kPath[6][2] = {
        {-0.636,  0.559},   // bottom-left end
        { 0.636, -0.429},   // top-right tip
        { 0.015, -1.000},   // ridge top
        { 0.015,  1.000},   // ridge bottom
        { 0.636,  0.429},   // bottom-right tip
        {-0.636, -0.559},   // top-left end
    };
    for (int i = 0; i + 1 < 6; ++i)
        emitCapsule(v, sx + kPath[i][0] * h,     sy + kPath[i][1] * h,
                       sx + kPath[i + 1][0] * h, sy + kPath[i + 1][1] * h, rod, c);

    if (pulse < 0.0) {
        // Static state: exactly the image the user provided (glyph + two arcs)
        emitArc(v, sx, sy, kArcIn * h,  -kArcSpan, kArcSpan, rod, c);
        emitArc(v, sx, sy, kArcOut * h, -kArcSpan, kArcSpan, rod, c);
    } else {
        // Animation: three arcs staggered by 1/3 period, traveling outward from the inner-arc position and fading.
        // At any moment one is freshly spawned (hugging the glyph), so the shape stays recognizable.
        for (int i = 0; i < 3; ++i) {
            const double p = std::fmod(pulse + i / 3.0, 1.0);
            const Uint8  a = Uint8(std::lround(255.0 * (1.0 - p)));
            if (a == 0) continue;
            const SDL_Color ac{c.r, c.g, c.b, a};
            emitArc(v, sx, sy, (kArcIn + 1.25 * p) * h, -kArcSpan, kArcSpan, rod, ac);
        }
    }

    flushTris(r, v);
}

/// Brand mark (the image the user provided on 2026-11-05, replacing the old dark "AI" square):
/// a dark navy open ring + a rounded play triangle at its center. size is the overall outer diameter.
/// The ring's gap is on the right, slightly above center (84 degrees, centered at -8 degrees), with round ends;
/// the triangle is equilateral, pointing right, with all three corners rounded -- assembled from "an inset triangle + a circle at each vertex",
/// equivalent to the Minkowski sum of the inset triangle and a circle of radius cr, giving exact circular arcs at the corners.
void drawPlayMark(SDL_Renderer* r, int cx, int cy, int size, SDL_Color c)
{
    if (size < 12) return;
    const double S  = size;
    const double ro = S * 0.5;          // outer radius
    const double sw = S * 0.13;         // ring width (measured from the image)
    const double rm = ro - sw / 2;      // ring centerline

    auto& v = triBuf();

    // ① Open ring: from +34 degrees around to +310 degrees, an 84-degree gap centered at -8 degrees
    constexpr double kGapHalf = 0.733;  // 42 degrees
    constexpr double kTilt    = -0.140; // -8 degrees
    emitArc(v, cx, cy, rm, kTilt + kGapHalf, kTilt - kGapHalf + 2 * M_PI, sw, c);

    // ② Rounded play triangle
    const double rt = S * 0.275;        // circumradius (measured from the image: triangle height ~= 0.375 x outer diameter)
    const double cr = S * 0.045;        // corner radius (any larger overshoots the triangle itself and turns into three round bumps)
    const double gx = cx + S * 0.012;   // the triangle's ink centroid sits left of the circumcenter; nudge right slightly to look centered
    SDL_FPoint p[3];
    for (int i = 0; i < 3; ++i) {
        const double a = 2 * M_PI * i / 3.0;    // 0 degrees = straight right, i.e. the tip
        p[i] = SDL_FPoint{float(gx + rt * std::cos(a)), float(cy + rt * std::sin(a))};
    }
    const double gcx = (p[0].x + p[1].x + p[2].x) / 3.0;
    const double gcy = (p[0].y + p[1].y + p[2].y) / 3.0;
    SDL_FPoint q[3];
    for (int i = 0; i < 3; ++i) {
        const double dx = gcx - p[i].x, dy = gcy - p[i].y;
        const double L  = std::hypot(dx, dy);
        const double d  = 2.0 * cr;            // equilateral triangle: r / sin(30 degrees)
        q[i] = SDL_FPoint{float(p[i].x + dx / L * d), float(p[i].y + dy / L * d)};
    }
    v.push_back(vert(q[0].x, q[0].y, c));
    v.push_back(vert(q[1].x, q[1].y, c));
    v.push_back(vert(q[2].x, q[2].y, c));
    for (const SDL_FPoint& t : q) emitCircle(v, t.x, t.y, cr, c);

    flushTris(r, v);
}

void drawCentered(const TextRenderer& t, SDL_Renderer* r, int cx, int y,
                  const std::string& s, SDL_Color c)
{
    t.draw(r, cx - t.measure(s) / 2, y, s, c);
}

void drawRight(const TextRenderer& t, SDL_Renderer* r, int right, int y,
               const std::string& s, SDL_Color c)
{
    t.draw(r, right - t.measure(s), y, s, c);
}

/// Truncate by character and append an ellipsis if it does not fit (Bluetooth names can be long)
std::string ellipsize(const TextRenderer& t, std::string s, int maxW)
{
    if (t.measure(s) <= maxW) return s;
    while (!s.empty() && t.measure(s + "…") > maxW) {
        size_t i = s.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;   // back up to a UTF-8 character head
        s.erase(i);
    }
    return s + "…";
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

int Launcher::logicalW(int winW, int winH)
{
    if (winW <= 0 || winH <= 0) return kDesignW;
    const int w = int(std::lround(double(kDesignH) * winW / winH));
    return w > kDesignW ? w : kDesignW;   // do not shrink below 1.81; let proportional centering leave the bars
}

Launcher::Layout Launcher::layoutFor(int winW, int winH) const
{
    Layout L;
    L.w = logicalW(winW, winH);
    L.h = kDesignH;
    const double sx = double(winW) / L.w;
    const double sy = double(winH) / L.h;
    L.scale = sx < sy ? sx : sy;
    L.ox    = (winW - L.w * L.scale) / 2.0;
    L.oy    = (winH - L.h * L.scale) / 2.0;

    // The cancel button position follows font size and line height, matching what draw() computes
    const int subtitleY = 142 + title_.lineHeight() + 10;
    const int contentY  = subtitleY + body_.lineHeight() + 62;
    const int buttonY   = contentY + kVisualH + 66;
    L.cancel = SDL_Rect{L.w / 2 - 125, buttonY, 250, 56};
    L.lang   = SDL_Rect{L.w - kMarginX - 72, 26, 72, 36};
    return L;
}

bool Launcher::init()
{
    // Chinese by default (the native version of the head-unit pairing page); ALLPLAY_LANG=en switches to English for screenshot layout checks
    if (const char* env = std::getenv("ALLPLAY_LANG")) {
        if (env[0] == 'e' || env[0] == 'E') lang_ = Lang::En;
    }
    return title_.init(42, FontFace::CJK, /*bold=*/true)
        && body_.init(19, FontFace::CJK)
        && eyebrow_.init(16, FontFace::CJK, /*bold=*/true)
        && device_.init(32, FontFace::CJK)   // the name is content, not a heading; do not bold it
        && hint_.init(15, FontFace::CJK)
        && button_.init(17, FontFace::CJK, /*bold=*/true)
        && footer_.init(14, FontFace::CJK)
        && brand_.init(21, FontFace::CJK, /*bold=*/true);
}

bool Launcher::handleEvent(const SDL_Event& e, int winW, int winH)
{
    if (e.type != SDL_MOUSEBUTTONDOWN || e.button.button != SDL_BUTTON_LEFT) return false;
    // Mouse coordinates are in window pixels; map them back with the window size drawn last frame (in fullscreen it may differ from --screen)
    if (winW_ > 0 && winH_ > 0) {
        winW = winW_;
        winH = winH_;
    }
    const Layout L = layoutFor(winW, winH);
    // Mouse events give **window coordinates** (logical points on high-DPI macOS/Wayland),
    // while L is computed in **renderer pixels**; multiply by pixelRatio_ to get pixels first, then map back to design coordinates.
    const double x = (e.button.x * pixelRatio_ - L.ox) / L.scale;
    const double y = (e.button.y * pixelRatio_ - L.oy) / L.scale;
    auto in = [&](const SDL_Rect& rc) {
        return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
    };
    if (in(L.cancel)) return true;                 // Cancel = exit
    if (in(L.lang)) lang_ = (lang_ == Lang::Zh) ? Lang::En : Lang::Zh;
    return false;
}

void Launcher::releaseTextures()
{
    for (TextRenderer* t : {&title_, &body_, &eyebrow_, &device_, &hint_,
                            &button_, &footer_, &brand_})
        t->clearCache();
    if (scene_) {
        SDL_DestroyTexture(scene_);
        scene_ = nullptr;
        sceneW_ = sceneH_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Launcher::draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs)
{
    winW_ = winW;
    winH_ = winH;
    const Layout L = layoutFor(winW, winH);

    // 2x supersampling, computed in **device pixels**: the scene texture = twice this picture's size in the window, drawn and then
    // scaled back exactly 2:1, which is a 2x2 box average per device pixel -- smooth edges with undistorted shapes.
    // Do not build the texture at "design x2": scaling that to the window is 2.4:1, so bilinear only samples 2x2 texels,
    // giving blur and moire (this is exactly what caused the earlier "blurry rounded corners/icon edges").
    constexpr int kSS = 2;   // must be a power of two: bilinear at 3:1 samples only 2x2 texels and causes moire
    const double sc = L.scale;                     // design -> device pixels
    const int vw = int(std::lround(L.w * sc));     // the picture's size in device pixels within the window
    const int vh = int(std::lround(L.h * sc));
    const int tw = vw * kSS, th = vh * kSS;

    // Rebuilding at 10 fps is enough for animation (ripples, ellipsis); static content is redrawn only on state changes.
    const uint64_t bucket = nowMs / 100;
    const bool dirty = !scene_ || sceneW_ != tw || sceneH_ != th ||
                       bucket != sceneBucket_ || st != sceneSt_ ||
                       lang_ != sceneLang_ || deviceName_ != sceneName_;
    if (dirty) {
        if (!scene_ || sceneW_ != tw || sceneH_ != th) {
            if (scene_) SDL_DestroyTexture(scene_);
            scene_ = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_TARGET, tw, th);
            sceneW_ = scene_ ? tw : 0;
            sceneH_ = scene_ ? th : 0;
            if (!scene_ && !sceneWarned_) {
                sceneWarned_ = true;
                std::fprintf(stderr, "[launcher] Renderer does not support offscreen textures, falling back to direct drawing (edges will be jagged): %s\n",
                             SDL_GetError());
            }
        }
        if (scene_) {
            SDL_SetRenderTarget(r, scene_);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_RenderClear(r);
            SDL_RenderSetScale(r, float(sc * kSS), float(sc * kSS));  // design coordinates -> 2x device pixels
            drawScene(r, L, st, nowMs, sc * kSS);                    // glyphs are rasterized at this same factor
            SDL_RenderSetScale(r, 1, 1);
            SDL_SetRenderTarget(r, nullptr);
            SDL_SetTextureScaleMode(scene_, SDL_ScaleModeLinear);
            sceneBucket_ = bucket;
            sceneSt_     = st;
            sceneLang_   = lang_;
            sceneName_   = deviceName_;
        }
    }

    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderClear(r);
    if (scene_) {
        // Do not set logical size: blit the 2x texture back directly in device pixels (the scale ratio is exactly 2:1)
        SDL_Rect dst{int(std::lround(L.ox)), int(std::lround(L.oy)), vw, vh};
        SDL_RenderCopy(r, scene_, nullptr, &dst);
    } else {
        // Renderer has no target texture: fall back to drawing directly with logical size (slightly rough edges, but fully functional)
        SDL_RenderSetLogicalSize(r, L.w, L.h);
        drawScene(r, L, st, nowMs, L.scale);
        SDL_RenderSetLogicalSize(r, 0, 0);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void Launcher::drawScene(SDL_Renderer* r, const Layout& L, LinkState st, uint64_t nowMs,
                         double rasterScale)
{
    // Rasterize glyphs at final device pixels (the supersampling factor is already multiplied in)
    for (TextRenderer* t : {&title_, &body_, &eyebrow_, &device_, &hint_,
                            &button_, &footer_, &brand_})
        t->setRasterScale(rasterScale);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const bool zh = (lang_ == Lang::Zh);

    // ======================= Top bar =======================
    SDL_SetRenderDrawColor(r, kRule.r, kRule.g, kRule.b, 255);
    SDL_Rect rule{0, kHeaderH, L.w, 2};
    SDL_RenderFillRect(r, &rule);

    // Wordmark: the C+play icon the user provided (the old dark square + "AI" has been removed)
    drawPlayMark(r, kMarginX + 20, kHeaderH / 2, 40, kInk);   // pure black like the text (user request)

    // Brand name
    if (brand_.ok())
        brand_.draw(r, kMarginX + 38 + 13, brand_.centeredY(0, kHeaderH, "AIIPlay"),
                    "AIIPlay", kInk);

    // Language pill: shows EN in Chinese mode, Chinese in English mode
    strokeRoundRect(r, L.lang.x, L.lang.y, L.lang.w, L.lang.h, L.lang.h / 2, 2,
                    kLangLine, SDL_Color{255, 255, 255, 255});
    if (footer_.ok()) {
        const char* pill = zh ? "EN" : "中文";
        drawCentered(footer_, r, L.lang.x + L.lang.w / 2,
                     footer_.centeredY(L.lang.y, L.lang.h, pill), pill, kLangInk);
    }

    // ======================= Title =======================
    const std::string title = zh ? "欢迎使用AllPlay" : "Welcome to AllPlay";
    const std::string sub   = zh ? "连接成功后，即可在车机屏幕使用完整的CarPlay服务"
                                 : "Once connected, you can use CarPlay services on the vehicle's display screen.";
    drawCentered(title_, r, L.w / 2, 142, title, kInk);
    const int subtitleY = 142 + title_.lineHeight() + 10;
    drawCentered(body_, r, L.w / 2, subtitleY, sub, kMuted);
    const int contentY = subtitleY + body_.lineHeight() + 62;

    // ======================= Two columns =======================
    const int mainX   = (L.w - kMainW) / 2;   // content block 1280 wide, centered (the HTML max-width)
    const int col1W   = int((kMainW - kColGap) * 0.92 / 2.0);
    const int leftX   = mainX + kLeftPad;
    const int leftW   = col1W - kLeftPad;
    const int visualX = mainX + col1W + kColGap;
    const int visualW = kMainW - col1W - kColGap;

    // ------------- Left column: device Bluetooth name -------------
    std::vector<std::string> hintLines;
    if (zh) {
        hintLines.push_back("请在手机蓝牙列表中选择此设备进行配对，并确保手机 Wi-Fi 已开启。");
    } else {
        hintLines.push_back("Select this device in your phone's Bluetooth list");
        hintLines.push_back("and make sure Wi-Fi is turned on.");
    }
    const int hintLH   = 27;
    const int blockH   = eyebrow_.lineHeight() + 18 + kCardH + 23
                       + int(hintLines.size()) * hintLH;
    int       blockY   = contentY + (kVisualH - blockH) / 2;

    eyebrow_.draw(r, leftX, blockY, zh ? "设备蓝牙名称" : "Bluetooth device name", kEyebrow);
    const int cardY = blockY + eyebrow_.lineHeight() + 18;

    softShadow(r, leftX, cardY, leftW, kCardH, 17);
    strokeRoundRect(r, leftX, cardY, leftW, kCardH, 17, 2, kCardLine,
                    SDL_Color{255, 255, 255, 255});

    const int tileX = leftX + 25, tileY = cardY + (kCardH - 48) / 2;
    SDL_SetRenderDrawColor(r, kTileBg.r, kTileBg.g, kTileBg.b, 255);
    fillRoundRect(r, tileX, tileY, 48, 48, 14);
    // Icon: the round Bluetooth badge the user provided (the ripple animation is on the badge in the right-column diagram; not drawn here)
    drawBluetoothBadge(r, tileX + 24, tileY + 24, 19, kIconBlue);

    if (device_.ok()) {
        const int nameX = tileX + 48 + 20;
        const int nameW = leftX + leftW - 25 - nameX;
        const std::string name = ellipsize(device_, deviceName_, nameW);
        device_.draw(r, nameX, device_.centeredY(cardY, kCardH, name), name, kInk);
    }

    int hintY = cardY + kCardH + 23;
    for (const std::string& line : hintLines) {
        hint_.draw(r, leftX, hintY, line, kHint);
        hintY += hintLH;
    }

    // ------------- Right column: diagram -------------
    softShadow(r, visualX, contentY, visualW, kVisualH, 20);
    strokeRoundRect(r, visualX, contentY, visualW, kVisualH, 20, 2, kVisualLine, kVisualBg);
    softGlow(r, visualX + visualW / 2, contentY + int(kVisualH * 0.52),
             int(visualW * 0.30), int(kVisualH * 0.30));

    // The SVG design is 620x285; fit it proportionally into the area with 28 padding
    const double sv = std::min(double(visualW - 56) / 620.0, double(kVisualH - 56) / 285.0);
    const double sox = visualX + (visualW - 620 * sv) / 2.0;
    const double soy = contentY + (kVisualH - 285 * sv) / 2.0;
    auto PX = [&](double v) { return int(std::lround(sox + v * sv)); };
    auto PY = [&](double v) { return int(std::lround(soy + v * sv)); };
    auto PS = [&](double v) { return int(std::lround(v * sv)); };
    auto rect = [&](double x, double y, double w, double h, double rad, SDL_Color c) {
        SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
        fillRoundRect(r, PX(x), PY(y), PS(w), PS(h), PS(rad));
    };

    // Phone
    softShadow(r, PX(20), PY(55), PS(88), PS(174), PS(15));
    rect(20, 55, 88, 174, 15, SDL_Color{0x24, 0x2A, 0x34, 255});
    rect(25, 61, 78, 162, 11, SDL_Color{0xEE, 0xF2, 0xF7, 255});
    rect(48, 66, 32, 5, 2, SDL_Color{0xC5, 0xCB, 0xD5, 255});
    SDL_SetRenderDrawColor(r, 0xC5, 0xCB, 0xD5, 255);
    fillCircle(r, PX(64), PY(198), PS(4));
    rect(42, 116, 44, 44, 12, kPhoneGreen);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    fillTriangle(r, SDL_Point{PX(58), PY(126)}, SDL_Point{PX(75), PY(138)},
                    SDL_Point{PX(58), PY(150)});

    // Bluetooth badge: the same icon as in the left card. Here the two signal arcs travel outward once every 2 seconds and fade
    // -- that is the "ripple expanding" animation, which suits the icon itself better than adding separate rings.
    const double phase = std::fmod(double(nowMs) / 2000.0, 1.0);
    drawBluetoothBadge(r, PX(198), PY(143), PS(19), kIconBlue, phase);

    // Head unit
    softShadow(r, PX(284), PY(24), PS(316), PS(230), PS(13));
    rect(284, 24, 316, 230, 13, SDL_Color{0x20, 0x27, 0x33, 255});
    rect(291, 31, 302, 216, 9, SDL_Color{0xF7, 0xF9, 0xFC, 255});
    rect(301, 42, 282, 164, 5, SDL_Color{0xE9, 0xED, 0xF4, 255});
    for (int i = 0; i < 3; ++i)
        rect(308 + i * 92, 49, 84, 150, 5, SDL_Color{0xE6, 0xEA, 0xF1, 255});

    // App icons in the three panels
    rect(325, 111, 50, 50, 12, SDL_Color{255, 255, 255, 255});
    SDL_SetRenderDrawColor(r, 0x28, 0xBD, 0x78, 255);
    fillTriangle(r, SDL_Point{PX(343), PY(122)}, SDL_Point{PX(362), PY(136)},
                    SDL_Point{PX(343), PY(150)});
    rect(416, 111, 52, 50, 12, SDL_Color{255, 255, 255, 255});
    rect(508, 111, 52, 50, 12, SDL_Color{255, 255, 255, 255});
    SDL_SetRenderDrawColor(r, 0x6C, 0x7A, 0x91, 255);
    for (int i = 0; i < 3; ++i) {
        const int y = PY(127 + i * 9);
        SDL_Rect line{PX(431), y, PS(i == 2 ? 15 : 22), std::max(2, PS(4))};
        SDL_RenderFillRect(r, &line);
    }
    fillTriangle(r, SDL_Point{PX(524), PY(145)}, SDL_Point{PX(534), PY(126)},
                    SDL_Point{PX(544), PY(145)});

    // Bottom dock
    rect(301, 211, 282, 26, 4, SDL_Color{0xED, 0xF1, 0xF6, 255});
    const SDL_Color dockIcons[4] = {{0x28, 0x78, 0xF0, 255}, {0xF2, 0xC9, 0x4C, 255},
                                    {0x28, 0xBD, 0x78, 255}, {0xEF, 0x9B, 0x78, 255}};
    for (int i = 0; i < 4; ++i)
        rect(314 + i * 28, 217, 15, 15, 4, dockIcons[i]);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int i = 0; i < 2; ++i) {   // the two white lines inside the yellow / orange icons
        SDL_Rect l1{PX(348), PY(221 + i * 4), PS(7), std::max(1, PS(1.5))};
        SDL_Rect l2{PX(404), PY(221 + i * 4), PS(7), std::max(1, PS(1.5))};
        SDL_RenderFillRect(r, &l1);
        SDL_RenderFillRect(r, &l2);
    }
    fillTriangle(r, SDL_Point{PX(374), PY(221)}, SDL_Point{PX(381), PY(224.5)},
                    SDL_Point{PX(374), PY(228)});

    SDL_SetRenderDrawColor(r, 0xD5, 0xDB, 0xE4, 255);   // dock separator
    SDL_Rect sep{PX(442), PY(224), PS(48), std::max(1, PS(1.5))};
    SDL_RenderFillRect(r, &sep);
    SDL_SetRenderDrawColor(r, 0x9A, 0xA5, 0xB5, 255);   // play / pause / page
    strokeRing(r, PX(516), PY(224.5) + 1, PS(5.5), std::max(1, PS(1.4)));
    fillTriangle(r, SDL_Point{PX(515), PY(222)}, SDL_Point{PX(518), PY(225)},
                    SDL_Point{PX(515), PY(228)});
    for (int i = 0; i < 2; ++i) {
        SDL_Rect bar{PX(538 + i * 13), PY(222), std::max(1, PS(2)), PS(5)};
        SDL_RenderFillRect(r, &bar);
    }
    fillTriangle(r, SDL_Point{PX(568), PY(222)}, SDL_Point{PX(572), PY(225)},
                    SDL_Point{PX(568), PY(228)});

    // ======================= Cancel button =======================
    softShadow(r, L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h, 14);
    SDL_SetRenderDrawColor(r, kInk.r, kInk.g, kInk.b, 255);
    fillRoundRect(r, L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h, 14);
    if (button_.ok()) {
        const char* label = zh ? "取消" : "Cancel";
        drawCentered(button_, r, L.cancel.x + L.cancel.w / 2,
                     button_.centeredY(L.cancel.y, L.cancel.h, label),
                     label, SDL_Color{255, 255, 255, 255});
    }

    // ======================= Footer =======================
    SDL_SetRenderDrawColor(r, kRule.r, kRule.g, kRule.b, 255);
    SDL_Rect footRule{0, L.h - kFooterH, L.w, 2};
    SDL_RenderFillRect(r, &footRule);

    SDL_Color dot = kGreen;
    std::string status;
    switch (st) {
        case LinkState::Idle:
        case LinkState::WaitingPair:
            status = zh ? "等待设备连接" : "Waiting for device";
            status += std::string(size_t((nowMs / 600) % 4), '.');
            break;
        case LinkState::Connecting:
            dot   = kBlue;
            status = zh ? "正在连接" : "Connecting";
            status += std::string(size_t((nowMs / 400) % 4), '.');
            break;
        case LinkState::Streaming:
            status = zh ? "已连接" : "Connected";
            break;
        case LinkState::Error:
            dot   = kRed;
            status = zh ? "连接失败，请重试" : "Connection failed";
            break;
    }
    const int footMid = L.h - kFooterH / 2;
    // Both segments share one y: text on the same line must share a baseline; centering each by its own ink would misalign them
    const int footY = footer_.centeredY(L.h - kFooterH, kFooterH, status);
    SDL_SetRenderDrawColor(r, dot.r, dot.g, dot.b, 255);
    fillCircle(r, kMarginX + 4, footMid, 4);
    footer_.draw(r, kMarginX + 21, footY, status, kFooterInk);
    if (footer_.ok())
        drawRight(footer_, r, L.w - kMarginX, footY, "AIIPlay · CarPlay", kFooterInk);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

}  // namespace allplay
