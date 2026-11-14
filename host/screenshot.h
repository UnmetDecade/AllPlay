/**
 * @file   screenshot.h
 * @brief  Read the current render result back to PPM (to verify unattended what the picture looks like)
 * @author blt
 */

// ============================================================================
// host/screenshot.h -- Read render result back -> PPM
// ============================================================================
// Why it exists: much of this project's acceptance testing happens on **headless** machines
// (CI, dev boxes reached over ssh), and "is the splash right", "are red and blue swapped",
// "is the letterbox placement right" are all visual questions.
// Eyeballing the screen cannot be automated and is impossible without a screen, so instead
// we read the window pixels back to disk and let a script judge.
//
// What is read back is the renderer's **final composited result**, so it covers the intermediate
// stages (texture format, blending, letterbox) -- more convincing than inspecting one layer's output alone.
#pragma once

#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>
#include <cstddef>

namespace allplay {

/// Save the renderer's current contents as a binary PPM (P6). Returns false = read-back or file write failed.
/// NOTE: must be called **before** SDL_RenderPresent and after the frame is drawn -- GL's
/// backbuffer contents are undefined after Present; on Mac it happens to read fine, but llvmpipe (110)
/// reads back all black.
inline bool dumpRender(SDL_Renderer* ren, const std::string& path)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    if (w <= 0 || h <= 0) return false;

    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             px.data(), w * 4) != 0) {
        std::fprintf(stderr, "[shot] Failed to read back render result: %s\n", SDL_GetError());
        return false;
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[shot] Cannot write %s\n", path.c_str());
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    // ARGB8888 bit layout is fixed (A31-24 R23-16 G15-8 B7-0); on little-endian machines memory order is B,G,R.
    // All target platforms (x86_64 / aarch64 Linux, arm64 macOS) are little-endian.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            std::fputc(p[2], f);
            std::fputc(p[1], f);
            std::fputc(p[0], f);
        }
    }
    std::fclose(f);
    std::printf("[shot] Render result saved to %s (%dx%d)\n", path.c_str(), w, h);
    return true;
}

}  // namespace allplay
