/**
 * AllPlay · 鸿蒙启动页（连接引导）
 *
 * 版式照搬桌面 host/launcher.cpp（1540×850 设计稿）：
 *   顶栏：品牌标 + AllPlay + 右上角胶囊（桌面是语言开关，这里换成「设置」）
 *   居中：大标题 + 副标题
 *   两栏：左 = 设备蓝牙名称卡片 + 配对提示；右 = 手机→蓝牙→中控屏 示意图
 *          （宽屏并排，手机竖屏改上下排）
 *   底部：取消按钮
 *   页脚：状态点 + 状态文字（左）· AllPlay · CarPlay（右）
 *
 * 功能仍是"把 iPhone 连成 CarPlay 车机"：
 *   ArkTS 管蓝牙（sppConnect/sppRead/sppWrite），core/iap2 在原生线程跑协议
 *   （marker→SYN→ACK、MFi、识别、订阅、0x5703、0x4301）。
 *   SSID / 密码在设置页填（右上角），存本地，不进仓库。
 *
 * 诊断：状态栏文字点一下展开/收起日志 —— 版面上看不出来，调试时有用。
 */

import { socket, connection } from '@kit.ConnectivityKit';
import { abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { router, window, display } from '@kit.ArkUI';
import { mdns } from '@kit.NetworkKit';
import { start as iap2Start, feed as iap2Feed, stop as iap2Stop,
         isRunning as iap2Running, TriggerConfig,
         startAirplayServer, stopAirplayServer, airplayServerPort, setVideoSurface, ReceiverConfig,
         airplayIdentity, AirplayIdentity, probeCarPlay, hotspotIpv4,
         startCarPlayCtrlWatch, stopCarPlayCtrlWatch,
         sendTouch, TouchPoint } from 'liballplay_core.so';

import { PlayMark, BluetoothGlyph, GearIcon } from '../components/Glyphs';
import { ConnectVisual } from '../components/ConnectVisual';
import { WifiConfig, loadWifiConfig, securityTypeLabel } from '../common/SettingsStore';
import { localIpv4, linkedSsid, apIpv4, hotspotActive, netSummary } from '../common/NetInfo';
import {
  C_BLUE, C_CARD_LINE, C_EYEBROW, C_FOOTER, C_GREEN, C_HINT, C_INK,
  C_LANG_LINE, C_MUTED, C_RED, C_RULE, C_TILE_BG, C_VISUAL_BG, C_VISUAL_LINE, C_WHITE
} from '../common/Theme';

/// iPhone 的 iAP2 SPP 服务 UUID（和 Linux/macOS/Android 端连的是同一个）
const IAP2_UUID: string = '00000000-deca-fade-deca-deafdecacafe';

/// 一条日志最多留多少（防止长时间跑把内存吃光）
const MAX_LOGS: number = 200;

// ---- Bonjour TXT（照 core/airplay/server.cpp 的 Receiver::start 抄）----
// features 必须与 core/airplay/info.cpp 的 DeviceInfo::features 默认值一致：
// 0x615653AEE2 → TXT 写作 "0x5653aee2,0x61"（低 32 位,高 32 位）。
const AP_FEATURES: string = '0x5653aee2,0x61';
const AP_SRCVERS: string = '950.7.1';
const AP_PROTOVERS: string = '1.1';
const AP_MODEL: string = 'AllPlay1,1';

/// TXT 的 value 是原始字节，不是字符串（ASCII 就够，TXT 值全是可见字符）
function asciiBytes(s: string): number[] {
  const out: number[] = [];
  for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i) & 0xff);
  return out;
}

/// 链路状态（和桌面 launcher 的 LinkState 一一对应）
const ST_IDLE: number = 0;
const ST_CONNECTING: number = 1;
const ST_STREAMING: number = 2;
const ST_ERROR: number = 3;

function nowStamp(): string {
  const d = new Date();
  const h: string = d.getHours() < 10 ? '0' + d.getHours() : d.getHours().toString();
  const m: string = d.getMinutes() < 10 ? '0' + d.getMinutes() : d.getMinutes().toString();
  const s: string = d.getSeconds() < 10 ? '0' + d.getSeconds() : d.getSeconds().toString();
  return h + ':' + m + ':' + s;
}

/// Uint8Array → 独立 ArrayBuffer（拷一份，保证只含有效字节）
function toArrayBuffer(u8: Uint8Array): ArrayBuffer {
  const buf: ArrayBuffer = new ArrayBuffer(u8.length);
  const dst: Uint8Array = new Uint8Array(buf);
  dst.set(u8);
  return buf;
}

@Entry
@Component
struct Index {
  @State linkState: number = ST_IDLE;
  /// CarPlay 画面（XComponent）的控制器：拿 surface id 交给原生解码器
  private xc: XComponentController = new XComponentController();
  /// 向手机协商的显示分辨率（横屏）。手机按这个尺寸排版、编码、也给触摸坐标定标；
  /// XComponent 按 videoW:videoH 摆，横屏后正好铺满。
  @State videoW: number = 1280;
  @State videoH: number = 720;
  /// XComponent 实际占的尺寸（vp）。触摸坐标 t.x/t.y 就是相对它的 —— 用它归一化。
  private compW: number = 0;
  private compH: number = 0;
  /// 手指 id → HID 槽位（0/1）。同一根手指必须一直落在同一槽，否则手机会当成两根手指数打架。
  private touchSlots: Map<number, number> = new Map<number, number>();
  private touchWarned: boolean = false;
  private landscape: boolean = false;
  @State dotTick: number = 0;
  @State devices: string[] = [];
  @State selectedName: string = '未选择设备';
  @State selectedAddr: string = '';
  @State showPicker: boolean = false;
  @State logs: string[] = [];
  @State showLogs: boolean = false;
  @State ssid: string = '';
  /// iAP2 的 securityType：0 开放 / 1 WEP / 2 WPA2 / 3 WPA3（设置页里选）
  @State securityType: number = 2;
  @State passphrase: string = '';
  @State localIp: string = '';
  /// 宽屏（平板 / 横屏）：两栏并排；手机竖屏改为上下排
  @State wide: boolean = false;

  private socketId: number = -1;
  private connected: boolean = false;
  private nativeRunning: boolean = false;
  private dotTimer: number = -1;
  private lastCoreLine: string = '';
  private mdnsOn: boolean = false;
  private airplayDeviceId: string = '';
  private ctrlDiscovery: mdns.DiscoveryService | undefined = undefined;

  // -------------------------------------------------------------------------
  // 生命周期
  // -------------------------------------------------------------------------
  aboutToAppear(): void {
    this.dotTimer = setInterval(() => {
      this.dotTick = (this.dotTick + 1) % 4;
    }, 400);
    this.refreshLocalIp();
    this.reloadSettings();
    this.startProbe();
    this.startMdns();
    this.startCtrlWatch();
    this.startNativeCtrlWatch();
  }

  aboutToDisappear(): void {
    if (this.dotTimer !== -1) {
      clearInterval(this.dotTimer);
      this.dotTimer = -1;
    }
    stopCarPlayCtrlWatch();
    this.stopCtrlWatch();
    this.stopMdns();
    this.stopProbe();
    this.disconnect();
  }

  // -------------------------------------------------------------------------
  // Bonjour：广播 _airplay._tcp
  // -------------------------------------------------------------------------
  // 手机加入 Wi-Fi 后要在网络上"看见"这台车机。0x4301 只告诉它 IP/端口，
  // 真机实测还要有这条 Bonjour 记录（和桌面宿主用 dns_sd 广播的是同一份 TXT），
  // 否则它连 SYN 都不发 —— 表现就是"蓝牙握手全绿，画面永远不来"。
  private startMdns(): void {
    try {
      const ctx = getContext(this);
      const id: AirplayIdentity = airplayIdentity(ctx.filesDir + '/allplay-state');
      if (id.deviceId.length === 0 || id.pk.length === 0) {
        this.log('mDNS 未广播：拿不到 AirPlay 身份');
        return;
      }
      const attrs: mdns.ServiceAttribute[] = [
        { key: 'deviceid', value: asciiBytes(id.deviceId) },
        { key: 'features', value: asciiBytes(AP_FEATURES) },
        { key: 'flags', value: asciiBytes('0x4') },
        { key: 'model', value: asciiBytes(AP_MODEL) },
        { key: 'srcvers', value: asciiBytes(AP_SRCVERS) },
        { key: 'protovers', value: asciiBytes(AP_PROTOVERS) },
        { key: 'pi', value: asciiBytes(id.pi) },
        { key: 'pk', value: asciiBytes(id.pk) },
      ];
      this.airplayDeviceId = id.deviceId;
      const info: mdns.LocalServiceInfo = {
        serviceType: '_airplay._tcp',
        serviceName: 'AllPlay',
        port: 7000,
        serviceAttribute: attrs
      };
      mdns.addLocalService(ctx, info).then(() => {
        this.mdnsOn = true;
        this.log('mDNS：已广播 _airplay._tcp（deviceid=' + id.deviceId + '，端口 7000）');
      }).catch((e: BusinessError) => {
        this.log('mDNS 广播失败：' + e.code + ' ' + e.message);
      });
      // 车机还要以 _carplay._tcp 的身份出现（Android 端两个都广播，真机才认）
      const carplay: mdns.LocalServiceInfo = {
        serviceType: '_carplay._tcp',
        serviceName: 'AllPlay',
        port: 7000,
        serviceAttribute: attrs
      };
      mdns.addLocalService(ctx, carplay).then(() => {
        this.log('mDNS：已广播 _carplay._tcp');
      }).catch((e: BusinessError) => {
        this.log('mDNS 广播 _carplay._tcp 失败：' + e.code + ' ' + e.message);
      });
    } catch (e) {
      const err = e as BusinessError;
      this.log('mDNS 启动异常：' + err.code + ' ' + err.message);
    }
  }

  private stopMdns(): void {
    if (!this.mdnsOn) return;
    this.mdnsOn = false;
    try {
      const ctx = getContext(this);
      const info: mdns.LocalServiceInfo = {
        serviceType: '_airplay._tcp',
        serviceName: 'AllPlay',
        port: 7000
      };
      mdns.removeLocalService(ctx, info).catch((e: BusinessError) => {
        console.info('[allplay] mDNS 撤广播失败：' + e.code + ' ' + e.message);
      });
    } catch (e) {
      const err = e as BusinessError;
      console.info('[allplay] mDNS 撤广播异常：' + err.code + ' ' + err.message);
    }
  }

  // -------------------------------------------------------------------------
  // Bonjour 发现：找到手机的 _carplay-ctrl 并"戳一下"
  // -------------------------------------------------------------------------
  // 手机上有个 _carplay-ctrl._tcp 服务。给它发 GET /ctrl-int/1/connect
  // （带 AirPlay-Receiver-Device-ID）之后，手机才会主动连我们的 RTSP 端口。
  // 只发 0x4301、只广播 _airplay._tcp，手机是不会动的（Android 端同样靠这一枪）。
  private startCtrlWatch(): void {
    try {
      const ctx = getContext(this);
      const ds: mdns.DiscoveryService = mdns.createDiscoveryService(ctx, '_carplay-ctrl._tcp');
      this.ctrlDiscovery = ds;
      ds.on('serviceFound', (found: mdns.LocalServiceInfo) => {
        this.log('mDNS：发现手机控制服务 ' + found.serviceName + '，开始解析');
        mdns.resolveLocalService(ctx, found).then((r: mdns.LocalServiceInfo) => {
          const host: string = r.host !== undefined ? r.host.address : '';
          const port: number = r.port !== undefined ? r.port : 0;
          if (host.length === 0 || port === 0) {
            this.log('mDNS：_carplay-ctrl 解析不到地址/端口');
            return;
          }
          this.log('mDNS：手机控制服务 ' + host + ':' + port + '，发 /ctrl-int/1/connect');
          if (!probeCarPlay(host, port, this.airplayDeviceId)) {
            this.log('carplay 探测没发出去（' + host + ':' + port + ' 连不上）');
          }
        }).catch((e: BusinessError) => {
          this.log('mDNS：解析 _carplay-ctrl 失败 ' + e.code + ' ' + e.message);
        });
      });
      ds.startSearchingMDNS();
      this.log('mDNS：开始找手机的 _carplay-ctrl 服务');
    } catch (e) {
      const err = e as BusinessError;
      this.log('mDNS 发现启动失败：' + err.code + ' ' + err.message);
    }
  }

  private stopCtrlWatch(): void {
    const ds: mdns.DiscoveryService | undefined = this.ctrlDiscovery;
    if (ds === undefined) return;
    this.ctrlDiscovery = undefined;
    try {
      ds.stopSearchingMDNS();
      ds.off('serviceFound');
    } catch (e) {
      const err = e as BusinessError;
      console.info('[allplay] 停止 mDNS 发现失败：' + err.code + ' ' + err.message);
    }
  }

  /// 系统 mDNS 只能走默认网络（`@ohos.net.mdns` 没有网卡参数）：热点和家里
  /// Wi-Fi/蜂窝同时开着时，默认网络不是热点，`_carplay-ctrl` 永远发现不到。
  /// 这里让原生层直接绑热点网卡再找一次。
  private startNativeCtrlWatch(): void {
    const ap: string = this.localIp;
    if (ap.length === 0) {
      this.log('原生 mDNS：本机没有热点地址，跳过');
      return;
    }
    if (this.airplayDeviceId.length === 0) {
      this.log('原生 mDNS：还没有 AirPlay 身份，跳过');
      return;
    }
    const ok: boolean = startCarPlayCtrlWatch(ap, this.airplayDeviceId);
    this.log('原生 mDNS：在 ' + ap + ' 上找手机的 _carplay-ctrl（' +
             (ok ? '已启动' : '启动失败') + '）');
  }

  /// 0x4301 要报的本机地址：热点开着就报热点网卡的地址，否则报默认网络地址
  private refreshLocalIp(): void {
    this.localIp = localIpv4();
    // 热点网卡在 isHotspotActive() 不可用时也要能认出来：只要有名叫 wlan1/softap 的
    // 网卡带 IPv4，它就是手机的落点（鸿蒙热点实测是 wlan1 = 192.168.43.1）。
    // 先问 ArkTS 的网络列表（列得出 wlan1 的设备），再问内核（鸿蒙的 NetManager
    // 不列 AP 网卡，只有 getifaddrs 看得到 192.168.43.1）
    let ap: string = apIpv4();
    if (ap.length === 0) {
      try { ap = hotspotIpv4(); } catch (e) { ap = ''; }
    }
    if (ap.length > 0) this.localIp = ap;
    this.log('本机网络：' + netSummary() +
             ' → 0x4301 用 ' + (this.localIp.length > 0 ? this.localIp : '（没地址）') +
             (ap.length > 0 ? '（热点网卡）' : ''));
  }

  /// 核对"设置里的 SSID"和"本机实际连的 Wi-Fi"——不一致就一定进不了 CarPlay
  private checkNetwork(): void {
    if (apIpv4().length > 0 || hotspotActive()) {
      this.log('本机热点开着：0x4301 报 ' +
               (this.localIp.length > 0 ? this.localIp : '（没有热点地址！）') +
               '，手机应当加入热点「' + (this.ssid.length > 0 ? this.ssid : '未设置') + '」');
      return;
    }
    const real: string = linkedSsid();
    if (real.length === 0) {
      this.log('本机 Wi-Fi 名读不到（未连 Wi-Fi 或没有 GET_WIFI_INFO 权限）');
      return;
    }
    if (this.ssid.length === 0) {
      this.log('本机 Wi-Fi：' + real + '（设置页还没填 SSID）');
      return;
    }
    if (real !== this.ssid) {
      this.log('⚠ 本机连的是「' + real + '」，设置里填的是「' + this.ssid +
               '」——手机会去连后者，找不到本机。请把车机 Wi-Fi 改成同一个网络。');
    } else {
      this.log('本机 Wi-Fi：' + real + '（与设置一致，加密方式 ' +
               securityTypeLabel(this.securityType) + '）');
    }
  }

  /// AirPlay 接收端：手机拿到 0x4301 之后 1 秒内就来连这个端口。
  /// 这里答 /info、/pair-setup、/pair-verify、/auth-setup、SETUP —— 真的 RTSP 服务器
  /// （core/airplay），不再是那个只收不答的探针。
  private startProbe(): void {
    try {
      const geo: number[] = this.landscapeGeometry();
      this.videoW = geo[0];
      this.videoH = geo[1];
      const cfg: ReceiverConfig = {
        name: 'AllPlay',
        port: 7000,
        stateDir: getContext(this).filesDir + '/allplay-state',
        btMac: '00:00:00:00:00:00',
        width: this.videoW,
        height: this.videoH
      };
      this.loadMfiInto(cfg);
      const ok: boolean = startAirplayServer(cfg, (line: string) => {
        this.log('[airplay] ' + line);
        if (line.indexOf('连接进入') >= 0) {
          this.lastCoreLine = line;
          this.linkState = ST_CONNECTING;
          this.setOrientation(true);   // 手机来连就转横屏：视频 SETUP 一到就有 surface
        } else if (line.indexOf('RECORD') >= 0 || line.indexOf('会话建立') >= 0) {
          this.lastCoreLine = line;
          this.linkState = ST_STREAMING;
          this.setOrientation(true);
        }
      });
      if (ok) {
        this.log('AirPlay 接收端：监听 ' + airplayServerPort() + '，本机 ' +
                 (this.localIp.length > 0 ? this.localIp : '未联网'));
      } else {
        this.log('AirPlay 接收端启动失败（见上面的 [core] 日志）');
      }
    } catch (e) {
      const err = e as BusinessError;
      this.log('启动 AirPlay 接收端失败：' + err.code + ' ' + err.message);
    }
  }

  private stopProbe(): void {
    try {
      stopAirplayServer();
    } catch (e) {
      const err = e as BusinessError;
      this.log('停止 AirPlay 接收端失败：' + err.code + ' ' + err.message);
    }
  }

  /// 协商给手机的显示分辨率：[宽, 高]（横屏，偶数）。
  /// 用**屏幕自己的长宽比**而不是固定 16:9 —— 手机按这个尺寸排 CarPlay 界面，
  /// 比例一致才能横屏铺满（16:9 在 20:9 的屏幕上必然上下/左右留黑边）。
  /// 宽度封在 1920：再高 iPhone 编码和本机硬解都更吃力，画质收益却很小。
  private landscapeGeometry(): number[] {
    let w: number = 2688;
    let h: number = 1216;
    try {
      const d = display.getDefaultDisplaySync();
      w = Math.max(d.width, d.height);
      h = Math.min(d.width, d.height);
    } catch (e) {
      const err = e as BusinessError;
      this.log('读屏幕尺寸失败（按 2688x1216 算）：' + err.code);
    }
    if (w <= 0 || h <= 0) { w = 2688; h = 1216; }
    const outW: number = Math.min(1920, w);
    let outH: number = Math.round(outW * h / w / 2) * 2;
    if (outH < 480) outH = 480;
    return [outW, outH];
  }

  /// CarPlay 是横屏画面：会话进来就转横屏，断开转回竖屏（启动页是竖屏设计的）。
  private setOrientation(land: boolean): void {
    if (this.landscape === land) return;
    this.landscape = land;
    window.getLastWindow(getContext(this)).then((win: window.Window) => {
      win.setPreferredOrientation(
        land ? window.Orientation.LANDSCAPE : window.Orientation.PORTRAIT);
    }).catch((e: BusinessError) => {
      this.log('切换屏幕方向失败：' + e.code + ' ' + e.message);
    });
  }

  /// 从设置页回来时刷新一次（SSID / 密码可能刚改过）
  onPageShow(): void {
    this.refreshLocalIp();
    this.reloadSettings();
  }

  private async reloadSettings(): Promise<void> {
    const cfg: WifiConfig = await loadWifiConfig(getContext(this));
    this.ssid = cfg.ssid;
    this.passphrase = cfg.passphrase;
    this.securityType = cfg.securityType;
    this.checkNetwork();
  }

  // -------------------------------------------------------------------------
  // 日志
  // -------------------------------------------------------------------------
  private log(msg: string): void {
    const line: string = '[' + nowStamp() + '] ' + msg;
    const next: string[] = this.logs.slice();
    next.push(line);
    while (next.length > MAX_LOGS) next.shift();
    this.logs = next;
    console.info('[allplay] ' + msg);
  }

  private coreLog(msg: string): void {
    this.lastCoreLine = msg;
    this.log('[core] ' + msg);
    if (msg.indexOf('认证通过') >= 0) {
      this.linkState = ST_STREAMING;
    } else if (msg.indexOf('0xAA04') >= 0) {
      this.linkState = ST_ERROR;
    }
  }

  // -------------------------------------------------------------------------
  // 蓝牙
  // -------------------------------------------------------------------------
  private async ensurePermission(): Promise<boolean> {
    const at = abilityAccessCtrl.createAtManager();
    const perms: Permissions[] = ['ohos.permission.ACCESS_BLUETOOTH'];
    try {
      const r = await at.requestPermissionsFromUser(getContext(this), perms);
      const granted: boolean = r.authResults.length > 0 && r.authResults[0] === 0;
      this.log(granted ? '蓝牙权限：已授权' : '蓝牙权限：被拒绝');
      return granted;
    } catch (e) {
      const err = e as BusinessError;
      this.log('申请蓝牙权限失败：' + err.code + ' ' + err.message);
      return false;
    }
  }

  /// 读一遍已配对设备（iPhone 必须先在系统蓝牙里配好）
  private refreshPaired(): void {
    try {
      const addrs: string[] = connection.getPairedDevices();
      const list: string[] = [];
      for (let i = 0; i < addrs.length; i++) {
        const a: string = addrs[i];
        let name: string = '';
        try {
          name = connection.getRemoteDeviceName(a);
        } catch (e) {
          name = '未知设备';
        }
        list.push(name + '  ' + a);
      }
      this.devices = list;
      this.log('已配对设备 ' + list.length + ' 个');
    } catch (e) {
      const err = e as BusinessError;
      this.log('读已配对设备失败：' + err.code + ' ' + err.message);
      this.linkState = ST_ERROR;
    }
  }

  private async openPicker(): Promise<void> {
    if (this.connected) {
      this.log('已连接 ' + this.selectedName + '，先断开再换设备');
      return;
    }
    const ok: boolean = await this.ensurePermission();
    if (ok) this.refreshPaired();
    this.showPicker = true;
  }

  private connect(deviceId: string, name: string): void {
    if (this.connected) return;
    this.selectedName = name;
    this.selectedAddr = deviceId;
    this.linkState = ST_CONNECTING;

    const opt: socket.SppOptions = {
      uuid: IAP2_UUID,
      secure: true,
      type: socket.SppType.SPP_RFCOMM
    };
    this.log('sppConnect → ' + deviceId);
    try {
      socket.sppConnect(deviceId, opt, (err: BusinessError, id: number) => {
        if (err) {
          this.log('sppConnect 失败：' + err.code + ' ' + err.message);
          this.linkState = ST_ERROR;
          return;
        }
        this.socketId = id;
        this.connected = true;
        this.linkState = ST_STREAMING;
        this.log('已连接 socket=' + id);
        this.watchRead(id);
        this.startNative(id);
      });
    } catch (e) {
      const err = e as BusinessError;
      this.log('sppConnect 抛异常：' + err.code + ' ' + err.message);
      this.linkState = ST_ERROR;
    }
  }

  private watchRead(id: number): void {
    socket.on('sppRead', id, (data: ArrayBuffer) => {
      if (!data || data.byteLength === 0) return;
      // 收到的字节全部交给 core 的链路层，由它决定回 SYN / ACK / 识别报文
      if (this.nativeRunning) iap2Feed(data);
    });
  }

  private disconnect(): void {
    this.touchSlots.clear();
    this.setOrientation(false);
    if (this.nativeRunning || iap2Running()) {
      try {
        iap2Stop();
      } catch (e) {
        const err = e as BusinessError;
        this.log('停止原生触发失败：' + err.code + ' ' + err.message);
      }
      this.nativeRunning = false;
    }
    if (!this.connected) {
      this.linkState = ST_IDLE;
      return;
    }
    try {
      socket.sppCloseClientSocket(this.socketId);
      this.log('已断开 socket=' + this.socketId);
    } catch (e) {
      const err = e as BusinessError;
      this.log('断开失败：' + err.code + ' ' + err.message);
    }
    this.connected = false;
    this.socketId = -1;
    this.linkState = ST_IDLE;
  }

  // -------------------------------------------------------------------------
  // 原生 iAP2
  // -------------------------------------------------------------------------
  private startNative(id: number): void {
    if (iap2Running()) {
      this.log('原生触发已经在跑，忽略重复启动');
      return;
    }
    const cfg: TriggerConfig = {
      name: 'AllPlay',
      model: 'AllPlay1,1',
      manufacturer: 'AllPlay',
      serial: '000000000001',
      firmware: '1.0',
      hardware: '1.0',
      // 鸿蒙不开放本机蓝牙地址；识别报文 group 17 要 6 字节，用全 0 占位。
      // 实测：group 17 **必须发**——删掉它手机会回 0x1D03（识别被拒，
      // body 里的参数 id 正是 0x0011=17）；发全 0 则会 0x1D02 通过。
      btMac: '00:00:00:00:00:00',
      ssid: this.ssid,
      passphrase: this.passphrase,
      securityType: this.securityType,
      airPlayPort: 7000,
      mfiDir: '',
      timeoutMs: 45000,
      // AirPlay 配对身份落盘在应用沙箱里 —— 0x4301 的 publicKey 由它生成
      stateDir: getContext(this).filesDir + '/allplay-state'
    };
    if (this.localIp.length > 0) cfg.ip = this.localIp;
    this.loadMfiInto(cfg);

    if (this.ssid.length === 0) {
      this.log('还没设置 Wi-Fi（右上角「设置」）——0x5703 会发空的网络配置');
    } else if (this.securityType !== 0 && this.passphrase.length === 0) {
      this.log('⚠ 「' + this.ssid + '」是' + securityTypeLabel(this.securityType) +
               '网络，但设置里没填密码 —— 手机连不上');
    }

    this.nativeRunning = true;
    this.log('原生 iAP2 触发启动');
    iap2Start(cfg, (data: ArrayBuffer) => {
      // core 线程给的字节，必须在 ArkTS 主线程写进 socket
      try {
        socket.sppWrite(id, data);
      } catch (e) {
        const err = e as BusinessError;
        this.log('sppWrite 失败：' + err.code + ' ' + err.message);
      }
    }, (line: string) => {
      this.coreLog(line);
    });
  }

  /// 从 HAP 的 rawfile 里读 MFi 凭证（构建前用 scripts/sync-harmony-identity.sh 拷进去）。
  private loadMfiInto(cfg: TriggerConfig | ReceiverConfig): void {
    try {
      const rm = getContext(this).resourceManager;
      const key: Uint8Array = rm.getRawFileContentSync('identity/identity.pk8');
      const cert: Uint8Array = rm.getRawFileContentSync('identity/certificate.p7b');
      cfg.mfiKey = toArrayBuffer(key);
      cfg.mfiCert = toArrayBuffer(cert);
      this.log('MFi 身份已读入：私钥 ' + key.length + ' 字节，证书 ' + cert.length + ' 字节');
    } catch (e) {
      const err = e as BusinessError;
      this.log('HAP rawfile 里没有 identity/：' + err.code + ' ' + err.message);
    }
  }

  // -------------------------------------------------------------------------
  // 文案
  // -------------------------------------------------------------------------
  private dots(): string {
    let s: string = '';
    for (let i = 0; i < this.dotTick; i++) s += '.';
    return s;
  }

  private footerText(): string {
    switch (this.linkState) {
      case ST_CONNECTING:
        return '正在连接' + this.dots();
      case ST_STREAMING:
        return this.lastCoreLine.length > 0 ? this.lastCoreLine : '已连接';
      case ST_ERROR:
        return '连接失败，请重试';
      default:
        return '等待设备连接' + this.dots();
    }
  }

  private dotColor(): string {
    if (this.linkState === ST_CONNECTING) return C_BLUE;
    if (this.linkState === ST_ERROR) return C_RED;
    return C_GREEN;
  }

  // -------------------------------------------------------------------------
  // 界面片段
  // -------------------------------------------------------------------------
  @Builder
  header() {
    Row() {
      PlayMark({ diameter: 26, color: C_INK })
      Text('AllPlay')
        .fontSize(20)
        .fontWeight(FontWeight.Bold)
        .fontColor(C_INK)
        .margin({ left: 9 })
      Blank()
      // 桌面这里是语言胶囊（EN / 中文）；鸿蒙换成设置入口
      Row() {
        GearIcon({ diameter: 15, color: C_INK })
        Text('设置')
          .fontSize(13)
          .fontColor(C_INK)
          .margin({ left: 5 })
      }
      .height(32)
      .padding({ left: 12, right: 12 })
      .borderRadius(16)
      .border({ width: 1, color: C_LANG_LINE })
      .backgroundColor(C_WHITE)
      .onClick(() => {
        router.pushUrl({ url: 'pages/Settings' });
      })
    }
    .width('100%')
    .height(60)
    .padding({ left: 16, right: 16 })
  }

  @Builder
  deviceCard() {
    Column() {
      Text('设备蓝牙名称')
        .fontSize(12)
        .fontWeight(FontWeight.Bold)
        .fontColor(C_EYEBROW)
        .width('100%')

      Row() {
        Stack() {
          BluetoothGlyph({ radius: 14, color: '#1E9BE0' })
        }
        .width(40)
        .height(40)
        .backgroundColor(C_TILE_BG)
        .borderRadius(11)

        Column() {
          Text(this.selectedName)
            .fontSize(16)
            .fontColor(C_INK)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          if (this.selectedAddr.length > 0) {
            Text(this.selectedAddr)
              .fontSize(11)
              .fontColor(C_HINT)
              .margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        .margin({ left: 14 })

        Text(this.connected ? '已连接' : '选择')
          .fontSize(12)
          .fontColor(C_BLUE)
      }
      .width('100%')
      .height(68)
      .padding({ left: 12, right: 14 })
      .margin({ top: 10 })
      .backgroundColor(C_WHITE)
      .borderRadius(14)
      .border({ width: 1, color: C_CARD_LINE })
      .shadow({ radius: 8, color: 'rgba(26,38,58,0.06)', offsetY: 3 })
      .onClick(() => {
        this.openPicker();
      })

      Text('请在手机蓝牙列表中选择此设备进行配对，并确保手机 Wi-Fi 已开启。')
        .fontSize(12)
        .fontColor(C_HINT)
        .lineHeight(19)
        .width('100%')
        .margin({ top: 12 })
    }
    .width('100%')
  }

  @Builder
  visualPanel() {
    Column() {
      ConnectVisual({ heightVp: this.wide ? 250 : 180 })
    }
    .width('100%')
    .borderRadius(16)
    .backgroundColor(C_VISUAL_BG)
    .border({ width: 1, color: C_VISUAL_LINE })
    .clip(true)
  }

  @Builder
  logPanel() {
    Column() {
      Text('诊断日志')
        .fontSize(12)
        .fontWeight(FontWeight.Bold)
        .fontColor(C_EYEBROW)
        .width('100%')
        .margin({ bottom: 6 })
      List({ space: 2 }) {
        ForEach(this.logs, (l: string) => {
          ListItem() {
            Text(l)
              .fontSize(10.5)
              .fontColor('#333333')
              .width('100%')
          }
        }, (l: string, i: number) => i.toString() + l)
      }
      .width('100%')
      .height(150)
      .padding(8)
      .backgroundColor('#FAFAFA')
      .borderRadius(10)
    }
    .width('100%')
    .margin({ top: 14 })
  }

  @Builder
  pickerSheet() {
    Column() {
      Text('选择要连接的 iPhone')
        .fontSize(17)
        .fontWeight(FontWeight.Bold)
        .fontColor(C_INK)
        .width('100%')
      Text('只列出系统蓝牙里已配对的设备')
        .fontSize(12)
        .fontColor(C_HINT)
        .width('100%')
        .margin({ top: 4 })

      if (this.devices.length === 0) {
        Text('没有已配对设备。先到系统「设置 → 蓝牙」里和 iPhone 配对，再回来点这里。')
          .fontSize(13)
          .fontColor(C_MUTED)
          .lineHeight(20)
          .width('100%')
          .margin({ top: 20 })
        Blank()
      } else {
        List({ space: 6 }) {
          ForEach(this.devices, (d: string) => {
            ListItem() {
              Text(d)
                .fontSize(15)
                .fontColor(C_INK)
                .width('100%')
                .padding({ left: 14, right: 14, top: 14, bottom: 14 })
                .backgroundColor('#F7F8FA')
                .borderRadius(12)
            }
            .onClick(() => {
              // "名字  AA:BB:.." —— 地址固定在末尾 17 个字符
              const addr: string = d.slice(d.length - 17);
              const name: string = d.slice(0, d.length - 19).trim();
              this.showPicker = false;
              this.connect(addr, name);
            })
          }, (d: string) => d)
        }
        .layoutWeight(1)
        .width('100%')
        .margin({ top: 14 })
      }
    }
    .width('100%')
    .height('100%')
    .padding({ left: 20, right: 20, top: 18, bottom: 20 })
    .backgroundColor(C_WHITE)
  }

  // -------------------------------------------------------------------------
  // 页面
  // -------------------------------------------------------------------------
  /// XComponent 起来后把 surface id 交给原生硬解。
  /// 注意：surface 只在 XComponent 挂载时存在；卸载要立刻告诉原生（喂 0），
  /// 否则解码器会往一块已经被系统回收的 surface 上画。
  private attachSurface(): void {
    try {
      const id: string = this.xc.getXComponentSurfaceId();
      this.log('[video] surface id=' + id);
      setVideoSurface(id);
    } catch (e) {
      const err = e as BusinessError;
      this.log('[video] 取 surface 失败：' + err.code + ' ' + err.message);
    }
  }

  // -------------------------------------------------------------------------
  // 触摸：屏幕上的手指 → 事件通道的 HID report
  // -------------------------------------------------------------------------
  /// XComponent 里的坐标是 vp、相对组件左上角；手机要的是**归一化 0..1**，
  /// 所以除以组件的实际宽高。组件按 videoW:videoH 摆，画面铺满组件，两者比例一致。
  private clamp01(v: number): number {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
  }

  /// 找一个空槽（0 或 1）；两个都在用返回 -1（第三根手指忽略）。
  private freeSlot(): number {
    let used0: boolean = false;
    let used1: boolean = false;
    this.touchSlots.forEach((slot: number) => {
      if (slot === 0) used0 = true;
      else if (slot === 1) used1 = true;
    });
    if (!used0) return 0;
    if (!used1) return 1;
    return -1;
  }

  private onCarplayTouch(e: TouchEvent): void {
    if (this.compW <= 0 || this.compH <= 0) return;
    const xs: number[] = [0, 0];
    const ys: number[] = [0, 0];
    const downs: boolean[] = [false, false];
    let any: boolean = false;

    // 1) 先处理抬起：Up/Cancel 可能在 touches，也可能只在 changedTouches 里
    for (let i = 0; i < e.changedTouches.length; i++) {
      const t: TouchObject = e.changedTouches[i];
      const slot: number | undefined = this.touchSlots.get(t.id);
      if (slot === undefined) continue;
      if (t.type !== TouchType.Up && t.type !== TouchType.Cancel) continue;
      xs[slot] = this.clamp01(t.x / this.compW);
      ys[slot] = this.clamp01(t.y / this.compH);
      downs[slot] = false;
      this.touchSlots.delete(t.id);
      any = true;
    }

    // 2) 按着的 / 移动中的
    for (let i = 0; i < e.touches.length; i++) {
      const t: TouchObject = e.touches[i];
      if (t.type === TouchType.Up || t.type === TouchType.Cancel) {
        const slot: number | undefined = this.touchSlots.get(t.id);
        if (slot === undefined) continue;
        xs[slot] = this.clamp01(t.x / this.compW);
        ys[slot] = this.clamp01(t.y / this.compH);
        downs[slot] = false;
        this.touchSlots.delete(t.id);
        any = true;
        continue;
      }
      let slot: number | undefined = this.touchSlots.get(t.id);
      if (slot === undefined) {
        const free: number = this.freeSlot();
        if (free < 0) continue;   // 第三根手指：HID 描述符只有两个槽
        slot = free;
        this.touchSlots.set(t.id, slot);
      }
      xs[slot] = this.clamp01(t.x / this.compW);
      ys[slot] = this.clamp01(t.y / this.compH);
      downs[slot] = true;
      any = true;
    }
    if (!any) return;

    const contacts: TouchPoint[] = [
      { x: xs[0], y: ys[0], down: downs[0] },
      { x: xs[1], y: ys[1], down: downs[1] }
    ];
    if (!sendTouch(contacts) && !this.touchWarned) {
      this.touchWarned = true;
      this.log('[touch] 转发失败（事件通道还没建好）');
    }
  }

  build() {
    Stack() {
    Column() {
      this.header()
      Divider().color(C_RULE).strokeWidth(1)

      Scroll() {
        Column() {
          Text('欢迎使用AllPlay')
            .fontSize(this.wide ? 31 : 27)
            .fontWeight(FontWeight.Bold)
            .fontColor(C_INK)
            .textAlign(TextAlign.Center)
            .width('100%')
            .margin({ top: this.wide ? 30 : 22 })

          // 窄屏一行放不下（375vp 上会只剩"服务"两字掉到第二行），拆成两行更整齐
          Text(this.wide ? '连接成功后，即可在车机屏幕使用完整的CarPlay服务'
                         : '连接成功后，即可在车机屏幕\n使用完整的CarPlay服务')
            .fontSize(13)
            .fontColor(C_MUTED)
            .textAlign(TextAlign.Center)
            .lineHeight(21)
            .width('100%')
            .padding({ left: 24, right: 24 })
            .margin({ top: 8 })

          if (this.wide) {
            Row({ space: 24 }) {
              Column() {
                this.deviceCard()
              }
              .layoutWeight(1)

              Column() {
                this.visualPanel()
              }
              .layoutWeight(1.15)
            }
            .width('100%')
            .alignItems(VerticalAlign.Center)
            .margin({ top: 28 })
          } else {
            Column({ space: 18 }) {
              this.deviceCard()
              this.visualPanel()
            }
            .width('100%')
            .margin({ top: 22 })
          }

          Button('取消')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor(C_WHITE)
            .backgroundColor(C_INK)
            .width(200)
            .height(48)
            .borderRadius(12)
            .margin({ top: 26 })
            .onClick(() => {
              this.disconnect();
            })

          if (this.showLogs) {
            this.logPanel()
          }
        }
        .width('100%')
        .padding({ left: 20, right: 20, top: 4, bottom: 24 })
        // 内容不满一屏时整体居中（桌面那版就是上下留白对称的）；
        // 展开日志撑高之后，minHeight 不再生效，自然退回从顶部滚动。
        .constraintSize({ minHeight: '100%' })
        .justifyContent(FlexAlign.Center)
      }
      .layoutWeight(1)
      .align(Alignment.Top)
      .onAreaChange((_old: Area, now: Area) => {
        const w: number = Number(now.width);
        this.wide = w >= 600;
      })

      Divider().color(C_RULE).strokeWidth(1)

      // ---------------- 页脚 ----------------
      Row() {
        Circle()
          .width(8)
          .height(8)
          .fill(this.dotColor())
        Text(this.footerText())
          .fontSize(12)
          .fontColor(C_FOOTER)
          .fontWeight(FontWeight.Medium)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)
          .margin({ left: 8 })
        Text('AllPlay · CarPlay')
          .fontSize(12)
          .fontColor(C_FOOTER)
          .margin({ left: 12 })
      }
      .width('100%')
      .height(46)
      .padding({ left: 20, right: 20 })
      .onClick(() => {
        // 版面上看不出来的诊断入口
        this.showLogs = !this.showLogs;
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(C_WHITE)
    .bindSheet($$this.showPicker, this.pickerSheet(), {
      height: 460,
      dragBar: true,
      backgroundColor: C_WHITE,
      onDisappear: () => {
        this.showPicker = false;
      }
    })

    // 手机开始连接就挂上 XComponent：视频 SETUP 一到就要有 surface 可解码，
    // 晚挂上只能等手机 5 秒后重发配置帧，画面会白等一轮。
    if (this.linkState === ST_CONNECTING || this.linkState === ST_STREAMING) {
      Column() {
        XComponent({
          id: 'carplay',
          type: XComponentType.SURFACE,
          controller: this.xc
        })
          .onLoad(() => {
            this.attachSurface();
          })
          .onDestroy(() => {
            setVideoSurface('');
          })
          // 触摸：坐标相对组件，按组件实际大小归一化后交给原生转发给手机
          .onAreaChange((_old: Area, now: Area) => {
            this.compW = Number(now.width);
            this.compH = Number(now.height);
          })
          .onTouch((e: TouchEvent) => {
            this.onCarplayTouch(e);
          })
          // 画面宽高比 = 和手机协商的显示分辨率（横屏时基本铺满整块屏幕）。
          // 别用固定的 16:9：屏幕是 20:9 的话会上下留黑边。
          .width('100%')
          .aspectRatio(this.videoW / this.videoH)
      }
      .width('100%')
      .height('100%')
      .justifyContent(FlexAlign.Center)
      .backgroundColor(Color.Black)
    }
    }
    .width('100%')
    .height('100%')
  }
}
