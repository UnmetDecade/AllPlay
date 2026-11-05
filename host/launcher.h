/**
 * @file   launcher.h
 * @brief  启动页（"连接 Apple CarPlay"连接引导页）
 * @author blt
 */

// ============================================================================
// host/launcher.h —— 连接引导页（按 AI IPlay 的设计稿实现，白底黑字）
// ============================================================================
// 版式：
//   顶栏：AI 方块字标 + AIIPlay + 右侧语言胶囊（EN / 中文）
//   居中：大标题「连接 Apple CarPlay」+ 副标题
//   两栏：左边设备蓝牙名称卡片 + 配对提示；右边手机 → 蓝牙 → 中控屏 示意图
//   底部：取消按钮（深底白字，点了退出程序）
//   页脚：状态点 + 状态文字（左）· AIIPlay · CarPlay（右）
// 示意图全部是 SDL 图元现画，不依赖位图素材。
//
// 全部坐标写在 1540×850 的设计稿坐标系里，渲染时按 logical size 等比缩放居中
// —— 换窗口尺寸不用改任何一个数字。字体拿不到时不致命：只画图形不写字。
#pragma once

#include "appstate.h"
#include "statusbar.h"

#include <SDL.h>

#include <cstdint>
#include <string>

namespace allplay {

class Launcher {
public:
    /// 建字形。返回 false = 没字体，只画图形。
    bool init();

    /// 设备蓝牙名称（手机蓝牙列表里看到的那个名字）
    void setDeviceName(const std::string& name) { deviceName_ = name; }

    /// 画一帧。nowMs 驱动蓝牙波纹的呼吸和状态文字的省略号。
    void draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs);

    /// 渲染器像素 / 窗口逻辑点 的比值（高 DPI 下是 2）。鼠标事件按逻辑点来，
    /// 判断点击前要先乘它。调用方每帧更新一次即可。
    void setPixelRatio(double s) { pixelRatio_ = s > 0.0 ? s : 1.0; }

    /// 处理一次鼠标事件。返回 true = 用户点了「取消」，调用方应当退出。
    /// 语言切换在这里自己吃掉（不发出去）。
    bool handleEvent(const SDL_Event& e, int winW, int winH);

    /// 丢掉文字纹理缓存。**SDL_DestroyRenderer 之前必须调用**。
    void releaseTextures();

private:
    enum class Lang { Zh, En };

    struct Layout {
        int    w     = 1540;  // 逻辑画布宽（等比自适应，见 logicalW）
        int    h     = 850;
        double scale = 1.0;   // 设计稿 → 窗口
        double ox    = 0.0;   // 居中偏移（窗口坐标）
        double oy    = 0.0;
        SDL_Rect lang{};      // 以下都是设计稿坐标
        SDL_Rect cancel{};
    };
    Layout layoutFor(int winW, int winH) const;   // 用到字号行高，所以是成员函数

    /// 把整页画到当前渲染目标（设计稿坐标；调用方负责绑定 2× 纹理并按 kSS 放大）
    void drawScene(SDL_Renderer* r, const Layout& L, LinkState st, uint64_t nowMs,
                   double rasterScale);

    /// 逻辑画布宽：高度钉在 850，宽屏就往两边长（车机屏常见 2.6:1）。
    /// 不这么做的话超宽屏会左右各留一大条白边、顶栏/页脚横线只画到 1540。
    static int logicalW(int winW, int winH);

    TextRenderer title_;     ///< 大标题
    TextRenderer body_;      ///< 副标题
    TextRenderer eyebrow_;   ///< "设备蓝牙名称"
    TextRenderer device_;    ///< 蓝牙名称
    TextRenderer hint_;      ///< 配对提示
    TextRenderer button_;    ///< 取消
    TextRenderer footer_;    ///< 页脚 / 语言胶囊
    TextRenderer brand_;     ///< AIIPlay
    // 字标不再用文字：顶栏那颗是 drawPlayMark() 画的 C+播放 图标

    std::string deviceName_ = "AllPlay";
    Lang        lang_       = Lang::Zh;
    int         winW_ = 0, winH_ = 0;   ///< 上一帧的窗口像素（鼠标坐标要按它反算）
    double      pixelRatio_ = 1.0;      ///< 渲染器像素 / 窗口逻辑点

    // 2× 超采样场景缓存：只在动画/状态变化时重画，其余帧只是贴一张纹理
    SDL_Texture* scene_ = nullptr;
    int          sceneW_ = 0, sceneH_ = 0;
    bool         sceneWarned_ = false;
    uint64_t     sceneBucket_ = ~0ull;
    LinkState    sceneSt_     = LinkState::Idle;
    Lang         sceneLang_   = Lang::Zh;
    std::string  sceneName_;
};

}  // namespace allplay
