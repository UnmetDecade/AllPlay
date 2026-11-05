/**
 * @file   screenshot.h
 * @brief  把当前渲染结果读回来存成 PPM（无人值守时校验"画面到底长什么样"）
 * @author blt
 */

// ============================================================================
// host/screenshot.h —— 渲染结果读回 → PPM
// ============================================================================
// 为什么需要它：这个项目的验收大量发生在**没有显示器**的机器上（CI、ssh 进去的
// 开发机），而"启动页对不对""红蓝有没有互换""letterbox 落点对不对"全是视觉问题。
// 靠肉眼看屏幕既不可自动化、也没法在没有屏幕时做，所以改成把窗口像素读回来落盘，
// 再由脚本判定。
//
// 读回的是渲染器**最终合成结果**，所以它同时覆盖了纹理格式、混合、letterbox 这些
// 中间环节 —— 比单独去查某一层的输出更有说服力。
#pragma once

#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>
#include <cstddef>

namespace allplay {

/// 把渲染器当前内容存成二进制 PPM（P6）。返回 false = 读回或写文件失败。
/// 注意：必须在 SDL_RenderPresent **之前**、画完这一帧之后调用 —— GL 的
/// backbuffer 在 Present 之后内容未定义，Mac 上碰巧读得到，llvmpipe（110）
/// 读回来是全黑。
inline bool dumpRender(SDL_Renderer* ren, const std::string& path)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    if (w <= 0 || h <= 0) return false;

    std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             px.data(), w * 4) != 0) {
        std::fprintf(stderr, "[shot] 读回渲染结果失败：%s\n", SDL_GetError());
        return false;
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[shot] 写不了 %s\n", path.c_str());
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    // ARGB8888 位布局固定（A31-24 R23-16 G15-8 B7-0）；小端机内存序即 B,G,R。
    // 目标平台（x86_64 / aarch64 Linux、arm64 macOS）都是小端。
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
            std::fputc(p[2], f);
            std::fputc(p[1], f);
            std::fputc(p[0], f);
        }
    }
    std::fclose(f);
    std::printf("[shot] 渲染结果已存 %s（%dx%d）\n", path.c_str(), w, h);
    return true;
}

}  // namespace allplay
