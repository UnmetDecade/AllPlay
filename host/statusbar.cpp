#include "statusbar.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace allplay {

namespace {

// Font candidates (the first one that opens is used). Can be overridden by the ALLPLAY_FONT env var.
// index: which face inside the .ttc (Noto Sans CJK's SC is #2; JP=0/KR=1/SC=2/TC=3/HK=4).
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

// Chinese UI: prefer fonts with the full CJK glyph set (they also cover Latin, so DejaVu need not be combined in)
const FontFile kCjkCandidates[] = {
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},   // SC
    {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
    {"/usr/share/fonts/truetype/arphic/uming.ttc", 0},
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},               // macOS
    {"/System/Library/Fonts/STHeiti Light.ttc", 0},
};

// Bold prefers a real bold face (looks better than TTF's synthetic bold); fall back to synthetic if unavailable
const FontFile kBoldCandidates[] = {
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 0},
    {"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", 0},
};

const FontFile kCjkBoldCandidates[] = {
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", 2},
    // In Hiragino Sans GB's .ttc: 0=W3, 1=Interface W3, **2=W6**, 3=Interface W6.
    // The 1 written before was actually the regular face, so the Mac "bold" was not bold at all (user: "make bold bolder").
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", 2},
};

bool openFont(const FontFile& f, int px, TTF_Font** out)
{
    *out = f.index > 0 ? TTF_OpenFontIndex(f.path, px, f.index)
                       : TTF_OpenFont(f.path, px);
    return *out != nullptr;
}

/// Pick a font by family: for bold, first look for a real bold face, else use the main font + synthetic bold
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
    // Synthesize only when no real bold face is available (synthetic bold blurs the edges; use a real bold face whenever possible)
    if (f && bold && !*realBold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    return f;
}

// TTF_Init/TTF_Quit is library-level global state: with multiple TextRenderers alive it must be reference-counted,
// otherwise the first one destroyed takes the still-live fonts down with it.
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
    ink_.clear();          // ink measurements are in-line offsets at the "current font size"; a size change invalidates them
}

bool TextRenderer::init(int pixelHeight, FontFace face, bool bold)
{
    if (!font_) {
        if (g_ttfRefs == 0 && TTF_Init() != 0) {
            std::fprintf(stderr, "[statusbar] TTF_Init failed: %s (status bar will not show text)\n",
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
            std::fprintf(stderr, "[statusbar] Cannot open ALLPLAY_FONT=\"%s\": %s\n",
                         env, TTF_GetError());
    }
    if (!font_) font_ = openFace(pixelHeight, face, bold, &hasRealBold_);
    if (!font_) {
        std::fprintf(stderr, "[statusbar] No usable font found (is fonts-dejavu-core installed?)"
                             " — status bar degraded to color blocks\n");
        return false;
    }
    lh_ = TTF_FontLineSkip(font_);
    return true;
}

void TextRenderer::setRasterScale(double scale)
{
    if (scale < 0.25) scale = 0.25;
    if (scale > 4.0)  scale = 4.0;
    if (std::abs(scale - scale_) < 0.02) return;   // scale unchanged, no need to reopen the font

    clearCache();          // font size changed; all cached glyph textures are invalid
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

    // Render once and scan alpha to find the ink's top/bottom edges. Results are cached by (string, font generation) -- the launcher
    // asks about the same batch of strings every frame, and the actual scan happens only once.
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
    if (y1 < y0) return false;      // empty line (all whitespace)

    // The surface is rasterized at "device pixels"; convert back to design coordinates (draw() maps back at the same ratio)
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
    return top + (h - lineHeight()) / 2;    // fall back to line-box centering if it cannot be measured
}

void TextRenderer::draw(SDL_Renderer* r, int x, int y, const std::string& s,
                        SDL_Color c) const
{
    TTF_Font* f = hi_ ? hi_ : font_;
    if (!f || s.empty()) return;

    // Glyph texture cache: the launcher redraws every frame, but each text/color has only one copy -- calling
    // TTF_RenderUTF8_Blended on CJK every frame is the source of 90% CPU.
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
    if (cache_.size() > 48) clearCache();   // the status text changes every 600 ms, so this is plenty
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
    // Backing panel + top edge
    SDL_SetRenderDrawColor(r, 0x22, 0x22, 0x26, 255);
    SDL_Rect bg{0, y_, w_, h_};
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 0x3c, 0x3c, 0x42, 255);
    SDL_RenderDrawLine(r, 0, y_, w_, y_);

    const auto  s   = static_cast<LinkState>(st.state.load(std::memory_order_relaxed));
    const SDL_Color sc = stateColor(s);
    const int   midY = y_ + h_ / 2;

    // Status dot (8x8 square; when the font is missing this is the entire status display)
    SDL_SetRenderDrawColor(r, sc.r, sc.g, sc.b, 255);
    SDL_Rect dot{10, midY - 4, 8, 8};
    SDL_RenderFillRect(r, &dot);

    // Left: status text (with the reason on Error, truncated if too long)
    std::string left = toString(s);
    if (s == LinkState::Error) {
        std::string msg = st.error();
        if (!msg.empty()) {
            if (msg.size() > 70) msg = msg.substr(0, 67) + "...";
            left += ": " + msg;
        }
    }
    const SDL_Color fg{230, 230, 230, 255};
    const int textY = text_.centeredY(y_, h_, left);   // shared by both segments to keep a common baseline
    text_.draw(r, 24, textY, left, fg);

    // Right: resolution | fps | frame count
    char buf[128];
    const int vw = st.videoW.load(std::memory_order_relaxed);
    const int vh = st.videoH.load(std::memory_order_relaxed);
    const int fps = st.fps.load(std::memory_order_relaxed);
    const uint64_t frames = st.frames.load(std::memory_order_relaxed);
    // Audio appears only when a stream actually exists (showing 0 with no audio stream just looks broken)
    const uint64_t aframes = st.audioFrames.load(std::memory_order_relaxed);
    const uint64_t aunder  = st.audioUnderruns.load(std::memory_order_relaxed);
    char abuf[48] = "";
    if (aframes > 0)
        std::snprintf(abuf, sizeof abuf, " | a:%llu%s", static_cast<unsigned long long>(aframes),
                      aunder > 0 ? "!" : "");   // "!" = the audio buffer has underrun
    if (vw > 0 && vh > 0) {
        std::snprintf(buf, sizeof buf, "%dx%d | %d fps | %llu frames%s",
                      vw, vh, fps, static_cast<unsigned long long>(frames), abuf);
    } else {
        std::snprintf(buf, sizeof buf, "allplay (M0: display pipeline not connected)");
    }
    const std::string right = buf;
    const int rightW = text_.measure(right);
    text_.draw(r, w_ - rightW - 10, textY, right, fg);
}

}  // namespace allplay
