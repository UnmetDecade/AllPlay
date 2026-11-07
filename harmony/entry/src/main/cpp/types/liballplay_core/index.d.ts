// liballplay_core.so 的类型声明（实现在 entry/src/main/cpp/napi_bridge.cpp）
// 配置字段对应 core/iap2/wireless_client.h 的 WirelessTriggerConfig。

export interface TriggerConfig {
  name?: string;
  model?: string;
  manufacturer?: string;
  serial?: string;
  firmware?: string;
  hardware?: string;
  /** 本机蓝牙 MAC（"AA:BB:CC:DD:EE:FF"）。鸿蒙不开放本机地址，填 00:.. 占位 */
  btMac?: string;
  ssid?: string;
  passphrase?: string;
  securityType?: number;
  airPlayPort?: number;
  deviceIdentifier?: string;
  publicKeyHex?: string;
  /** identity/ 目录（identity.pk8 + certificate.p7b）；空 = 发空证书，只验到识别 */
  mfiDir?: string;
  /** identity.pk8 的原始字节（从 HAP rawfile 读；优先于 mfiDir） */
  mfiKey?: ArrayBuffer;
  /** certificate.p7b 的原始字节 */
  mfiCert?: ArrayBuffer;
  ip?: string;
  timeoutMs?: number;
  /**
   * AirPlay 配对身份的持久化目录（应用沙箱里，如 filesDir + "/allplay-state"）。
   * 第一次会生成 Ed25519 密钥 + pairing id，之后复用；
   * 0x4301 的 publicKey / deviceIdentifier 就从这里来。空 = 不带身份，手机不会连 AirPlay。
   */
  stateDir?: string;
}

/** 起触发线程。onWrite 收到要写给手机的字节，onLog 收到过程日志。 */
export const start: (config: TriggerConfig,
                     onWrite: (data: ArrayBuffer) => void,
                     onLog: (line: string) => void) => void;

/** 把 sppRead 收到的字节喂给链路层。 */
export const feed: (data: ArrayBuffer) => void;

/** 取消并等工作线程退出（断开 socket 前必须调）。 */
export const stop: () => void;

/** 触发线程是否还活着。 */
export const isRunning: () => boolean;

/**
 * AirPlay 7000 探针：在 0.0.0.0:port 上监听，把手机连上来的第一条 RTSP 请求
 * 通过 onLog 打出来。用来验证「手机拿到 0x4301 之后会不会回连本机」。
 * 这不是完整的 RTSP 服务器 —— 只收不答。
 */
export const startAirplayProbe: (port: number, onLog: (line: string) => void) => void;

/** 停掉 7000 探针。 */
export const stopAirplayProbe: () => void;

/** AirPlay 配对身份（和 0x4301 里报的同一份）。 */
export interface AirplayIdentity {
  /** 合成 deviceid，"02:xx:xx:xx:xx:xx"（跨重启稳定） */
  deviceId: string;
  /** Ed25519 公钥 hex（小写） */
  pk: string;
  /** pairing uuid */
  pi: string;
}

/**
 * 读出（必要时生成）AirPlay 配对身份，供 ArkTS 广播 `_airplay._tcp`。
 * TXT 里的 deviceid/pk/pi 必须与 0x4301 一致 —— 手机靠它认出"同一台车机"。
 */
export const airplayIdentity: (stateDir: string) => AirplayIdentity;

/**
 * 探测手机的 CarPlay 控制服务：`GET /ctrl-int/1/connect`，
 * 带 `AirPlay-Receiver-Device-ID: <deviceId 去掉冒号>`。
 * 手机收到才会主动来连我们的 RTSP 端口 —— 和 Android 的 MdnsService.probeCarPlay 同款。
 * @returns 请求是否发出（连不上手机时 false）
 */
export const probeCarPlay: (host: string, port: number, deviceId: string) => boolean;

/**
 * 在指定网卡上自己找手机的 CarPlay 控制服务并打一枪。
 *
 * 系统 `@ohos.net.mdns` 的发现/广播都没有网络参数，只能走默认网络；热点和
 * STA/蜂窝同时开着时查询包发不到手机所在的那张网卡。这里直接在 ifaceIp
 * （通常是热点地址 192.168.43.1）上发 mDNS PTR 查询，解析到 SRV+A 之后
 * 调 `GET /ctrl-int/1/connect`。后台线程，最多找 120 秒。
 *
 * @param ifaceIp 要绑的网卡地址（热点 AP 的 IPv4）
 * @param deviceId AirPlay deviceid（冒号会自动去掉）
 * @returns 监听是否启动（ifaceIp 为空时 false）
 */
export const startCarPlayCtrlWatch: (ifaceIp: string, deviceId: string) => boolean;

/** 停掉 `startCarPlayCtrlWatch` 起的线程。 */
export const stopCarPlayCtrlWatch: () => void;

/**
 * 本机热点（AP）网卡的 IPv4；没开热点返回空串。
 * 鸿蒙的 connection.getAllNetsSync() 不列 AP 网卡，只能走内核 getifaddrs 要。
 */
export const hotspotIpv4: () => string;

/** AirPlay 接收端配置（core/airplay::ReceiverConfig 的子集）。 */
export interface ReceiverConfig {
  name?: string;
  /** RTSP 端口，默认 7000（被占会自动 +1）。 */
  port?: number;
  /** AirPlay 配对身份 + 配对记录的持久化目录（和 0x4301 用同一个）。 */
  stateDir?: string;
  /** 本机蓝牙 MAC（/info bluetoothIDs 用）。 */
  btMac?: string;
  width?: number;
  height?: number;
  /** identity.pk8 原始字节（HAP rawfile）。 */
  mfiKey?: ArrayBuffer;
  /** certificate.p7b 原始字节。 */
  mfiCert?: ArrayBuffer;
}

/**
 * 起真正的 AirPlay 接收端（RTSP + pair-setup/verify + SETUP）。
 * 注意：它自己会监听 port，**别再同时开 7000 探针**。
 * @returns 是否启动成功
 */
export const startAirplayServer: (config: ReceiverConfig,
                                  onLog: (line: string) => void) => boolean;

/** 停掉接收端（关端口、断连接）。 */
export const stopAirplayServer: () => void;

/** 接收端实际监听的端口；0 = 没在跑。 */
export const airplayServerPort: () => number;

/**
 * 把 XComponent 的 surface id 交给原生硬解（解码器直接画到这块 surface 上）。
 * 传空串表示 surface 要销毁了，停手。
 */
export const setVideoSurface: (surfaceId: string) => boolean;

/** 一个触点：坐标归一化到 0..1（相对协议里协商的主屏），down = 是否按下。 */
export interface TouchPoint {
  x: number;
  y: number;
  down: boolean;
}

/**
 * 把触摸转发给 iPhone（事件通道的 HID touchReport）。
 * 最多 2 个触点；抬起时也要发一次 down=false，否则手机那边的手指会一直按着。
 * @returns 是否发出去（事件通道未就绪时为 false）
 */
export const sendTouch: (contacts: TouchPoint[]) => boolean;

/** 自检：不开会话，直接用 48k 立体声正弦走一遍音频渲染器（仅调试用） */
export const testAudioSink: (ms?: number) => boolean;

/** 自检：用一段 ADTS 样片验证 AAC 解码，ADTS 与裸帧+ASC 两条路各跑一遍（仅调试用） */
export const testAudioAac: (bytes: Uint8Array) => boolean;
