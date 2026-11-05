/**
 * 界面里那几个矢量图符 —— 全部用 Canvas 现画，和 host/launcher.cpp 同一套几何参数
 *
 *   · PlayMark       顶栏品牌标：开口圆环 + 圆角播放三角
 *   · BluetoothGlyph 蓝牙图符：六点折线符文 + 右侧两道信号弧（无外圈）
 *   · GearIcon       设置按钮里的齿轮（桌面那版这里是语言胶囊，鸿蒙换成设置）
 *   · ChevronLeft    设置页返回箭头
 *
 * 都不依赖位图素材，换尺寸只需改 @Prop。
 */

import { C_ICON_BLUE, C_INK } from '../common/Theme';

/// 圆角矩形路径（ArkTS 的 canvas 没有现成的 roundRect，自己拼四段弧）
export function traceRoundRect(c: CanvasRenderingContext2D, x: number, y: number,
                               w: number, h: number, r: number): void {
  const rad: number = Math.min(r, Math.min(w, h) / 2);
  c.beginPath();
  c.moveTo(x + rad, y);
  c.lineTo(x + w - rad, y);
  c.arcTo(x + w, y, x + w, y + rad, rad);
  c.lineTo(x + w, y + h - rad);
  c.arcTo(x + w, y + h, x + w - rad, y + h, rad);
  c.lineTo(x + rad, y + h);
  c.arcTo(x, y + h, x, y + h - rad, rad);
  c.lineTo(x, y + rad);
  c.arcTo(x, y, x + rad, y, rad);
  c.closePath();
}

// ============================================================================
// 品牌标
// ============================================================================
// 深色开口圆环 + 环心一个圆角播放三角。缺口在右侧略偏上（84°，中心 -8°），
// 两端圆头；三角等边、尖朝右，三个角倒圆。
// 倒圆的偷懒做法：先填三角，再用同样颜色、lineJoin=round 的粗描边包一圈 ——
// 等价于「三角 + 半径 cr 的圆」做闵可夫斯基和，角上正好是正圆弧。
@Component
export struct PlayMark {
  @Prop diameter: number = 28;
  @Prop color: string = C_INK;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);

  build() {
    Canvas(this.ctx)
      .width(this.diameter)
      .height(this.diameter)
      .onReady(() => this.draw())
  }

  private draw(): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const S: number = this.diameter;
    const cx: number = S / 2;
    const cy: number = S / 2;
    c.clearRect(0, 0, S, S);

    // ① 开口圆环：+34° 绕到 +310°，缺口 84°，缺口中心 -8°
    const ro: number = S * 0.5;
    const sw: number = S * 0.13;
    const rm: number = ro - sw / 2;
    const gapHalf: number = 0.733;
    const tilt: number = -0.140;
    c.strokeStyle = this.color;
    c.lineWidth = sw;
    c.lineCap = 'round';
    c.beginPath();
    c.arc(cx, cy, rm, tilt + gapHalf, tilt - gapHalf + 2 * Math.PI);
    c.stroke();

    // ② 圆角播放三角
    //    桌面版 drawPlayMark 的做法：三个顶点先朝重心缩 2*cr，再在每个顶点补一个半径 cr 的圆。
    //    净效果 = 三角整体内缩 cr、三个角是正圆弧。
    //    （直接「填三角 + 描 2*cr 粗边」是往外胖 cr —— 三角会顶到环内侧，和 mac 对不上。）
    const rt: number = S * 0.275;
    const cr: number = S * 0.045;
    const gx: number = cx + S * 0.012;
    const px: number[] = [];
    const py: number[] = [];
    for (let i = 0; i < 3; i++) {
      const a: number = 2 * Math.PI * i / 3.0;
      px.push(gx + rt * Math.cos(a));
      py.push(cy + rt * Math.sin(a));
    }
    const gcx: number = (px[0] + px[1] + px[2]) / 3.0;
    const gcy: number = (py[0] + py[1] + py[2]) / 3.0;
    const qx: number[] = [];
    const qy: number[] = [];
    for (let i = 0; i < 3; i++) {
      const dx: number = gcx - px[i];
      const dy: number = gcy - py[i];
      const L: number = Math.hypot(dx, dy);
      const d: number = 2.0 * cr;
      qx.push(px[i] + dx / L * d);
      qy.push(py[i] + dy / L * d);
    }
    c.fillStyle = this.color;
    c.beginPath();
    c.moveTo(qx[0], qy[0]);
    c.lineTo(qx[1], qy[1]);
    c.lineTo(qx[2], qy[2]);
    c.closePath();
    c.fill();
    for (let i = 0; i < 3; i++) {
      c.beginPath();
      c.arc(qx[i], qy[i], cr, 0, 2 * Math.PI);
      c.fill();
    }
  }
}

// ============================================================================
// 蓝牙图符
// ============================================================================
// 符文用官方 logo 的六点折线描出来，笔画圆头；两道弧以符文中心为圆心，
// 内弧 0.88 半高、外弧 1.42 半高，开口 ±0.55 rad。
// 和桌面不同：这里是静止态（示意图那颗的呼吸动效用 opacity 做，不重画）。
@Component
export struct BluetoothGlyph {
  /// 外半径（= SDL 里的 outerR）
  @Prop radius: number = 18;
  @Prop color: string = C_ICON_BLUE;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);

  build() {
    Canvas(this.ctx)
      .width(this.radius * 2.6)
      .height(this.radius * 2.6)
      .onReady(() => this.draw())
  }

  private draw(): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const box: number = this.radius * 2.6;
    const cx: number = box / 2;
    const cy: number = box / 2;
    c.clearRect(0, 0, box, box);

    const h: number = this.radius * 0.82;                      // 符文半高
    const rod: number = Math.max(1, this.radius * 0.115);      // 笔画粗细
    const sx: number = cx - (1.42 - 0.636) / 2 * h;            // 符文+右弧整体居中
    const sy: number = cy;

    // 六点折线：左下端 → 右上尖 → 竖脊顶 → 竖脊底 → 右下尖 → 左上端
    const path: number[][] = [
      [-0.636, 0.559], [0.636, -0.429], [0.015, -1.000],
      [0.015, 1.000], [0.636, 0.429], [-0.636, -0.559]
    ];
    c.strokeStyle = this.color;
    c.lineWidth = rod;
    c.lineCap = 'round';
    c.lineJoin = 'round';
    c.beginPath();
    c.moveTo(sx + path[0][0] * h, sy + path[0][1] * h);
    for (let i = 1; i < path.length; i++) {
      c.lineTo(sx + path[i][0] * h, sy + path[i][1] * h);
    }
    c.stroke();

    // 两道信号弧
    const span: number = 0.55;
    const radii: number[] = [0.88, 1.42];
    for (let i = 0; i < radii.length; i++) {
      c.beginPath();
      c.arc(sx, sy, radii[i] * h, -span, span);
      c.stroke();
    }
  }
}

// ============================================================================
// 齿轮（设置按钮）
// ============================================================================
@Component
export struct GearIcon {
  @Prop diameter: number = 18;
  @Prop color: string = C_INK;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);

  build() {
    Canvas(this.ctx)
      .width(this.diameter)
      .height(this.diameter)
      .onReady(() => this.draw())
  }

  private draw(): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const S: number = this.diameter;
    const cx: number = S / 2;
    const cy: number = S / 2;
    c.clearRect(0, 0, S, S);
    c.fillStyle = this.color;

    // 8 颗齿：从半径 0.30S 长到 0.46S，宽 0.12S，圆角
    const toothW: number = S * 0.12;
    const toothH: number = S * 0.16;
    const toothY: number = S * 0.30;
    for (let i = 0; i < 8; i++) {
      c.save();
      c.translate(cx, cy);
      c.rotate(i * Math.PI / 4);
      traceRoundRect(c, -toothW / 2, -toothY - toothH, toothW, toothH, toothW * 0.35);
      c.fill();
      c.restore();
    }

    // 齿环：描一条环心线，线宽正好盖住 [rHole, rOuter]，中心自然是空的
    const rOuter: number = S * 0.34;
    const rHole: number = S * 0.15;
    c.beginPath();
    c.arc(cx, cy, (rOuter + rHole) / 2, 0, 2 * Math.PI);
    c.lineWidth = rOuter - rHole;
    c.strokeStyle = this.color;
    c.stroke();
  }
}

// ============================================================================
// 返回箭头
// ============================================================================
@Component
export struct ChevronLeft {
  @Prop diameter: number = 20;
  @Prop color: string = C_INK;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);

  build() {
    Canvas(this.ctx)
      .width(this.diameter)
      .height(this.diameter)
      .onReady(() => this.draw())
  }

  private draw(): void {
    const c: CanvasRenderingContext2D = this.ctx;
    const S: number = this.diameter;
    c.clearRect(0, 0, S, S);
    c.strokeStyle = this.color;
    c.lineWidth = S * 0.12;
    c.lineCap = 'round';
    c.lineJoin = 'round';
    c.beginPath();
    c.moveTo(S * 0.62, S * 0.20);
    c.lineTo(S * 0.32, S * 0.50);
    c.lineTo(S * 0.62, S * 0.80);
    c.stroke();
  }
}
