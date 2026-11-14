/**
 * @file   launcher.h
 * @brief  Launcher page (the "Connect to Apple CarPlay" pairing guide)
 * @author blt
 */

// ============================================================================
// host/launcher.h -- Pairing guide page (implemented from the AI IPlay design, black on white)
// ============================================================================
// Layout:
//   Top bar: AI square wordmark + AIIPlay + language pill on the right (EN / Chinese)
//   Center: large title "Connect to Apple CarPlay" + subtitle
//   Two columns: left = device Bluetooth name card + pairing hint; right = phone -> Bluetooth -> head-unit diagram
//   Bottom: cancel button (white text on dark background; clicking it exits the program)
//   Footer: status dot + status text (left) · AIIPlay · CarPlay (right)
// All diagrams are drawn from SDL primitives at runtime, no bitmap assets needed.
//
// All coordinates are in the 1540x850 design coordinate system; at render time they are scaled
// proportionally and centered by logical size -- changing the window size touches no numbers.
// Missing fonts are not fatal: draw shapes only, no text.
#pragma once

#include "appstate.h"
#include "statusbar.h"

#include <SDL.h>

#include <cstdint>
#include <string>

namespace allplay {

class Launcher {
public:
    /// Build the glyphs. Returns false = no font, draw shapes only.
    bool init();

    /// Device Bluetooth name (the name seen in the phone's Bluetooth list)
    void setDeviceName(const std::string& name) { deviceName_ = name; }

    /// Draw one frame. nowMs drives the Bluetooth ripple breathing and the status text ellipsis.
    void draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs);

    /// Renderer pixels / window logical points ratio (2 on high DPI). Mouse events arrive in logical points,
    /// so multiply by it before hit-testing. The caller only needs to update it once per frame.
    void setPixelRatio(double s) { pixelRatio_ = s > 0.0 ? s : 1.0; }

    /// Handle one mouse event. Returns true = the user clicked "Cancel"; the caller should exit.
    /// Language toggles are consumed here (not forwarded).
    bool handleEvent(const SDL_Event& e, int winW, int winH);

    /// Drop the text texture cache. **Must be called before SDL_DestroyRenderer**.
    void releaseTextures();

private:
    enum class Lang { Zh, En };

    struct Layout {
        int    w     = 1540;  // Logical canvas width (proportionally adaptive, see logicalW)
        int    h     = 850;
        double scale = 1.0;   // Design -> window
        double ox    = 0.0;   // Centering offset (window coordinates)
        double oy    = 0.0;
        SDL_Rect lang{};      // Everything below is in design coordinates
        SDL_Rect cancel{};
    };
    Layout layoutFor(int winW, int winH) const;   // Uses font size and line height, hence a member function

    /// Draw the whole page to the current render target (design coordinates; the caller binds the 2x texture and scales by kSS)
    void drawScene(SDL_Renderer* r, const Layout& L, LinkState st, uint64_t nowMs,
                   double rasterScale);

    /// Logical canvas width: height is fixed at 850, wide screens grow sideways (head units are often 2.6:1).
    /// Otherwise an ultrawide screen leaves a large white border on each side and the top/footer rules only reach 1540.
    static int logicalW(int winW, int winH);

    TextRenderer title_;     ///< Large title
    TextRenderer body_;      ///< Subtitle
    TextRenderer eyebrow_;   ///< "Device Bluetooth name"
    TextRenderer device_;    ///< Bluetooth name
    TextRenderer hint_;      ///< Pairing hint
    TextRenderer button_;    ///< Cancel
    TextRenderer footer_;    ///< Footer / language pill
    TextRenderer brand_;     ///< AIIPlay
    // The wordmark is no longer text: the top-bar one is a C+play icon drawn by drawPlayMark()

    std::string deviceName_ = "AllPlay";
    Lang        lang_       = Lang::Zh;
    int         winW_ = 0, winH_ = 0;   ///< Window pixels of the previous frame (mouse coordinates are mapped back through it)
    double      pixelRatio_ = 1.0;      ///< Renderer pixels / window logical points

    // 2x supersampled scene cache: redrawn only on animation/state changes, other frames just blit a texture
    SDL_Texture* scene_ = nullptr;
    int          sceneW_ = 0, sceneH_ = 0;
    bool         sceneWarned_ = false;
    uint64_t     sceneBucket_ = ~0ull;
    LinkState    sceneSt_     = LinkState::Idle;
    Lang         sceneLang_   = Lang::Zh;
    std::string  sceneName_;
};

}  // namespace allplay
