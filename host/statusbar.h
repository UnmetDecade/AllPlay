/**
 * @file   statusbar.h
 * @brief  Bottom status bar (SDL-drawn, temporary desktop UI)
 * @author blt
 */

// ============================================================================
// statusbar.h -- Bottom status bar (SDL-drawn, temporary desktop UI)
// ============================================================================
//
// Depends on SDL_ttf + system fonts. CJK text (the launcher's "Connect to Apple CarPlay") needs Chinese glyphs:
// preferably Noto Sans CJK SC (Ubuntu: fonts-noto-cjk), falling back to Hiragino Sans GB on macOS.
// If no font is available it degrades to "draw color blocks only, no text", which does not affect
// functionality -- a missing font on a dev box should not keep the whole pipeline from starting.
#pragma once

#include "appstate.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>
#include <vector>

namespace allplay {

/// Font family: mono suits status-bar alignment, sans suits the launcher logo, CJK suits the Chinese UI
enum class FontFace { Mono, Sans, CJK };

/// Minimal text renderer (enough for the status bar; no layout engine)
class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;

    /// pixelHeight: font pixel height (default 15). Returns false if no font is found (not fatal).
    /// bold: bold face (for the launcher logo). TTF_Init/Quit is reference-counted across instances,
    /// so multiple TextRenderers can coexist.
    bool init(int pixelHeight = 15, FontFace face = FontFace::Mono, bool bold = false);
    bool ok() const { return font_ != nullptr; }
    int  lineHeight() const { return lh_; }

    /// Draw one line at (x, y); y is the top of the line.
    void draw(SDL_Renderer* r, int x, int y, const std::string& s, SDL_Color c) const;

    /// The y to use for vertical centering: center the line's **ink** within [top, top+h) rather than
    /// centering the line box. Fonts differ widely in ascent/descent (Chinese fonts on macOS especially); centering by
    /// line box makes the same layout correct on Linux but noticeably high on Mac.
    int centeredY(int top, int h, const std::string& s) const;

    /// Measure the width of one line of text (estimates 8 px/char when the font is unavailable)
    int measure(const std::string& s) const;

    /// Reopen a high-resolution set of glyphs at the screen scale; draw() then maps back to design coordinates.
    /// Why: the launcher scales proportionally with SDL_RenderSetLogicalSize, so when the whole frame is blown up 1.6x
    /// the designer-size glyph textures get stretched too -- and the edges turn jagged. Here they are rasterized at scale,
    /// while target rects stay in design coordinates (SDL maps them back to the same device pixels), so text is crisp 1:1.
    void setRasterScale(double scale);

    /// Drop the glyph texture cache. **Must be called before destroying the renderer** -- textures are children of the renderer,
    /// so calling SDL_DestroyTexture after it is gone is a double free.
    void clearCache() const;

private:
    struct CachedText {
        std::string s;
        SDL_Color   c{};
        uint32_t    gen = 0;
        SDL_Texture* tex = nullptr;
        int          w = 0, h = 0;
    };
    struct InkSpan {                 // Ink top/bottom of one line (relative to line top, design pixels)
        std::string s;
        uint32_t    gen = 0;
        double      top = 0, bot = 0;
    };
    mutable std::vector<CachedText> cache_;   // draw() is const, so the cache must be mutable
    mutable std::vector<InkSpan>    ink_;
    uint32_t gen_ = 1;                        // Incremented on every font-size change; all old textures become invalid

    /// Measure the ink top/bottom of one line (render once, scan alpha, cache the result)
    bool inkSpan(const std::string& s, double* top, double* bot) const;

    TTF_Font* font_ = nullptr;    // Design-size font (width/line-height measured with it, baseline for vertical layout)
    TTF_Font* hi_   = nullptr;    // Glyphs at device pixels (only when scale != 1)
    int       lh_   = 16;
    int       px_   = 15;
    double    scale_ = 1.0;
    FontFace  face_  = FontFace::Mono;
    bool      bold_  = false;
    bool      hasRealBold_ = false;
};

/// Bottom status bar: left = status dot + status text; right = resolution / fps / frame count
class StatusBar {
public:
    /// y = window coordinate of the status bar top; h = height
    bool init(SDL_Renderer* r, int winW, int y, int h);

    void draw(SDL_Renderer* r, const SharedStatus& st) const;

    int height() const { return h_; }

private:
    TextRenderer text_;
    SDL_Color    stateColor(LinkState s) const;
    int  y_ = 0, h_ = 24, w_ = 0;
};

}  // namespace allplay
