/**
 * @file   statusbar.h
 * @brief  底部状态栏（SDL 自绘，桌面临时 UI）
 * @author blt
 */

// ============================================================================
// statusbar.h —— 底部状态栏（SDL 自绘，桌面临时 UI）
// ============================================================================
//
// 依赖 SDL_ttf + 系统字体。CJK 文本（启动页的"连接 Apple CarPlay"）需要中文字形：
// 优先 Noto Sans CJK SC（Ubuntu: fonts-noto-cjk），macOS 退到 Hiragino Sans GB。
// 拿不到字体就降级为"只画色块不写字"，不影响程序功能 —— 开发机上字体缺失
// 不该让整条链路起不来。
#pragma once

#include "appstate.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>
#include <vector>

namespace allplay {

/// 字体族：等宽适合状态栏对齐，无衬线适合启动页 logo，CJK 适合中文界面
enum class FontFace { Mono, Sans, CJK };

/// 最小文本渲染器（够状态栏用；不做排版）
class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;

    /// pixelHeight: 字号像素高（默认 15）。找不到字体返回 false（不致命）。
    /// bold: 粗体（启动页 logo 用）。TTF_Init/Quit 在多个实例间做了引用计数，
    /// 所以可以同时存在多个 TextRenderer。
    bool init(int pixelHeight = 15, FontFace face = FontFace::Mono, bool bold = false);
    bool ok() const { return font_ != nullptr; }
    int  lineHeight() const { return lh_; }

    /// 在 (x, y) 画一行；y 是行顶。
    void draw(SDL_Renderer* r, int x, int y, const std::string& s, SDL_Color c) const;

    /// 竖向居中时该给的 y：让这行字的**墨迹**居中在 [top, top+h) 里，而不是让
    /// 行框居中。各家字体的 ascent/descent 差得远（macOS 的中文字体尤其），按
    /// 行框居中会让同一份排版在 Linux 上正、在 Mac 上偏高一截。
    int centeredY(int top, int h, const std::string& s) const;

    /// 量一行文本宽度（字体不可用时按 8px/字符估）
    int measure(const std::string& s) const;

    /// 按屏幕缩放重开一份高分辨率字形，draw() 时再换算回设计稿坐标。
    /// 为什么：启动页用 SDL_RenderSetLogicalSize 等比缩放，整帧被放大 1.6 倍时
    /// 设计师字号的字形纹理也会被一起拉大 —— 边缘就是锯齿。这里按 scale 栅格化，
    /// 目标矩形仍按设计稿坐标给（SDL 再把它们映射回等量设备像素），字就是 1:1 清晰的。
    void setRasterScale(double scale);

    /// 丢掉字形纹理缓存。**销毁渲染器之前必须调用** —— 纹理是渲染器的子对象，
    /// 渲染器没了再 SDL_DestroyTexture 就是重复释放。
    void clearCache() const;

private:
    struct CachedText {
        std::string s;
        SDL_Color   c{};
        uint32_t    gen = 0;
        SDL_Texture* tex = nullptr;
        int          w = 0, h = 0;
    };
    struct InkSpan {                 // 一行字的墨迹上下沿（相对行顶，设计稿像素）
        std::string s;
        uint32_t    gen = 0;
        double      top = 0, bot = 0;
    };
    mutable std::vector<CachedText> cache_;   // draw() 是 const，缓存得可变
    mutable std::vector<InkSpan>    ink_;
    uint32_t gen_ = 1;                        // 字号一变就 +1，旧纹理全部作废

    /// 量一行字的墨迹上下沿（渲染一次、扫描 alpha，结果缓存）
    bool inkSpan(const std::string& s, double* top, double* bot) const;

    TTF_Font* font_ = nullptr;    // 设计稿字号（量宽/行高按它算，纵向排版的基准）
    TTF_Font* hi_   = nullptr;    // 设备像素上的字形（scale ≠ 1 时才有）
    int       lh_   = 16;
    int       px_   = 15;
    double    scale_ = 1.0;
    FontFace  face_  = FontFace::Mono;
    bool      bold_  = false;
    bool      hasRealBold_ = false;
};

/// 底部状态栏：左侧 = 状态点 + 状态文本；右侧 = 分辨率 / fps / 帧数
class StatusBar {
public:
    /// y = 状态栏顶部的窗口坐标；h = 高度
    bool init(SDL_Renderer* r, int winW, int y, int h);

    void draw(SDL_Renderer* r, const SharedStatus& st) const;

    int height() const { return h_; }

private:
    TextRenderer text_;
    SDL_Color    stateColor(LinkState s) const;
    int  y_ = 0, h_ = 24, w_ = 0;
};

}  // namespace allplay
