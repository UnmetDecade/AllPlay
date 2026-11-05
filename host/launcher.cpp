#include "launcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846   // MSVC 的 <cmath> 不定义它
#endif

namespace allplay {

namespace {

// —— 设计稿坐标系：1540×850（= 设计稿 1540×850 的网页），渲染时整体缩放居中 ——
constexpr int kDesignW = 1540;
constexpr int kDesignH = 850;

// 颜色取自设计稿的 CSS 变量，别随手改
constexpr SDL_Color kInk        = {0x17, 0x19, 0x1F, 255};   // --ink
constexpr SDL_Color kMuted      = {0x73, 0x79, 0x85, 255};   // --muted
constexpr SDL_Color kBlue       = {0x28, 0x78, 0xF0, 255};   // --blue
constexpr SDL_Color kEyebrow    = {0x5D, 0x64, 0x70, 255};
constexpr SDL_Color kHint       = {0x85, 0x8B, 0x96, 255};
constexpr SDL_Color kFooterInk  = {0x4E, 0x55, 0x60, 255};   // 页脚别太淡：14px 灰字看不清（左右两段共用这个色）
constexpr SDL_Color kLangInk    = {0x30, 0x36, 0x42, 255};
constexpr SDL_Color kRule       = {0xF0, 0xF1, 0xF4, 255};
constexpr SDL_Color kCardLine   = {0xE8, 0xEB, 0xF1, 255};
constexpr SDL_Color kLangLine   = {0xE5, 0xE8, 0xEE, 255};
constexpr SDL_Color kVisualLine = {0xED, 0xF0, 0xF4, 255};
constexpr SDL_Color kVisualBg   = {0xFA, 0xFB, 0xFD, 255};
constexpr SDL_Color kTileBg     = {0xED, 0xF5, 0xFF, 255};
constexpr SDL_Color kGreen      = {0x25, 0xB8, 0x78, 255};   // 页脚状态点
constexpr SDL_Color kPhoneGreen = {0x25, 0xC7, 0x7A, 255};   // 手机里的播放图标
constexpr SDL_Color kRed        = {0xE5, 0x48, 0x4D, 255};
// 蓝牙图标自己的蓝（用户给的那张图取样：天蓝，比 --blue 浅）
constexpr SDL_Color kIconBlue   = {0x1E, 0x9B, 0xE0, 255};

// 版面常量（设计稿像素）
constexpr int kHeaderH   = 88;
constexpr int kMarginX   = 76;
constexpr int kMainW     = 1280;
constexpr int kLeftPad   = 24;
constexpr int kColGap    = 100;
constexpr int kVisualH   = 330;
constexpr int kCardH     = 88;
constexpr int kFooterH   = 54;

// —— 基本图元 ——
//
// 全部用 SDL_RenderGeometry（**浮点顶点**）画。
// 为什么不用逐行整数扫描线：那种画法把每个坐标都量化到 1 个设计稿像素，在 Retina
// 上就是 1.66 个设备像素——圆角、圆环、小图标于是全是台阶。超采样只能把台阶的
// 边磨圆一点，磨不掉台阶本身（用户看到的"蓝牙图标非常糊"就是它）。
// 交给 GPU 之后，顶点先按渲染缩放换成设备坐标，边缘精确到设备像素。

/// 当前绘制色（调用方照旧用 SDL_SetRenderDrawColor 设色，这里读回来当顶点色）
SDL_Color curColor(SDL_Renderer* r)
{
    SDL_Color c{0, 0, 0, 255};
    SDL_GetRenderDrawColor(r, &c.r, &c.g, &c.b, &c.a);
    return c;
}

/// 三角形收集缓冲：场景重建是单线程的，复用一块内存，别每次都去 malloc
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

/// 往缓冲里塞一个实心圆（不落盘，调用方自己 flush —— 蓝牙图标一次要画上百个小圆）
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
    emitCircle(v, cx, cy, rad + 0.5, curColor(r));   // +0.5：和旧扫描线的覆盖范围对齐
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

/// 圆环（rad = 外半径，thick = 环宽）
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
    const double x0 = x - 0.5, y0 = y - 0.5;      // 旧扫描线的像素边界
    const double x1 = x0 + w, y1 = y0 + h;
    double R = rad;
    if (R * 2 > w) R = w / 2.0;
    if (R * 2 > h) R = h / 2.0;

    auto& v = triBuf();
    if (R <= 0.5) {                                // 直角矩形
        v.push_back(vert(x0, y0, c)); v.push_back(vert(x1, y0, c));
        v.push_back(vert(x1, y1, c));
        v.push_back(vert(x0, y0, c)); v.push_back(vert(x1, y1, c));
        v.push_back(vert(x0, y1, c));
        flushTris(r, v);
        return;
    }

    // 轮廓：四个圆角各画一段 90° 弧，首尾自然连出四条直边（形状是凸的，扇形三角化）
    struct Corner { double cx, cy, a0; };
    const Corner corners[4] = {
        {x0 + R, y0 + R, M_PI},          // 左上：π → 3π/2
        {x1 - R, y0 + R, -M_PI / 2},     // 右上：-π/2 → 0
        {x1 - R, y1 - R, 0},             // 右下：0 → π/2
        {x0 + R, y1 - R, M_PI / 2},      // 左下：π/2 → π
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

/// 圆角矩形描边：先按边框色填满，再用填充色盖住内部（背景都是纯色，够用）。
void strokeRoundRect(SDL_Renderer* r, int x, int y, int w, int h, int rad, int t,
                     SDL_Color border, SDL_Color fill)
{
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    fillRoundRect(r, x, y, w, h, rad);
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    fillRoundRect(r, x + t, y + t, w - 2 * t, h - 2 * t, rad > t ? rad - t : 0);
}

/// 卡片投影：同宽、只往下长的一叠淡圆角矩形 —— 离底边越远叠得越少，看起来就是
/// 一层向下淡出的影子。不向左右溢出，免得在卡片左边露出一条灰边（SDL 没有模糊）。
void softShadow(SDL_Renderer* r, int x, int y, int w, int h, int rad)
{
    SDL_SetRenderDrawColor(r, 0x1A, 0x26, 0x3A, 3);
    for (int i = 9; i >= 1; --i)
        fillRoundRect(r, x, y, w, h + i, rad);
}

/// 示意图背后的蓝色柔光（设计稿里的 radial-gradient）
void softGlow(SDL_Renderer* r, int cx, int cy, int rx, int ry)
{
    SDL_SetRenderDrawColor(r, 0x28, 0x78, 0xF0, 2);
    for (int i = 0; i < 14; ++i) {
        const double k = 1.0 - i * 0.055;
        fillEllipse(r, cx, cy, int(rx * k), int(ry * k));
    }
}

/// 一段圆弧（角度制，0° = +x 方向，顺时针为正）
/// 现在示意图用的是整圈扩散的波纹，这里暂时没人调；留着给"两侧信号弧"那种画法备用。
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

/// 一段粗圆弧：沿线摆一串小圆，端点自然是圆的（跟图标的圆头笔画一致）
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

/// 一段粗直线（圆头）
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

/// 蓝牙图符（用户指定的那张图标）：粗圆头符文 + 右侧两道信号弧，**没有外圈**。
/// 界面里两处图标都调它，所以两处长得一模一样。
/// 符文用官方 logo 的六点折线描出来（左下端 → 右上尖 → 竖脊顶 → 竖脊底 → 右下尖 →
/// 左上端），比例照图标量的；两道弧以符文中心为圆心，内弧 0.81 半高、外弧 1.33 半高，
/// 开口 ±36°。
///
/// pulse ∈ [0,1)：两弧一起往外走并渐隐 —— 右栏示意图那颗就靠它做扩散动效；
/// 传 -1 是静止态（左卡片的图标）。
void drawBluetoothBadge(SDL_Renderer* r, int cx, int cy, int outerR, SDL_Color c,
                        double pulse = -1.0)
{
    if (outerR < 5) return;

    constexpr double kArcIn  = 0.88;                   // 内弧半径 / 符文半高
    constexpr double kArcOut = 1.42;                   // 外弧半径 / 符文半高
    constexpr double kArcSpan = 0.55;                  // 弧开口 ±31.5°（照图标量的）

    const double h   = outerR * 0.82;                  // 符文半高
    const double rod = std::max(1.0, outerR * 0.115);  // 笔画粗细
    const double sx  = cx - (kArcOut - 0.636) / 2 * h; // 符文+右弧整体居中
    const double sy  = cy;

    auto& v = triBuf();

    static const double kPath[6][2] = {
        {-0.636,  0.559},   // 左下端
        { 0.636, -0.429},   // 右上尖
        { 0.015, -1.000},   // 竖脊顶端
        { 0.015,  1.000},   // 竖脊底端
        { 0.636,  0.429},   // 右下尖
        {-0.636, -0.559},   // 左上端
    };
    for (int i = 0; i + 1 < 6; ++i)
        emitCapsule(v, sx + kPath[i][0] * h,     sy + kPath[i][1] * h,
                       sx + kPath[i + 1][0] * h, sy + kPath[i + 1][1] * h, rod, c);

    if (pulse < 0.0) {
        // 静止态：就是用户给的那张图（符文 + 两道弧）
        emitArc(v, sx, sy, kArcIn * h,  -kArcSpan, kArcSpan, rod, c);
        emitArc(v, sx, sy, kArcOut * h, -kArcSpan, kArcSpan, rod, c);
    } else {
        // 动效：三道弧错开 1/3 周期，从内弧位置一路往外走并渐隐。
        // 任何时刻都有一道刚生成（贴在图符边），所以轮廓始终认得出来。
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

/// 品牌标（用户 2026-11-05 给的那张图，替换原来的"AI"深色方块）：
/// 深藏青的开口圆环 + 环心一个圆角播放三角。size 是整体外径。
/// 环的缺口在右侧、略偏上（84°，中心 -8°），两端圆头；
/// 三角等边、尖朝右，三个角倒圆 —— 用"内缩三角 + 每个顶点画圆"拼，
/// 等价于内缩三角和半径 cr 的圆做闵可夫斯基和，角上正好是正圆弧。
void drawPlayMark(SDL_Renderer* r, int cx, int cy, int size, SDL_Color c)
{
    if (size < 12) return;
    const double S  = size;
    const double ro = S * 0.5;          // 外半径
    const double sw = S * 0.13;         // 环宽（照图量的）
    const double rm = ro - sw / 2;      // 环心线

    auto& v = triBuf();

    // ① 开口圆环：+34° 绕到 +310°，缺口 84°，缺口中心 -8°
    constexpr double kGapHalf = 0.733;  // 42°
    constexpr double kTilt    = -0.140; // -8°
    emitArc(v, cx, cy, rm, kTilt + kGapHalf, kTilt - kGapHalf + 2 * M_PI, sw, c);

    // ② 圆角播放三角
    const double rt = S * 0.275;        // 外接圆半径（照图量的：三角高 ≈ 0.375·外径）
    const double cr = S * 0.045;        // 圆角半径（再大就盖过三角本身，会变成三个圆鼓包）
    const double gx = cx + S * 0.012;   // 三角的墨迹重心比外接圆心偏左，右移一点点视觉才正
    SDL_FPoint p[3];
    for (int i = 0; i < 3; ++i) {
        const double a = 2 * M_PI * i / 3.0;    // 0° = 正右，就是那个尖
        p[i] = SDL_FPoint{float(gx + rt * std::cos(a)), float(cy + rt * std::sin(a))};
    }
    const double gcx = (p[0].x + p[1].x + p[2].x) / 3.0;
    const double gcy = (p[0].y + p[1].y + p[2].y) / 3.0;
    SDL_FPoint q[3];
    for (int i = 0; i < 3; ++i) {
        const double dx = gcx - p[i].x, dy = gcy - p[i].y;
        const double L  = std::hypot(dx, dy);
        const double d  = 2.0 * cr;            // 等边三角形：r / sin(30°)
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

/// 放不下就按字符截断并补省略号（蓝牙名可能很长）
std::string ellipsize(const TextRenderer& t, std::string s, int maxW)
{
    if (t.measure(s) <= maxW) return s;
    while (!s.empty() && t.measure(s + "…") > maxW) {
        size_t i = s.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;   // 退到 UTF-8 字符头
        s.erase(i);
    }
    return s + "…";
}

}  // namespace

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

int Launcher::logicalW(int winW, int winH)
{
    if (winW <= 0 || winH <= 0) return kDesignW;
    const int w = int(std::lround(double(kDesignH) * winW / winH));
    return w > kDesignW ? w : kDesignW;   // 比 1.81 窄时不缩，交给等比居中留边
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

    // 取消按钮的位置跟着字号行高走，和 draw() 里算的是同一份
    const int subtitleY = 142 + title_.lineHeight() + 10;
    const int contentY  = subtitleY + body_.lineHeight() + 62;
    const int buttonY   = contentY + kVisualH + 66;
    L.cancel = SDL_Rect{L.w / 2 - 125, buttonY, 250, 56};
    L.lang   = SDL_Rect{L.w - kMarginX - 72, 26, 72, 36};
    return L;
}

bool Launcher::init()
{
    // 默认中文（车机连接页的母语版本）；ALLPLAY_LANG=en 可切英文，方便截图核对版式
    if (const char* env = std::getenv("ALLPLAY_LANG")) {
        if (env[0] == 'e' || env[0] == 'E') lang_ = Lang::En;
    }
    return title_.init(42, FontFace::CJK, /*bold=*/true)
        && body_.init(19, FontFace::CJK)
        && eyebrow_.init(16, FontFace::CJK, /*bold=*/true)
        && device_.init(32, FontFace::CJK)   // 名称是内容不是标题，别加粗
        && hint_.init(15, FontFace::CJK)
        && button_.init(17, FontFace::CJK, /*bold=*/true)
        && footer_.init(14, FontFace::CJK)
        && brand_.init(21, FontFace::CJK, /*bold=*/true);
}

bool Launcher::handleEvent(const SDL_Event& e, int winW, int winH)
{
    if (e.type != SDL_MOUSEBUTTONDOWN || e.button.button != SDL_BUTTON_LEFT) return false;
    // 鼠标坐标是窗口像素；用上一帧画过的窗口尺寸反算（全屏时它和 --screen 不一定一样）
    if (winW_ > 0 && winH_ > 0) {
        winW = winW_;
        winH = winH_;
    }
    const Layout L = layoutFor(winW, winH);
    // 鼠标事件给的是**窗口坐标**（macOS/Wayland 高 DPI 下是逻辑点），
    // 而 L 是按**渲染器像素**算的；先乘 pixelRatio_ 换到像素再反算设计稿坐标。
    const double x = (e.button.x * pixelRatio_ - L.ox) / L.scale;
    const double y = (e.button.y * pixelRatio_ - L.oy) / L.scale;
    auto in = [&](const SDL_Rect& rc) {
        return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
    };
    if (in(L.cancel)) return true;                 // 取消 = 退出
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
// 绘制
// ---------------------------------------------------------------------------

void Launcher::draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs)
{
    winW_ = winW;
    winH_ = winH;
    const Layout L = layoutFor(winW, winH);

    // 2× 超采样，按**设备像素**算：场景纹理 = 这块画面在窗口里的 2 倍，画完刚好
    // 2:1 缩回去，等于每个设备像素做 2×2 盒式平均 —— 边平滑、形状也不走样。
    // 注意别按"设计稿 ×2"来建纹理：那样缩到窗口是 2.4:1，双线性只采 2×2 个纹素，
    // 结果又糊又起摩尔纹（这正是之前"圆角/图标边缘很糊"的原因）。
    constexpr int kSS = 2;   // 只能是 2 的幂：双线性缩 3:1 只采 2×2 个纹素，反而起摩尔纹
    const double sc = L.scale;                     // 设计稿 → 设备像素
    const int vw = int(std::lround(L.w * sc));     // 画面在窗口里的设备像素尺寸
    const int vh = int(std::lround(L.h * sc));
    const int tw = vw * kSS, th = vh * kSS;

    // 动画（波纹、省略号）10 fps 重建就够；静态内容只在状态变化时重画。
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
                std::fprintf(stderr, "[launcher] 渲染器不支持离屏纹理，退回直画（边缘会有锯齿）：%s\n",
                             SDL_GetError());
            }
        }
        if (scene_) {
            SDL_SetRenderTarget(r, scene_);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_RenderClear(r);
            SDL_RenderSetScale(r, float(sc * kSS), float(sc * kSS));  // 设计稿坐标 → 2× 设备像素
            drawScene(r, L, st, nowMs, sc * kSS);                    // 字形也按这个倍数栅格化
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
        // 不设 logical size：直接把 2× 纹理按设备像素贴回去（缩放比正好 2:1）
        SDL_Rect dst{int(std::lround(L.ox)), int(std::lround(L.oy)), vw, vh};
        SDL_RenderCopy(r, scene_, nullptr, &dst);
    } else {
        // 渲染器不支持 target 纹理：退回 logical size 直接画（边缘略糙，但功能不缺）
        SDL_RenderSetLogicalSize(r, L.w, L.h);
        drawScene(r, L, st, nowMs, L.scale);
        SDL_RenderSetLogicalSize(r, 0, 0);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void Launcher::drawScene(SDL_Renderer* r, const Layout& L, LinkState st, uint64_t nowMs,
                         double rasterScale)
{
    // 字形按最终设备像素栅格化（超采样倍数已经乘进来）
    for (TextRenderer* t : {&title_, &body_, &eyebrow_, &device_, &hint_,
                            &button_, &footer_, &brand_})
        t->setRasterScale(rasterScale);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    const bool zh = (lang_ == Lang::Zh);

    // ======================= 顶栏 =======================
    SDL_SetRenderDrawColor(r, kRule.r, kRule.g, kRule.b, 255);
    SDL_Rect rule{0, kHeaderH, L.w, 2};
    SDL_RenderFillRect(r, &rule);

    // 字标：用户给的 C+播放 图标（原来的深色方块 + "AI" 已撤掉）
    drawPlayMark(r, kMarginX + 20, kHeaderH / 2, 40, kInk);   // 和字一样纯黑（用户要求）

    // 品牌名
    if (brand_.ok())
        brand_.draw(r, kMarginX + 38 + 13, brand_.centeredY(0, kHeaderH, "AIIPlay"),
                    "AIIPlay", kInk);

    // 语言胶囊：中文时显示 EN，英文时显示中文
    strokeRoundRect(r, L.lang.x, L.lang.y, L.lang.w, L.lang.h, L.lang.h / 2, 2,
                    kLangLine, SDL_Color{255, 255, 255, 255});
    if (footer_.ok()) {
        const char* pill = zh ? "EN" : "中文";
        drawCentered(footer_, r, L.lang.x + L.lang.w / 2,
                     footer_.centeredY(L.lang.y, L.lang.h, pill), pill, kLangInk);
    }

    // ======================= 标题 =======================
    const std::string title = zh ? "欢迎使用AllPlay" : "Welcome to AllPlay";
    const std::string sub   = zh ? "连接成功后，即可在车机屏幕使用完整的CarPlay服务"
                                 : "Once connected, you can use CarPlay services on the vehicle's display screen.";
    drawCentered(title_, r, L.w / 2, 142, title, kInk);
    const int subtitleY = 142 + title_.lineHeight() + 10;
    drawCentered(body_, r, L.w / 2, subtitleY, sub, kMuted);
    const int contentY = subtitleY + body_.lineHeight() + 62;

    // ======================= 两栏 =======================
    const int mainX   = (L.w - kMainW) / 2;   // 内容块 1280 宽，居中（HTML 的 max-width）
    const int col1W   = int((kMainW - kColGap) * 0.92 / 2.0);
    const int leftX   = mainX + kLeftPad;
    const int leftW   = col1W - kLeftPad;
    const int visualX = mainX + col1W + kColGap;
    const int visualW = kMainW - col1W - kColGap;

    // ------------- 左栏：设备蓝牙名称 -------------
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
    // 图标：用户给的那张蓝牙圆标（波纹扩散在右栏示意图那颗上，这里不画）
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

    // ------------- 右栏：示意图 -------------
    softShadow(r, visualX, contentY, visualW, kVisualH, 20);
    strokeRoundRect(r, visualX, contentY, visualW, kVisualH, 20, 2, kVisualLine, kVisualBg);
    softGlow(r, visualX + visualW / 2, contentY + int(kVisualH * 0.52),
             int(visualW * 0.30), int(kVisualH * 0.30));

    // SVG 设计稿是 620×285，等比放进内边距 28 的区域
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

    // 手机
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

    // 蓝牙图符：和左侧卡片里的是同一个图标。这里两道信号弧跟着 2 秒一轮往外走、
    // 渐隐——就是"波纹扩散"的动效，比另外套一圈圈圆环更贴图标本身。
    const double phase = std::fmod(double(nowMs) / 2000.0, 1.0);
    drawBluetoothBadge(r, PX(198), PY(143), PS(19), kIconBlue, phase);

    // 中控屏
    softShadow(r, PX(284), PY(24), PS(316), PS(230), PS(13));
    rect(284, 24, 316, 230, 13, SDL_Color{0x20, 0x27, 0x33, 255});
    rect(291, 31, 302, 216, 9, SDL_Color{0xF7, 0xF9, 0xFC, 255});
    rect(301, 42, 282, 164, 5, SDL_Color{0xE9, 0xED, 0xF4, 255});
    for (int i = 0; i < 3; ++i)
        rect(308 + i * 92, 49, 84, 150, 5, SDL_Color{0xE6, 0xEA, 0xF1, 255});

    // 三块面板里的应用图标
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

    // 底部 dock
    rect(301, 211, 282, 26, 4, SDL_Color{0xED, 0xF1, 0xF6, 255});
    const SDL_Color dockIcons[4] = {{0x28, 0x78, 0xF0, 255}, {0xF2, 0xC9, 0x4C, 255},
                                    {0x28, 0xBD, 0x78, 255}, {0xEF, 0x9B, 0x78, 255}};
    for (int i = 0; i < 4; ++i)
        rect(314 + i * 28, 217, 15, 15, 4, dockIcons[i]);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int i = 0; i < 2; ++i) {   // 黄色 / 橙色图标里的两条白线
        SDL_Rect l1{PX(348), PY(221 + i * 4), PS(7), std::max(1, PS(1.5))};
        SDL_Rect l2{PX(404), PY(221 + i * 4), PS(7), std::max(1, PS(1.5))};
        SDL_RenderFillRect(r, &l1);
        SDL_RenderFillRect(r, &l2);
    }
    fillTriangle(r, SDL_Point{PX(374), PY(221)}, SDL_Point{PX(381), PY(224.5)},
                    SDL_Point{PX(374), PY(228)});

    SDL_SetRenderDrawColor(r, 0xD5, 0xDB, 0xE4, 255);   // dock 分隔线
    SDL_Rect sep{PX(442), PY(224), PS(48), std::max(1, PS(1.5))};
    SDL_RenderFillRect(r, &sep);
    SDL_SetRenderDrawColor(r, 0x9A, 0xA5, 0xB5, 255);   // 播放 / 暂停 / 翻页
    strokeRing(r, PX(516), PY(224.5) + 1, PS(5.5), std::max(1, PS(1.4)));
    fillTriangle(r, SDL_Point{PX(515), PY(222)}, SDL_Point{PX(518), PY(225)},
                    SDL_Point{PX(515), PY(228)});
    for (int i = 0; i < 2; ++i) {
        SDL_Rect bar{PX(538 + i * 13), PY(222), std::max(1, PS(2)), PS(5)};
        SDL_RenderFillRect(r, &bar);
    }
    fillTriangle(r, SDL_Point{PX(568), PY(222)}, SDL_Point{PX(572), PY(225)},
                    SDL_Point{PX(568), PY(228)});

    // ======================= 取消按钮 =======================
    softShadow(r, L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h, 14);
    SDL_SetRenderDrawColor(r, kInk.r, kInk.g, kInk.b, 255);
    fillRoundRect(r, L.cancel.x, L.cancel.y, L.cancel.w, L.cancel.h, 14);
    if (button_.ok()) {
        const char* label = zh ? "取消" : "Cancel";
        drawCentered(button_, r, L.cancel.x + L.cancel.w / 2,
                     button_.centeredY(L.cancel.y, L.cancel.h, label),
                     label, SDL_Color{255, 255, 255, 255});
    }

    // ======================= 页脚 =======================
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
    // 左右两段共用同一个 y：同一行字必须共基线，各按自己的墨迹居中会错开
    const int footY = footer_.centeredY(L.h - kFooterH, kFooterH, status);
    SDL_SetRenderDrawColor(r, dot.r, dot.g, dot.b, 255);
    fillCircle(r, kMarginX + 4, footMid, 4);
    footer_.draw(r, kMarginX + 21, footY, status, kFooterInk);
    if (footer_.ok())
        drawRight(footer_, r, L.w - kMarginX, footY, "AIIPlay · CarPlay", kFooterInk);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

}  // namespace allplay
