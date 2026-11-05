/**
 * AllPlay 设计令牌 —— 与 host/launcher.cpp 里那份一一对应
 *
 * 颜色全部照搬桌面的启动页（白底黑字、极淡的分隔线、蓝色点缀）。改这里等于同时
 * 改两端，别在页面里写死色值。
 */

export const C_INK: string = '#17191F';          // 主文字（近黑）
export const C_MUTED: string = '#737985';        // 副标题
export const C_BLUE: string = '#2878F0';         // 主蓝
export const C_EYEBROW: string = '#5D6470';      // 小标题
export const C_HINT: string = '#858B96';         // 提示文字
export const C_FOOTER: string = '#4E5560';       // 页脚
export const C_RULE: string = '#F0F1F4';         // 顶栏/页脚分隔线
export const C_CARD_LINE: string = '#E8EBF1';    // 卡片描边
export const C_LANG_LINE: string = '#E5E8EE';    // 右上角胶囊描边
export const C_VISUAL_LINE: string = '#EDF0F4';  // 示意图描边
export const C_VISUAL_BG: string = '#FAFBFD';    // 示意图底色
export const C_TILE_BG: string = '#EDF5FF';      // 设备卡片里的图标底
export const C_GREEN: string = '#25B878';        // 页脚状态点（空闲）
export const C_RED: string = '#E5484D';          // 页脚状态点（失败）
export const C_ICON_BLUE: string = '#1E9BE0';    // 蓝牙图符
export const C_WHITE: string = '#FFFFFF';

/// 设计稿尺寸（桌面那份 1540×850 的坐标系）
export const DESIGN_W: number = 1540;
export const DESIGN_H: number = 850;
