/**
 * 右栏示意图：手机 → 蓝牙 → 中控屏
 *
 * 完全照 host/launcher.cpp 的 drawScene() 右半段搬过来：设计稿是 620×285 的
 * SVG 坐标系，这里按可用区域等比缩放并居中，所以换屏幕尺寸不用改任何一个数字。
 * 中间那颗蓝牙图符带"波纹扩散"动效（三道弧错开 1/3 周期往外走并渐隐）——
 * 和桌面一样按 10 fps 重画就够，静态部分每次都整幅重绘，开销可以忽略。
 */

import { traceRoundRect } from './Glyphs';
import { C_ICON_BLUE } from '../common/Theme';

const DESIGN_VW: number = 620;
const DESIGN_VH: number = 285;

@Component
export struct ConnectVisual {
  @Prop heightVp: number = 200;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);
  private timer: number = -1;
  private ready: boolean = false;
  private phase: number = 0;
  private w: number = 0;
  private h: number = 0;

  aboutToAppear(): void {
    this.timer = setInterval(() => {
      this.phase = (this.phase + 0.1) % 1.0;   // 100ms × 10 = 1 秒一轮（桌面是 2 秒，手机上快一点更精神）
      if (this.ready) this.draw();
    }, 100);
  }

  aboutToDisappear(): void {
    if (this.timer !== -1) {
      clearInterval(this.timer);
      this.timer = -1;
    }
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height(this.heightVp)
      .onReady(() => {
        this.w = this.ctx.width;
        this.h = this.ctx.height;
        this.ready = true;
        this.draw();
      })
  }

  // ---- 绘制原语 ----
  private rectF(x: number, y: number, w: number, h: number, r: number, color: string): void {
    const c: CanvasRenderingContext2D = this.ctx;
    c.fillStyle = color;
    if (r <= 0) {
      c.fillRect(x, y, w, h);
    } else {
      traceRoundRect(c, x, y, w, h, r);
      c.fill();
    }
  }

  private circleF(cx: number, cy: number, r: number, color: string): void {
    const c: CanvasRenderingContext2D = this.ctx;
    c.fillStyle = color;
    c.beginPath();
    c.arc(cx, cy, r, 0, 2 * Math.PI);
    c.fill();
  }

  private triF(x0: number, y0: number, x1: number, y1: number, x2: number, y2: number,
               color: string): void {
    const c: CanvasRenderingContext2D = this.ctx;
    c.fillStyle = color;
    c.beginPath();
    c.moveTo(x0, y0);
    c.lineTo(x1, y1);
    c.lineTo(x2, y2);
    c.closePath();
    c.fill();
  }

  private line(x: number, y: number, w: number, h: number, color: string): void {
    this.rectF(x, y, w, h, 0, color);
  }

  /// 蓝牙图符（带脉冲动效）
  private bluetooth(cx: number, cy: number, outerR: number, color: string, pulse: number): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const h: number = outerR * 0.82;
    const rod: number = Math.max(1, outerR * 0.115);
    const sx: number = cx - (1.42 - 0.636) / 2 * h;
    const sy: number = cy;

    const path: number[][] = [
      [-0.636, 0.559], [0.636, -0.429], [0.015, -1.000],
      [0.015, 1.000], [0.636, 0.429], [-0.636, -0.559]
    ];
    c.strokeStyle = color;
    c.lineWidth = rod;
    c.lineCap = 'round';
    c.lineJoin = 'round';
    c.beginPath();
    c.moveTo(sx + path[0][0] * h, sy + path[0][1] * h);
    for (let i = 1; i < path.length; i++) {
      c.lineTo(sx + path[i][0] * h, sy + path[i][1] * h);
    }
    c.stroke();

    // 三道弧错开 1/3 周期，从内弧位置往外走并渐隐
    const span: number = 0.55;
    for (let i = 0; i < 3; i++) {
      const p: number = (pulse + i / 3.0) % 1.0;
      const a: number = 1.0 - p;
      if (a <= 0) continue;
      c.strokeStyle = 'rgba(30, 155, 224, ' + a.toFixed(3) + ')';
      c.beginPath();
      c.arc(sx, sy, (0.88 + 1.25 * p) * h, -span, span);
      c.stroke();
    }
  }

  private draw(): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const W: number = this.w;
    const H: number = this.h;
    c.clearRect(0, 0, W, H);

    const sv: number = Math.min((W - 24) / DESIGN_VW, (H - 24) / DESIGN_VH);
    const ox: number = (W - DESIGN_VW * sv) / 2;
    const oy: number = (H - DESIGN_VH * sv) / 2;
    const X = (v: number): number => ox + v * sv;
    const Y = (v: number): number => oy + v * sv;
    const S = (v: number): number => v * sv;

    const shadow = (x: number, y: number, w: number, h: number, r: number): void => {
      c.save();
      c.shadowColor = 'rgba(26, 38, 58, 0.10)';
      c.shadowBlur = 8;
      c.shadowOffsetY = 3;
      this.rectF(x, y, w, h, r, '#FFFFFF');
      c.restore();
    };

    // ---------------- 手机 ----------------
    shadow(X(20), Y(55), S(88), S(174), S(15));
    this.rectF(X(20), Y(55), S(88), S(174), S(15), '#242A34');
    this.rectF(X(25), Y(61), S(78), S(162), S(11), '#EEF2F7');
    this.rectF(X(48), Y(66), S(32), S(5), S(2), '#C5CBD5');
    this.circleF(X(64), Y(198), S(4), '#C5CBD5');
    this.rectF(X(42), Y(116), S(44), S(44), S(12), '#25C77A');
    this.triF(X(58), Y(126), X(75), Y(138), X(58), Y(150), '#FFFFFF');

    // ---------------- 蓝牙 ----------------
    this.bluetooth(X(198), Y(143), S(19), C_ICON_BLUE, this.phase);

    // ---------------- 中控屏 ----------------
    shadow(X(284), Y(24), S(316), S(230), S(13));
    this.rectF(X(284), Y(24), S(316), S(230), S(13), '#202733');
    this.rectF(X(291), Y(31), S(302), S(216), S(9), '#F7F9FC');
    this.rectF(X(301), Y(42), S(282), S(164), S(5), '#E9EDF4');

    // 三块面板
    for (let i = 0; i < 3; i++) {
      this.rectF(X(308 + i * 92), Y(49), S(84), S(150), S(5), '#E6EAF1');
    }

    // 面板里的应用图标
    this.rectF(X(325), Y(111), S(50), S(50), S(12), '#FFFFFF');
    this.triF(X(343), Y(122), X(362), Y(136), X(343), Y(150), '#28BD78');

    this.rectF(X(416), Y(111), S(52), S(50), S(12), '#FFFFFF');
    this.rectF(X(508), Y(111), S(52), S(50), S(12), '#FFFFFF');

    // 第二块：三条列表线
    for (let i = 0; i < 3; i++) {
      const y: number = Y(127 + i * 9);
      this.line(X(431), y, S(i === 2 ? 15 : 22), Math.max(2, S(4)), '#6C7A91');
    }
    // 第三块：一座小山
    this.triF(X(524), Y(145), X(534), Y(126), X(544), Y(145), '#6C7A91');

    // 底部 dock
    this.rectF(X(301), Y(211), S(282), S(26), S(4), '#EDF1F6');
    const dockIcons: string[] = ['#2878F0', '#F2C94C', '#28BD78', '#EF9B78'];
    for (let i = 0; i < 4; i++) {
      this.rectF(X(314 + i * 28), Y(217), S(15), S(15), S(4), dockIcons[i]);
    }
    // 黄色 / 橙色图标里的两条白线
    for (let i = 0; i < 2; i++) {
      this.line(X(348), Y(221 + i * 4), S(7), Math.max(1, S(1.5)), '#FFFFFF');
      this.line(X(404), Y(221 + i * 4), S(7), Math.max(1, S(1.5)), '#FFFFFF');
    }
    this.triF(X(374), Y(221), X(381), Y(224.5), X(374), Y(228), '#FFFFFF');

    // dock 右侧：分隔线 + 播放 / 暂停 / 翻页
    this.line(X(442), Y(224), S(48), Math.max(1, S(1.5)), '#D5DBE4');
    c.strokeStyle = '#9AA5B5';
    c.lineWidth = Math.max(1, S(1.4));
    c.beginPath();
    c.arc(X(516), Y(224.5) + 1, S(5.5), 0, 2 * Math.PI);
    c.stroke();
    this.triF(X(515), Y(222), X(518), Y(225), X(515), Y(228), '#9AA5B5');
    for (let i = 0; i < 2; i++) {
      this.line(X(538 + i * 13), Y(222), Math.max(1, S(2)), S(5), '#9AA5B5');
    }
    this.triF(X(568), Y(222), X(572), Y(225), X(568), Y(228), '#9AA5B5');
  }
}
