// ============================================================================
// 鸿蒙 SPP 探针 —— 只为回答一个问题：
//   这台鸿蒙设备能不能连上 iPhone 的 iAP2 服务，并收到它发回来的字节？
// ============================================================================
// 为什么先做这一步：AllPlay 的其它平台（Linux/macOS/Android）都是"连上蓝牙 → 跑 iAP2 →
// 拿 Wi-Fi 配置 → 投屏"。鸿蒙这边链路能不能成立，取决于两件文档保证不了的事：
//   1. sppConnect 能否连到 iPhone 那个自定义 UUID 的 SPP 服务；
//   2. iPhone 认不认这个已经配对的鸿蒙设备，愿不愿意在上面开口说话。
// 这两条只要有一条不成立，后面移植 core/ 的所有工作都是白做。所以先写这二十行。
//
// 判据（看日志）：
//   · 出现 "sppConnect 成功"                     → 第 1 条成立
//   · 收到 FF 5A 开头的数据帧 / 手机自己的 marker → 第 2 条也成立，可以往下做
//   · 一直是 "已发 marker，等回应"               → 服务在，但手机不认（多半是 MFi/配对问题）
// ============================================================================

import { socket, connection } from '@kit.ConnectivityKit';
import { abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

/// iPhone 的 iAP2 SPP 服务 UUID（和 Linux/macOS 端连的是同一个）
const IAP2_UUID: string = '00000000-deca-fade-deca-deafdecacafe';

/// iAP2 链路起始 marker：FF 55 02 00 EE 10。
/// 无线场景里**配件不是发起方**：手机先发 marker，本端再回 SYN。
/// 反过来只做一件事 —— 每秒发一次 marker，等手机开口。
/// （ArkTS 不允许直接拿 Uint8Array.buffer 当 ArrayBuffer 用 —— 结构类型不支持，
///   所以老老实实按字节填一个。）
function markerBytes(): ArrayBuffer {
  const bytes: number[] = [0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10];
  const buf: ArrayBuffer = new ArrayBuffer(bytes.length);
  const view: Uint8Array = new Uint8Array(buf);
  for (let i = 0; i < bytes.length; i++) view[i] = bytes[i];
  return buf;
}

/// 一条日志最多留多少（防止长时间跑把内存吃光）
const MAX_LOGS: number = 200;

function hexOf(buf: ArrayBuffer): string {
  const u8 = new Uint8Array(buf);
  let s: string = '';
  for (let i = 0; i < u8.length; i++) {
    const h: string = u8[i].toString(16);
    s += (h.length < 2 ? '0' + h : h) + ' ';
  }
  return s.trim();
}

function nowStamp(): string {
  const d = new Date();
  const p = (n: number): string => (n < 10 ? '0' + n : n.toString());
  return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

@Entry
@Component
struct Index {
  @State status: string = '未开始';
  @State devices: string[] = [];
  @State logs: string[] = [];
  @State connected: boolean = false;

  private socketId: number = -1;
  private markerTimer: number = -1;
  private gotFrame: boolean = false;

  private log(msg: string): void {
    const line: string = '[' + nowStamp() + '] ' + msg;
    const next: string[] = this.logs.slice();
    next.push(line);
    while (next.length > MAX_LOGS) next.shift();
    this.logs = next;
    console.info('[probe] ' + msg);
  }

  private async ensurePermission(): Promise<boolean> {
    const at = abilityAccessCtrl.createAtManager();
    const perms: Permissions[] = ['ohos.permission.ACCESS_BLUETOOTH'];
    try {
      const r = await at.requestPermissionsFromUser(getContext(this), perms);
      const granted: boolean = r.authResults.length > 0 && r.authResults[0] === 0;
      this.log(granted ? '蓝牙权限：已授权' : '蓝牙权限：被拒绝（authResults=' +
        JSON.stringify(r.authResults) + '）');
      return granted;
    } catch (e) {
      const err = e as BusinessError;
      this.log('申请蓝牙权限失败：' + err.code + ' ' + err.message);
      return false;
    }
  }

  /// 读一遍已配对设备。探针的前提：iPhone 已经在这台设备的蓝牙设置里配好。
  private refreshPaired(): void {
    try {
      const addrs: string[] = connection.getPairedDevices();
      this.devices = [];
      for (let i = 0; i < addrs.length; i++) {
        const a: string = addrs[i];
        let name: string = '';
        try {
          name = connection.getRemoteDeviceName(a);
        } catch (e) {
          name = '?';
        }
        this.devices.push(name + '  ' + a);
      }
      this.log('已配对设备 ' + addrs.length + ' 个');
    } catch (e) {
      const err = e as BusinessError;
      this.log('读已配对设备失败：' + err.code + ' ' + err.message);
      this.status = '读已配对设备失败';
    }
  }

  private connect(deviceId: string): void {
    if (this.connected) {
      this.log('已经连着 ' + this.socketId + '，先断开再连');
      return;
    }
    const opt: socket.SppOptions = {
      uuid: IAP2_UUID,
      // iAP2 要加密链路；如果这一步报 2900004（Profile not supported）或一直连不上，
      // 下一个要试的就是把 secure 改成 false。
      secure: true,
      type: socket.SppType.SPP_RFCOMM
    };
    this.log('sppConnect → ' + deviceId + '（uuid=' + IAP2_UUID + '）…');
    this.status = '连接中…';
    try {
      socket.sppConnect(deviceId, opt, (err: BusinessError, id: number) => {
        if (err) {
          this.log('sppConnect 失败：' + err.code + ' ' + err.message);
          this.status = '连接失败（' + err.code + '）';
          return;
        }
        this.socketId = id;
        this.connected = true;
        this.gotFrame = false;
        this.status = '已连接 socket=' + id + '，等手机开口';
        this.log('sppConnect 成功，clientSocket=' + id);
        this.watchRead(id);
        this.startMarker(id);
      });
    } catch (e) {
      const err = e as BusinessError;
      this.log('sppConnect 抛异常：' + err.code + ' ' + err.message);
      this.status = '连接失败';
    }
  }

  private watchRead(id: number): void {
    socket.on('sppRead', id, (data: ArrayBuffer) => {
      if (!data || data.byteLength === 0) return;
      const hex = hexOf(data);
      this.log('收到 ' + data.byteLength + ' 字节：' + hex.slice(0, 160) +
        (hex.length > 160 ? ' …' : ''));
      // FF 5A 是 iAP2 数据帧的头；手机主动发的 marker 是 FF 55 02 00 EE 10。
      if (!this.gotFrame && hex.indexOf('ff 5a') >= 0) {
        this.gotFrame = true;
        this.status = '✅ 收到 iAP2 数据帧 —— 蓝牙这条路成立';
        this.log('★ 看到 FF 5A 数据帧：手机认了这个配件');
      } else if (hex.indexOf('ff 55 02 00 ee 10') >= 0) {
        this.log('★ 收到手机自己的 marker');
        if (!this.gotFrame) this.status = '收到手机 marker —— 链路已通';
      }
    });
  }

  private startMarker(id: number): void {
    this.stopMarker();
    const tick = (): void => {
      try {
        socket.sppWrite(id, markerBytes());
        this.log('已发 marker（FF 55 02 00 EE 10）');
      } catch (e) {
        const err = e as BusinessError;
        this.log('写 marker 失败：' + err.code + ' ' + err.message);
        this.stopMarker();
      }
    };
    tick();
    this.markerTimer = setInterval(tick, 1000);
  }

  private stopMarker(): void {
    if (this.markerTimer !== -1) {
      clearInterval(this.markerTimer);
      this.markerTimer = -1;
    }
  }

  private disconnect(): void {
    this.stopMarker();
    if (!this.connected) return;
    try {
      socket.sppCloseClientSocket(this.socketId);
      this.log('已断开 socket=' + this.socketId);
    } catch (e) {
      const err = e as BusinessError;
      this.log('断开失败：' + err.code + ' ' + err.message);
    }
    this.connected = false;
    this.socketId = -1;
    this.status = '已断开';
  }

  aboutToDisappear(): void {
    this.disconnect();
  }

  build() {
    Column({ space: 12 }) {
      Text('AllPlay · 鸿蒙 SPP 探针')
        .fontSize(22)
        .fontWeight(FontWeight.Bold)
      Text('先确认这台设备能连上 iPhone 的 iAP2 服务，再谈移植 CarPlay。')
        .fontSize(13)
        .fontColor('#666666')

      Text(this.status)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
        .width('100%')
        .padding(10)
        .backgroundColor('#F2F2F2')
        .borderRadius(8)

      Row({ space: 8 }) {
        Button('申请权限 + 读已配对设备')
          .fontSize(14)
          .onClick(async () => {
            const ok: boolean = await this.ensurePermission();
            if (ok) this.refreshPaired();
          })
        Button('断开')
          .fontSize(14)
          .enabled(this.connected)
          .onClick(() => this.disconnect())
      }
      .width('100%')

      if (this.devices.length === 0) {
        Text('没有已配对设备。先到系统「设置 → 蓝牙」里和 iPhone 配对，再回来点上面的按钮。')
          .fontSize(13)
          .fontColor('#999999')
      } else {
        Text('点一台设备开始连接（要选 iPhone；配错的设备会一直连不上）')
          .fontSize(13)
          .fontColor('#666666')
        ForEach(this.devices, (d: string) => {
          Button(d)
            .fontSize(14)
            .width('100%')
            .enabled(!this.connected)
            .onClick(() => {
              // "名字  AA:BB:.." —— 地址固定在末尾 17 个字符
              const addr: string = d.slice(d.length - 17);
              this.connect(addr);
            })
        }, (d: string) => d)
      }

      Text('日志').fontSize(14).fontWeight(FontWeight.Medium).width('100%')
      List({ space: 2 }) {
        ForEach(this.logs, (l: string) => {
          ListItem() {
            Text(l).fontSize(11).fontColor('#333333').width('100%')
          }
        }, (l: string, i: number) => i.toString() + l)
      }
      .layoutWeight(1)
      .width('100%')
      .padding(8)
      .backgroundColor('#FAFAFA')
      .borderRadius(8)
    }
    .padding(16)
    .width('100%')
    .height('100%')
  }
}
