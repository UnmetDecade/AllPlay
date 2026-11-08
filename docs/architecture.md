# AllPlay 架构

> 图用 Mermaid 绘制，GitHub / VS Code（装 Mermaid 插件）/ Typora 都能直接渲染。
> README 讲"这是什么、怎么跑"；这里讲**规则与契约** —— 改代码前该知道的那部分。

## 一、分层与四条规则

```mermaid
flowchart TB
    subgraph HOST["宿主层（组合根 / Composition Root）"]
        direction LR
        ANDROID["Android App<br/>MainActivity · AllPlaySessionService<br/>MediaCodecSink · AudioSink<br/>BluetoothService · MdnsService · HotspotManager"]
        DESKTOP["桌面 Host<br/>macOS/Linux: SDL2<br/>Windows: Win32 窗口"]
    end

    subgraph ADAPTER["平台适配层（实现 core::port 接口）"]
        direction LR
        POSIX["platform/posix<br/>net_posix · mdns(avahi/dns_sd)<br/>bt(IOBluetooth/BlueZ)"]
        WIN["platform/windows<br/>net_windows(Winsock2)<br/>bt_windows(AF_BTH)<br/>mdns"]
        JNI["android JNI<br/>jni_bridge.cpp"]
    end

    subgraph PORT["接口层（core/port，平台无关）"]
        direction LR
        TCP["ITcpListener / ITcpStream"]
        UDP["IUdpSocket"]
        NET["INetworkInfo"]
        BT["IBluetooth"]
        MDNS["IMdnsAdvertiser"]
        MEDIA["IVideoOutput / IAudioOutput"]
        STREAM["IByteStream"]
        PS["PlatformServices"]
    end

    subgraph CORE["协议核心（core，平台无关，唯一三方依赖 = OpenSSL）"]
        direction LR
        AIRPLAY["airplay<br/>rtsp · server · session<br/>video_stream · audio_stream<br/>event_channel · control_cipher · info"]
        IAP2["iap2<br/>wire · link · wireless_client"]
        MFI["mfi<br/>mfi_identity"]
        CRYPTO["crypto<br/>hkdf · chacha20poly1305 · srp6a<br/>x25519 · ed25519 · tlv8 · aes_ctr"]
        BPLIST["bplist"]
        BTTRIG["bt<br/>bt_trigger"]
        IDENTITY["platform<br/>device_identity · pairing_store"]
        COMMON["common<br/>log"]
    end

    HOST -->|注入实现| ADAPTER
    ADAPTER -->|实现| PORT
    CORE -->|依赖| PORT
    HOST -->|装配 PlatformServices| CORE
```

**四条规则**（改代码时按这个判断该放哪）：

1. **`core/` 里不许出现平台头。** 不 include `<sys/socket.h>` / `<SDL.h>` / `<jni.h>`，
   唯一的三方依赖是 OpenSSL。`core/` 要能脱离任何平台单独编译 —— `m3_test` / `iap2_test`
   就是这条规则的活证据（它们只链 `allplay_core`）。
2. **平台差异只走 `core/port/*.h`。** 需要一个新能力（比如读电池电量），
   先在 `core/port/` 定义接口，再去各平台实现，**而不是**在 core 里 `#ifdef __ANDROID__`。
3. **宿主层是唯一的组合根。** 没人比宿主更清楚"这台机器上蓝牙该用哪个实现"。
   `host/main.cpp`、`android/.../jni_bridge.cpp`、各 `tools/*.cpp` 各自装配自己的那份。
4. **依赖单向朝内。** `host → platform → port ← core`。core 不认识 platform，platform 不认识 host。

## 二、无线 CarPlay 数据流

```mermaid
flowchart LR
    IPHONE["iPhone<br/>(CarPlay 源端)"]

    subgraph RECEIVER["AllPlay 接收端"]
        BT_TRIG["蓝牙 iAP2 触发<br/>RFCOMM → 0x4301"]
        RTSP["RTSP 控制通道 :7000<br/>pair-setup / pair-verify<br/>auth-setup / SETUP / RECORD"]
        SCREEN["屏幕流 110/111<br/>TCP · H.264 · AVCC→AnnexB"]
        AUDIO["音频流 100/101/102<br/>UDP RTP · AAC/Opus/LPCM"]
        EVENT["事件通道 eventPort<br/>触摸/命令 · ControlCipher 加密"]
        MDNS_B["mDNS 广播<br/>_airplay._tcp"]
    end

    IPHONE -->|"① 蓝牙连入，发 iAP2"| BT_TRIG
    BT_TRIG -->|"② 0x4301 引导（SSID/密码/IP/端口）"| IPHONE
    IPHONE -->|"③ 连 Wi-Fi 后经 mDNS 发现"| MDNS_B
    IPHONE -->|"④ RTSP 协商"| RTSP
    RTSP -->|"⑤ 建立数据流"| SCREEN
    RTSP -->|"⑤ 建立音频流"| AUDIO
    RTSP -->|"⑤ 建立事件通道"| EVENT
    SCREEN -->|"H.264 帧 → IVideoOutput 解码上屏"| SCREEN
    EVENT -->|"触摸 → hidSendReport → 回 iPhone"| IPHONE
```

第 ① 步是**客户端模式**：我们主动连 iPhone 的 iAP2 SPP 服务（`00000000-deca-fade-deca-deafdecacafe`），
不是等手机连我们 —— 车机端就该这么干，手机只在收到我们的连接之后才往下走。

## 三、核心模块职责

| 模块 | 职责 |
|---|---|
| `airplay/rtsp` | 极简 RTSP 服务器（控制通道，pair-verify 后整条加密） |
| `airplay/server` | `Receiver`：组合根之外的门面，起 RTSP + mDNS 广播，持有 `PlatformServices` |
| `airplay/session` | 会话状态机：pair-setup / pair-verify / auth-setup / SETUP / RECORD |
| `airplay/video_stream` | 屏幕流：128B 头解密 → AVCC→AnnexB → 关键帧闸门 |
| `airplay/audio_stream` | 音频流：RTP over UDP 解密 + 序号乱序重排 |
| `airplay/event_channel` | 事件通道：触摸 HID 回传（最多 2 触点）+ `/command` 命令 |
| `airplay/control_cipher` | 控制通道帧加密（ChaCha20-Poly1305，双向独立计数器） |
| `airplay/info` | `/info` 的 bplist 应答（设备身份、公钥、支持的流） |
| `iap2/wire` | iAP2 线格式：TLV 参数编解码、CSM 拆帧 |
| `iap2/link` | iAP2 链路层：marker→SYN→ACK→NORMAL 握手、序号与校验和 |
| `iap2/wireless_client` | 蓝牙侧控制流程（`0xAA00`~`0xAA05` / `0x1D00` / `0x5703` / `0x4301`） |
| `mfi/mfi_identity` | MFi 配件身份（证书 + ECDSA 签名）。**接口按硬件芯片的形状设计**，换芯片实现不动 core |
| `crypto/*` | 加密原语全部从零实现，不依赖第三方协议库 |
| `bplist` | Apple 二进制 plist 编解码 |
| `bt/bt_trigger` | 蓝牙触发编排：选设备 → RFCOMM → iAP2 → `0x4301` 引导 |
| `platform/device_identity` | 本机 AirPlay 身份（Ed25519 密钥对），与 MFi 身份是**两码事** |
| `platform/pairing_store` | 配对记录持久化（谁配对过、共享密钥） |

## 四、接口层契约

### `core::PlatformServices`（组合根注入的能力束）

```cpp
struct PlatformServices {
    INetworkInfo* network;      // 网卡 IP/MAC —— AirPlay 广播与 0x4301 引导要用
    IBluetooth*   bluetooth;    // 蓝牙触发；不触发可为空
    std::function<std::unique_ptr<ITcpListener>()>    makeTcpListener;      // 每用建一个
    std::function<std::unique_ptr<IMdnsAdvertiser>()> makeMdnsAdvertiser;
    std::function<std::unique_ptr<IUdpSocket>()>      makeUdpSocket;
};
```

工厂用 `std::function` 是因为监听器 / 广播器要"用几个建几个"（RTSP、数据、事件各一个端口）。

> ⚠️ **`Receiver` 会自己持有一份 `PlatformServices` 的拷贝**，不存调用方传进来的地址。
> 原因是 `Session` 到 SETUP 才第一次用 `makeTcpListener`，那时 `start()` 早已返回 ——
> 存地址就是悬垂指针，症状是 SETUP 阶段 SIGSEGV。

### `core::IVideoOutput`

| 方法 | 约定 |
|---|---|
| `configure(codec, w, h, codecData)` | `codecData` = **剥掉 fourcc 的 avcC/hvcC 记录体**（首字节 `0x01`）。`w/h` 只是协商值，真实分辨率看码流 SPS。**会被重复调用**，配置没变必须跳过重建，否则周期性黑帧 |
| `feedAnnexB(data, len)` | 喂 AnnexB 帧。**返回 `false` 代价很高** —— core 会重新放闸并立刻向手机请求关键帧。只在解码器真的不可用时才返回 false |
| `reset()` | 关键帧恢复：清解码器 + **清帧队列**（否则恢复后还在显示陈旧帧） |
| `flush()` | 新 IDR 前清参考链。core 目前不调用 |

### `core::IAudioOutput`

`configure()` 每次 SETUP 音频流重建时调用一次；`feed()` 给的是**裸帧** ——
AAC-LC 无 ADTS 头（且会把 ≤12 字节的 config 小包也喂进来，要忽略）、Opus 裸包、LPCM 裸 PCM。
`sample` 是 RTP 时间戳，供重排与时钟用。

### `core::IBluetooth`

`pairedDevices()` / `connectRFCOMM(addr, uuid, err)` / `waitForPhoneConnection()`。
**返回的流不归调用方所有**，别 delete。

## 五、线程模型（本项目最容易出错的地方）

`core` 内部起了这些线程：

| 线程 | 在哪起 | 它会调你的哪个回调 |
|---|---|---|
| RTSP accept | `rtsp.cpp:135` | 每个连接一条处理线程 |
| ScreenStream | `video_stream.cpp:30` | `IVideoOutput::configure()` + `feedAnnexB()` |
| AudioStream | `audio_stream.cpp:44` | `IAudioOutput::feed()` |
| EventChannel | `event_channel.cpp:110` | — |
| 关键帧 / NTP / keep-alive | `session.cpp:451 / 472 / 504` | — |

**关键的不对称**（踩过一次，见 CHANGELOG v0.2 的修复）：

| 回调 | 来自哪个线程 |
|---|---|
| `IVideoOutput::configure()` / `feedAnnexB()` | **ScreenStream 线程** |
| `IVideoOutput::reset()` | **RTSP 线程**（`/command forceKeyFrame` 或丢关键帧后触发） |
| `IAudioOutput::configure()` | **RTSP 线程**（SETUP 里） |
| `IAudioOutput::feed()` | **AudioStream 线程** |

也就是说 **`reset()` 与 `feedAnnexB()` 是两个线程**在碰同一份解码器状态 ——
平台实现**必须自己加锁**，core 不管。

另外一条既有约定：**SDL 只在主线程调用**。所以音频设备的 open/close 不能直接在
`configure()`（RTSP 线程）里做，只能写一个原子 pending 标志，由主循环发现后处理。
宿主侧的实际做法见 `host/media/sdl_audio_sink.cpp` 的注释。

## 六、平台适配矩阵

| 能力 | Linux | macOS | Android | Windows |
|---|---|---|---|---|
| TCP / UDP | POSIX socket ✅ | POSIX socket ✅ | Socket ✅ | Winsock2 ✅ |
| 网卡信息 | `getifaddrs` ✅ | `getifaddrs` ✅ | `NetworkInterface` ✅ | `GetAdaptersAddresses` ✅ |
| 蓝牙 | **BlueZ**（SDP + 裸 RFCOMM，客户端模式）✅ 真机验到 `0xAA00` | IOBluetooth ⏳ 未真机验收 | `BluetoothSocket` ✅ | `AF_BTH` ⏳ 骨架 |
| 有线（USB） | libusb 切配置 + usbmux v2 + 内核 `cdc_ncm` ✅ 真机端到端 | 系统 `usbmuxd` + `AppleUSBNCMData`（不碰 USB，无需 DriverKit）✅ 真机端到端（投屏 + 声音） | — | — |
| mDNS | avahi ✅ | dns_sd ✅ | `NsdManager` ✅ | ⏳ 待做 |
| 视频输出 | ffmpeg 软解 + SDL2 ✅ | ffmpeg 软解 + SDL2 ✅ | MediaCodec 硬解 ✅ | ffmpeg ⏳ 待接 |
| 音频输出 | SDL2（AAC-LC / Opus / LPCM）✅ | SDL2 ✅ | AudioTrack ✅ | ⏳ 待接 |
| 桌面宿主 | ✅ 端到端跑通 | ✅ 端到端跑通 | —（用原生 App） | ⏳ 仅有窗口 |

对应实现文件：

```
platform/posix/  net_posix.cpp      mdns_avahi.cpp | mdns_dns_sd.cpp
                 bt_bluez.cpp      | bt_iobluetooth.mm
                 usb_libusb.cpp    | usbmuxd_mac.cpp
platform/windows/ net_windows.cpp   bt_windows.cpp
android/.../cpp/  jni_bridge.cpp
```

## 七、接一个新平台要做什么

按依赖顺序，每一步都能单独验证：

1. **`net`**：实现 `INetworkInfo` + `ITcpListener`/`ITcpStream` + `IUdpSocket`。
   验证：`core_selftest` 能起来、`/info` 能应答。
2. **`mdns`**：实现 `IMdnsAdvertiser`（广播 `_airplay._tcp`）。
   验证：`avahi-browse -t _airplay._tcp`（或对应平台的工具）能看到。
3. **`video`**：实现 `IVideoOutput`。**先别碰协议** —— 用 `tools/video_probe` 的思路
   本地喂一段 H.264，把"解码 + 上屏"跑通再往下走。
4. **`audio`**：实现 `IAudioOutput`（三个格式，至少要能忽略 AAC 的 config 小包）。
5. **`bluetooth`**：实现 `IBluetooth`。Linux 上是**客户端模式**（我们连手机），
   别照着"服务端等手机连"的思路写。
6. **宿主**：把上面几件装配成 `PlatformServices` + `ReceiverConfig`，起主循环。
7. **验收**：`tools/fake_phone` 跑端到端（不需要 iPhone），再拿真机验蓝牙那一段。

## 八、多协议演进（草案）

CarPlay 目前直接写在 `core/airplay` / `core/iap2` 等目录里。要让同一套 `core` 再接
Android Auto / CarLife / HiCar，路线是**协议后端化**：宿主只依赖
`core::projection::IProjectionBackend`，CarPlay 是第一个后端，其余按同一接口接入。
接口草案与迁移方案见 [projection-backend.md](projection-backend.md)。

当前状态（2026-11-08）：

| 后端 | 代码 | 状态 |
|---|---|---|
| CarPlay | `core/projection/carplay/`（`backend_carplay` 包住原 airplay/iap2/mfi/usb/bt） | 模块已就位，宿主尚未切换 |
| Android Auto（有线） | `core/projection/androidauto/` + `platform/posix/aa_usb_libusb.cpp` | 编译通过，待真机 |
| CarLife | `core/projection/carlife/` | 占位（接口齐全，未实现） |
| HiCar | `core/projection/hicar/` | 占位（接口齐全，未实现） |

AA 的有线链路是 AOAP（51/52/53）→ AAP VERSION → TLS 1.2（记录装在 0x0003）→
服务发现 → 通道 / 音视频 / 输入。protobuf 只实现了 wire 子集（`proto.h`），
TLS 用系统 OpenSSL，不引 protobuf / Boost / AASDK 代码。

