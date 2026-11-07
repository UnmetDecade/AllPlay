#include "statusbar.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace allplay {

namespace {

// 候选字体（第一个能开的就用）。可用 ALLPLAY_FONT 环境变量覆盖。
// index：.ttc 里的第几个字面（Noto Sans CJK 的 SC 是 2 号；JP=0/KR=1/SC=2/TC=3/HK=4）。
struct FontFile { const char* path; int index; };

const FontFile kMonoCandidates[] = {
    {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0},
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0},
    {"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", 0},
    {"/System/Library/Fonts/SFNSMono.ttf", 0},                        // macOS
    {"/System/Library/Fonts/Menlo.ttc", 0},
};

const FontFile kSansCandidates[] = {
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0},
    {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", 0},
    {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0},
    {"/System/Library/Fonts/SFNS.ttf", 0},                             // macOS
    {"/System/Library/Fonts/Helvetica.ttc", 0},
};

// 中文界面：优先带全 CJK 字形的字体（它们也含拉丁字母，不必再拼 DejaVu）
const FontFile kCjkCandidates[] = {
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},   // SC
    {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
    {"/usr/share/fonts/truetype/arphic/uming.ttc", 0},
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},               // macOS
    {"/System/Library/Fonts/STHeiti Light.ttc", 0},
};

// 粗体优先用真粗体字面（比 TTF 的合成粗体好看）；拿不到再退到合成
const FontFile kBoldCandidates[] = {
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 0},
    {"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", 0},
};

const FontFile kCjkBoldCandidates[] = {
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", 2},
    // 冬青黑体的 .ttc 里：0=W3、1=Interface W3、**2=W6**、3=Interface W6。
    // 以前写的 1 其实是常规体，所以 Mac 上的"粗体"根本没粗（用户："bold 再加粗一些"）。
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", 2},
};

bool openFont(const FontFile& f, int px, TTF_Font** out)
{
    *out = f.index > 0 ? TTF_OpenFontIndex(f.path, px, f.index)
                       : TTF_OpenFont(f.path, px);
    return *out != nullptr;
}

/// 按族挑一个字体：粗体先找真粗体字面，找不到就用主字体 + 合成粗体
TTF_Font* openFace(int px, FontFace face, bool bold, bool* realBold)
{
    const bool cjk = (face == FontFace::CJK);
    TTF_Font*  f   = nullptr;
    *realBold      = false;

    if (bold) {
        for (const FontFile& cand : (cjk ? kCjkBoldCandidates : kBoldCandidates)) {
            if (openFont(cand, px, &f)) {
                *realBold = true;
                break;
            }
        }
    }
    if (!f) {
        const FontFile* list = cjk ? kCjkCandidates
                              : face == FontFace::Sans ? kSansCandidates
                                                       : kMonoCandidates;
        const size_t n = cjk ? sizeof(kCjkCandidates) / sizeof(kCjkCandidates[0])
                         : face == FontFace::Sans ? sizeof(kSansCandidates) / sizeof(kSansCandidates[0])
                                                  : sizeof(kMonoCandidates) / sizeof(kMonoCandidates[0]);
        for (size_t i = 0; i < n; ++i) {
            if (openFont(list[i], px, &f)) break;
        }
    }
    // 只有拿不到真粗体时才合成（合成加粗会糊边，能开真粗体字面就别用）
    if (f && bold && !*realBold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    return f;
}

// TTF_Init/TTF_Quit 是库级全局状态：多个 TextRenderer 共存时必须引用计数，
// 否则先析构的那个会把还活着的那些字体一起带走。
int g_ttfRefs = 0;

}  // namespace

// ---------------------------------------------------------------------------
// TextRenderer
// ---------------------------------------------------------------------------

TextRenderer::~TextRenderer()
{
    clearCache();
    if (hi_) TTF_CloseFont(hi_);
    if (font_) TTF_CloseFont(font_);
    if (g_ttfRefs > 0 && --g_ttfRefs == 0) TTF_Quit();
}

void TextRenderer::clearCache() const
{
    for (CachedText& e : cache_)
        if (e.tex) SDL_DestroyTexture(e.tex);
    cache_.clear();
    ink_.clear();          // 墨迹量的是"当前字号"下的行内偏移，字号变了就作废
}

bool TextRenderer::init(int pixelHeight, FontFace face, bool bold)
{
    if (!font_) {
        if (g_ttfRefs == 0 && TTF_Init() != 0) {
            std::fprintf(stderr, "[statusbar] TTF_Init 失败：%s（状态栏不显示文本）\n",
                         TTF_GetError());
            return false;
        }
        ++g_ttfRefs;
    }

    px_   = pixelHeight;
    face_ = face;
    bold_ = bold;

    const char* env = std::getenv("ALLPLAY_FONT");
    if (env && *env) {
        font_ = TTF_OpenFont(env, pixelHeight);
        if (!font_)
            std::fprintf(stderr, "[statusbar] ALLPLAY_FONT=\"%s\" 打不开：%s\n",
                         env, TTF_GetError());
    }
    if (!font_) font_ = openFace(pixelHeight, face, bold, &hasRealBold_);
    if (!font_) {
        std::fprintf(stderr, "[statusbar] 找不到可用字体（装有 fonts-dejavu-core 吗？）"
                             "—— 状态栏降级为色块\n");
        return false;
    }
    lh_ = TTF_FontLineSkip(font_);
    return true;
}

void TextRenderer::setRasterScale(double scale)
{
    if (scale < 0.25) scale = 0.25;
    if (scale > 4.0)  scale = 4.0;
    if (std::abs(scale - scale_) < 0.02) return;   // 缩放没变就不用重开字体

    clearCache();          // 字号变了，缓存里的字形纹理全作废
    ++gen_;
    if (hi_) {
        TTF_CloseFont(hi_);
        hi_ = nullptr;
    }
    scale_ = scale;
    if (font_ && std::abs(scale - 1.0) > 0.02) {
        bool realBold = false;
        hi_ = openFace(int(std::lround(px_ * scale)), face_, bold_, &realBold);
    }
}

bool TextRenderer::inkSpan(const std::string& s, double* top, double* bot) const
{
    TTF_Font* f = hi_ ? hi_ : font_;
    if (!f || s.empty()) return false;
    for (const InkSpan& e : ink_)
        if (e.gen == gen_ && e.s == s) { *top = e.top; *bot = e.bot; return true; }

    // 渲染一次、扫 alpha 找墨迹的上下沿。结果按 (字符串, 字号代) 缓存 —— 启动页
    // 每帧都问同一批字符串，真正扫描只发生一次。
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return false;

    int y0 = surf->h, y1 = -1;
    if (surf->format->format == SDL_PIXELFORMAT_ARGB8888) {
        if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
        for (int y = 0; y < surf->h; ++y) {
            const Uint8* row = static_cast<const Uint8*>(surf->pixels) + size_t(y) * surf->pitch;
            for (int x = 0; x < surf->w; ++x)
                if (row[x * 4 + 3] > 8) { if (y < y0) y0 = y; y1 = y; break; }
        }
        if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
    }
    SDL_FreeSurface(surf);
    if (y1 < y0) return false;      // 空行（全是空白）

    // 表面是"设备像素"栅格化的，换算回设计稿坐标（draw() 也按同一比例贴回去）
    const double sc = hi_ ? scale_ : 1.0;
    InkSpan e;
    e.s   = s;
    e.gen = gen_;
    e.top = y0 / sc;
    e.bot = (y1 + 1) / sc;
    if (ink_.size() > 256) ink_.clear();
    ink_.push_back(e);
    *top = e.top;
    *bot = e.bot;
    return true;
}

int TextRenderer::centeredY(int top, int h, const std::string& s) const
{
    double t = 0.0, b = 0.0;
    if (inkSpan(s, &t, &b))
        return int(std::lround(top + h / 2.0 - (t + b) / 2.0));
    return top + (h - lineHeight()) / 2;    // 量不出来就退回行框居中
}

void TextRenderer::draw(SDL_Renderer* r, int x, int y, const std::string& s,
                        SDL_Color c) const
{
    TTF_Font* f = hi_ ? hi_ : font_;
    if (!f || s.empty()) return;

    // 字形纹理缓存：启动页每帧重画，但同样的字/颜色只有一份 —— 每帧都
    // TTF_RenderUTF8_Blended 一遍 CJK 是 CPU 90% 的来源。
    for (const CachedText& e : cache_) {
        if (e.gen == gen_ && e.s == s && e.c.r == c.r && e.c.g == c.g &&
            e.c.b == c.b && e.c.a == c.a) {
            SDL_Rect dst{x, y, e.w, e.h};
            SDL_RenderCopy(r, e.tex, nullptr, &dst);
            return;
        }
    }

    const double sc = hi_ ? scale_ : 1.0;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return;
    }
    CachedText e;
    e.s   = s;
    e.c   = c;
    e.gen = gen_;
    e.tex = tex;
    e.w   = surf->w;
    e.h   = surf->h;
    if (sc > 0.0 && std::abs(sc - 1.0) > 0.001) {
        e.w = int(std::lround(surf->w / sc));
        e.h = int(std::lround(surf->h / sc));
    }
    SDL_FreeSurface(surf);

    SDL_Rect dst{x, y, e.w, e.h};
    SDL_RenderCopy(r, tex, nullptr, &dst);

    cache_.push_back(std::move(e));
    if (cache_.size() > 48) clearCache();   // 状态文字每 600ms 变一次，够用
}

int TextRenderer::measure(const std::string& s) const
{
    TTF_Font* f = hi_ ? hi_ : font_;
    if (!f) return static_cast<int>(s.size()) * 8;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return hi_ ? int(std::lround(w / scale_)) : w;
}

// ---------------------------------------------------------------------------
// StatusBar
// ---------------------------------------------------------------------------

bool StatusBar::init(SDL_Renderer* /*r*/, int winW, int y, int h)
{
    w_ = winW;
    y_ = y;
    h_ = h;
    text_.init(h >= 26 ? 15 : 12);
    return true;
}

SDL_Color StatusBar::stateColor(LinkState s) const
{
    switch (s) {
        case LinkState::Idle:        return SDL_Color{150, 150, 150, 255};
        case LinkState::WaitingPair: return SDL_Color{235, 165, 40, 255};
        case LinkState::Connecting:  return SDL_Color{240, 200, 60, 255};
        case LinkState::Streaming:   return SDL_Color{90, 205, 100, 255};
        case LinkState::Error:       return SDL_Color{225, 70, 60, 255};
    }
    return SDL_Color{150, 150, 150, 255};
}

void StatusBar::draw(SDL_Renderer* r, const SharedStatus& st) const
{
    // 背板 + 顶边
    SDL_SetRenderDrawColor(r, 0x22, 0x22, 0x26, 255);
    SDL_Rect bg{0, y_, w_, h_};
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 0x3c, 0x3c, 0x42, 255);
    SDL_RenderDrawLine(r, 0, y_, w_, y_);

    const auto  s   = static_cast<LinkState>(st.state.load(std::memory_order_relaxed));
    const SDL_Color sc = stateColor(s);
    const int   midY = y_ + h_ / 2;

    // 状态点（8x8 方块；字体缺失时这就是全部状态显示）
    SDL_SetRenderDrawColor(r, sc.r, sc.g, sc.b, 255);
    SDL_Rect dot{10, midY - 4, 8, 8};
    SDL_RenderFillRect(r, &dot);

    // 左侧：状态文本（Error 时带原因，过长就截断）
    std::string left = toString(s);
    if (s == LinkState::Error) {
        std::string msg = st.error();
        if (!msg.empty()) {
            if (msg.size() > 70) msg = msg.substr(0, 67) + "...";
            left += ": " + msg;
        }
    }
    const SDL_Color fg{230, 230, 230, 255};
    const int textY = text_.centeredY(y_, h_, left);   // 左右两段共用，保持共基线
    text_.draw(r, 24, textY, left, fg);

    // 右侧：分辨率 | fps | 帧数
    char buf[128];
    const int vw = st.videoW.load(std::memory_order_relaxed);
    const int vh = st.videoH.load(std::memory_order_relaxed);
    const int fps = st.fps.load(std::memory_order_relaxed);
    const uint64_t frames = st.frames.load(std::memory_order_relaxed);
    // 音频只在真有流时才出现（没音频流时显示 0 只会让人以为坏了）
    const uint64_t aframes = st.audioFrames.load(std::memory_order_relaxed);
    const uint64_t aunder  = st.audioUnderruns.load(std::memory_order_relaxed);
    char abuf[48] = "";
    if (aframes > 0)
        std::snprintf(abuf, sizeof abuf, " | a:%llu%s", static_cast<unsigned long long>(aframes),
                      aunder > 0 ? "!" : "");   // "!" = 音频缓冲欠载过
    if (vw > 0 && vh > 0) {
        std::snprintf(buf, sizeof buf, "%dx%d | %d fps | %llu frames%s",
                      vw, vh, fps, static_cast<unsigned long long>(frames), abuf);
    } else {
        std::snprintf(buf, sizeof buf, "allplay (M0: 显示管线未接入)");
    }
    const std::string right = buf;
    const int rightW = text_.measure(right);
    text_.draw(r, w_ - rightW - 10, textY, right, fg);
}

}  // namespace allplay
