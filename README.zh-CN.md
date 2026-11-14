# AllPlay

<div align="left"><a href="README.md">English</a> · <b>简体中文</b></div>

![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platforms](https://img.shields.io/badge/platforms-Android%20%7C%20HarmonyOS%20%7C%20Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![Release](https://img.shields.io/badge/release-v1.1-informational)

**AllPlay 是一个用 C++17 实现的车机侧 CarPlay 接收端。** 它端到端实现了真实协议栈 ——
AirPlay RTSP、iAP2、MFi-SAP、USB lockdown/TLS 与 AAP —— 本地解码 H.264 视频与
AAC-LC/Opus/LPCM 音频，并把触摸以 HID 报文回传给手机。无线与有线 CarPlay 共用同一套
AirPlay 会话核心，只有引导与传输层不同。

工程规模（v1.1，含头文件）：

- `core/` —— 约 13.7k 行 C/C++，不含任何平台头文件，唯一三方依赖是 OpenSSL。
- 桌面组合根 + 平台适配层 + 验收工具 —— 约 27.6k 行。
- Android 宿主 —— 约 2.5k 行 Kotlin/C++。
- 5 个平台目标；4 个协议后端（1 个已落地，1 个协议完整，2 个仅接口）。

![AllPlay 在 macOS 上的启动页：等待 iPhone 连接](docs/images/allplay-launcher-mac.png)

---

## 1. 范围

### 已实现并在真机验证

| 路径 | 实际跑通的内容 |
|---|---|
| **无线 CarPlay** | 蓝牙 iAP2 触发 → mDNS 发现 → RTSP 协商 → 加密 H.264/AAC 流 → 触摸与媒体键 |
| **有线 CarPlay** | USB 切配置 → usbmux v2 → lockdown → 原地升 TLS → `com.apple.carkit.service` → iAP2 → 内核 NCM 媒体面 |
| **媒体管线** | Android/HarmonyOS 硬件解码 H.264，桌面 ffmpeg 软解；AudioTrack / OHAudio / SDL2 输出 |
| **协议后端外观** | `IProjectionBackend` / `IProjectionSession` / `IProjectionSink`；CarPlay 是第一个后端 |

### 已实现但未上真机

- **有线 Android Auto**：AOAP 51/52/53 → AAP VERSION → TLS 1.2 → 服务发现 →
  control/video/audio/input 通道。协议与 libusb 传输可编译并通过 `aa_wire_test`；
  **尚未接过真机，宿主也没有 UI 入口**。

### 仅接口 / 未实现

- **CarLife**、**HiCar**：`probe()` 返回空，`connect()` 报未实现。
- **Windows 宿主**：Winsock2 网络层可编译；mDNS、蓝牙、ffmpeg 视频与音频输出仍是 TODO。
- **Android/HarmonyOS 上的有线 CarPlay 或 Android Auto**：没有 USB 传输层与 UI。

### 不做任何声称

AllPlay 不是 Apple 产品，也没有 MFi 授权。仓库不含任何 Apple 代码、二进制或文档。
协议事实来自公开的上游实现，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

> **⚠️ 凭证前提。** 两条 CarPlay 路径的最后一道门都是 MFi 配件身份（`identity/`：
> EC P-256 私钥 + Apple 签发的证书）。它**不在本仓库**，也不受 GPL 覆盖，必须由使用者
> 自行提供。自签名身份足以在开发期走通全链路，但不是可出货方案；合规责任在使用者。

## 2. 能力矩阵

### 协议与传输

| 平台 | 无线 CarPlay | 有线 CarPlay | 有线 Android Auto | mDNS / 蓝牙适配 |
|---|---|---|---|---|
| **Android** | ✅ 端到端，硬件解码（含 API 19 盒子） | — | — | `NsdManager` / `BluetoothSocket` |
| **HarmonyOS** | ✅ 真机端到端（Mate 60） | — | — | ArkTS SPP / 系统 mDNS |
| **Linux** | ✅ 真实 iPhone | ✅ 真实 iPhone（视频；音频代码路径共用，但该次未验证） | 🟡 协议 + libusb，未上真机 | avahi / BlueZ |
| **macOS** | ✅ 真实 iPhone | ✅ 真实 iPhone（视频 + 音频） | 🟡 协议 + libusb，未上真机 | dns_sd / IOBluetooth |
| **Windows** | ⏳ 骨架 | — | — | ⏳ TODO |

### 媒体与输入

| 平台 | 视频 | 音频 | 输入 |
|---|---|---|---|
| **Android** | MediaCodec → `Surface` | `AudioTrack` | 归一化触摸 → HID 报文 |
| **HarmonyOS** | `OH_VideoDecoder` → `XComponent` | `OH_AudioCodec`（每帧前置 7 字节 ADTS）→ `OHAudio`，1.5 s 环形缓冲 / 700 ms 恢复水位 | `XComponent` 触摸 → 归一化 → HID |
| **Linux / macOS** | ffmpeg → SDL2 纹理（letterbox） | SDL2（AAC-LC / Opus / LPCM） | SDL 鼠标/触摸 → HID |
| **Windows** | ffmpeg ⏳ | — | — |

两个一开始就该知道的结构性约束：

- **有线媒体面不是本项目写的**。CarPlay USB 配置选中后，内核 `cdc_ncm` 自己拉起一块
  真实网卡（Linux），或由系统把 NCM 功能挂成 `enX`（macOS，`AppleUSBNCMData`）。
  AllPlay 只把 AirPlay 绑到那块网卡；NTB16/Ethernet/IPv6 封装是内核的事。
- **macOS 的有线控制面完全不碰 USB**，而是借用 Finder 用的系统 `usbmuxd` 隧道，
  因此这台 Mac 必须已经被手机信任过。

## 3. 构建

### 工具链

- CMake ≥ 3.16、C++17、pkg-config、OpenSSL 3。
- **Linux**：`build-essential cmake pkg-config libssl-dev libusb-1.0-0-dev libdbus-1-dev
  libbluetooth-dev bluez libavahi-client-dev libsdl2-dev libsdl2-ttf-dev libavcodec-dev
  libavformat-dev libswscale-dev libavutil-dev fonts-dejavu-core`。
- **macOS**：`brew install openssl@3 sdl2 sdl2_ttf ffmpeg libusb`（dns_sd 与 IOBluetooth
  是系统框架）。
- **Android**：SDK + NDK **r25c**（r26 起不再支持 API 19），OpenSSL 按 ABI 交叉编译。
- **HarmonyOS**：DevEco Studio SDK + NDK。

### 桌面（Linux / macOS）

```bash
git clone <repo> && cd AllPlay
bash scripts/bootstrap.sh        # Linux 装依赖；macOS 请先装上面的 brew 列表
bash scripts/build.sh            # 产物在 build/
bash scripts/m1-check.sh         # RTSP + /info（bplist）+ mDNS 验收
./build/m3_test                  # 加密向量 + 假手机 + 屏幕流
./build/aa_wire_test             # Android Auto wire 层
```

> `scripts/*.sh` 在仓库里没有可执行位。请用 `bash scripts/xxx.sh` 调用；
> `./scripts/build.sh` 会直接 permission denied。

### Android

```bash
cd android && ./gradlew :app:assembleDebug
```

`minSdk 19`、`targetSdk 34`、ABI `arm64-v8a` + `armeabi-v7a`。OpenSSL 放在按 ABI 划分的
目录（`openssl-arm64`、`openssl-arm`）；交叉编译说明在
`android/app/src/main/cpp/CMakeLists.txt` 文件头。

### HarmonyOS

```bash
bash scripts/build-ohos-openssl.sh     # 产物在 tmp/ohos-ssl/（不入库）
bash scripts/sync-harmony-identity.sh  # 把 identity/ 拷进 HAP 的 rawfile
cd harmony && hvigorw assembleHap --mode module -p product=default
hdc install -r entry/build/default/outputs/default/entry-default-signed.hap
```

把 `harmony/build-profile.example.json5` 复制成 `harmony/build-profile.json5`，填入自己的
签名证书；真实配置不入库。

## 4. 运行

### 三种模式

```bash
# ① 被动接收：等手机连进来，应答 RTSP
./build/allplay_host --name "AllPlay"

# ② 无线触发：用蓝牙 iAP2 叫醒手机，再接住它打开的 AirPlay
./build/allplay_host --bt --bt-device iPhone \
  --ssid "<Wi-Fi SSID>" --wifi-pass "<密码>"

# ③ 有线：不要蓝牙、不要同网段；前置条件见下
sudo env ALLPLAY_STATE_DIR="$HOME/.local/state/allplay" \
  ./build/allplay_host --wired --identity identity
```

`--bt` 与 `--wired` 都是严格 opt-in 且互斥；不加就是纯接收端。`--dual` 同时监听两条路，
谁先连上算谁。

### 命令行参数

**通用**

| 参数 | 默认值 | 作用 |
|---|---|---|
| `--screen WxH` | `1280x720` | 渲染面尺寸，也是向手机协商的目标分辨率 |
| `--fullscreen` | 关 | `SDL_WINDOW_FULLSCREEN_DESKTOP` |
| `--statusbar` | 关 | 调试叠加：状态点、分辨率、fps、帧数、音频欠载 |
| `--statusbar-h N` | `26` | 状态栏高度（像素） |
| `--font PATH` | DejaVu | TTF 字体；也可用 `ALLPLAY_FONT` |
| `--frames N` | 0 | 渲染 N 帧后退出（无头冒烟测试） |
| `--port N` | `7000` | RTSP 端口；被占则自动 +1 |
| `--ifname IFACE` | 自动 | mDNS/AirPlay 使用的网卡 |
| `--name S` | `AllPlay` | Bonjour 服务实例名 |
| `--state-dir DIR` | `$HOME/.local/state/allplay` | Ed25519 身份与配对记录 |
| `--identity DIR` | `identity` | MFi 配件身份目录 |
| `--no-avahi` | 关 | 不广播 `_airplay._tcp` |
| `--no-video` | 关 | 只跑协议，不建视频输出 |
| `--no-audio` | 关 | 只跑协议，不建音频输出 |
| `--dump PATH` | — | 退出前把最后一帧存成 PPM |
| `--inject-touch U,V` | — | 连接后注入一次归一化触摸 |

**无线**

| 参数 | 默认值 | 作用 |
|---|---|---|
| `--bt` | 关 | 启用蓝牙 iAP2 触发 |
| `--bt-addr A:B:C:D:E:F` | — | 按地址直连，跳过名字匹配 |
| `--bt-device NAME` | `iPhone` | 按已配对设备名做子串匹配 |
| `--bt-mode client\|server` | `client` | client 连手机的 SPP 服务；server 注册 SDP 等手机连入 |
| `--ssid S` | 当前 Wi-Fi | `0x5703` 里发给手机的 SSID |
| `--wifi-pass P` | 空 | `0x5703` 里发给手机的密码 |
| `--bt-retry N` | `10` | 整链重试次数，间隔 3 s |

**有线**

| 参数 | 默认值 | 作用 |
|---|---|---|
| `--wired` | 关 | 有线 CarPlay |
| `--dual` | 关 | 有线 + 无线同时监听；先连上者胜 |
| `--ncm-link-local A` | `fe80::2` | 钉在 NCM 网卡上、并在 `0x4301` 里上报的 IPv6 链路本地地址 |
| `--wired-pid N` | 不限 | 限定 USB 产品号（默认自动排除 Apple 键盘/扩展坞） |

运行时按键：`B` 重新触发无线，`W` 重试有线准备，`Esc` / 关窗退出。

### 环境变量

| 变量 | 取值 | 作用 |
|---|---|---|
| `ALLPLAY_LOG` | `debug` / `info` / `warn` / `error` | 日志阈值（默认 `info`） |
| `ALLPLAY_STATE_DIR` | 路径 | 覆盖状态目录；`sudo` 下必须显式给 |
| `ALLPLAY_FONT` | 路径 | 默认 TTF 字体 |

### 有线前置条件（Linux —— 三条缺一不可）

```bash
sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd   # USBMUX 口只能有一个主人
pkill -f gvfsd-gphoto2                                      # 否则 gvfs 会抢着挂载照片
echo "blacklist apple_mfi_fastcharge" | sudo tee /etc/modprobe.d/blacklist-apple-mfi.conf
```

`/dev/bus/usb/*` 只有 root 能开，因此**整个程序都要用 `sudo` 跑**。`sudo` 会重置环境：
`ALLPLAY_LOG` 与 `ALLPLAY_STATE_DIR` 必须显式传入，否则 `HOME=/root` 会另建一份本地身份，
每次都要重新配对。

真实 iPhone 的有线画面（Ubuntu，插着数据线）：

![真实 iPhone 的有线 CarPlay 渲染在 Ubuntu 上](docs/images/carplay-wired.jpg)

触摸走的是与无线**完全相同**的事件通道（`POST /command` + 加密 HID）。下图在
"Appearance" 行注入了一次点击：

![注入触摸后，CarPlay 进入了 Appearance 页面](docs/images/carplay-wired-touch.jpg)

> 那次有线会话没有验证音频：手机只在真正出声时才创建音频流。代码路径与无线共用，
> 但**没在真机上跑过的东西不算验证过**。

## 5. 协议层

协议事实、报文布局与真机实测值维护在 [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md)。
下面是实现覆盖面。

### 5.1 无线 CarPlay

```
iPhone                                      AllPlay（车机侧）
  │ ① RFCOMM SPP → iAP2                        │
  ├── 0x1D00 StartIdentification ─────────────▶
  ◀── 0x1D01 IdentificationInfo / 0x1D02 ──────
  ├── 0xAA00 RequestCertificate ──────────────▶
  ◀── 0xAA01 证书 / 0xAA02 challenge ──────────
  ├── 0xAA03 signature ───────────────────────▶
  ◀── 0xAA05 success ──────────────────────────
  ├── 0x5702 RequestAccessoryWiFiConfig ──────▶
  ◀── 0x5703 AccessoryWiFiConfiguration ───────
  ├── 0x4300 CarPlayAvailability ─────────────▶
  ◀── 0x4301 CarPlayStartSession ──────────────
  │ ② 手机加入局域网，发现 _airplay._tcp        │
  │ ③ RTSP /pair-setup → /pair-verify → SETUP   │
  │ ④ 屏幕流 / 音频流 / 事件通道                │
```

| 层 | 传输 | 实现要点 |
|---|---|---|
| 触发 | RFCOMM SPP `00000000-deca-fade-deca-deafdecacafe` | 客户端模式：我们连手机。服务端模式则注册 "Wireless iAPv2" 的 SDP 记录等手机连入 |
| iAP2 链路 | RFCOMM | 6 字节起始标记 `FF 55 02 00 EE 10`；9 字节数据头 `FF5A \| u16 len \| control \| seq \| ack \| session \| checksum`，校验和 ≡ 0 (mod 256)；SYN `0x80` / ACK `0x40` / EAK `0x20` / RST `0x10`；初始 seq 99，4 s 重传，最多 4 次 |
| iAP2 消息 | CSM | `0x4040 \| u16 len \| u16 msgId \| TLVs`（大端） |
| 发现 | mDNS | 广播 `_airplay._tcp`，TXT 含 `deviceid`（网卡 MAC）、`features`、`flags=0x4`、`model`、`srcvers=950.7.1`、`protovers=1.1`、`pi`、`pk` |
| 促使手机连接 | mDNS + HTTP | 解析 `_carplay-ctrl._tcp`，再 `GET /ctrl-int/1/connect`，带 `AirPlay-Receiver-Device-ID`；7 次 × 1.5 s，连接/读取超时 3 s |
| 控制 | RTSP over TCP `:7000` | `/info`、`/pair-setup`、`/pair-verify`、`/auth-setup`、`SETUP`、`RECORD`、`/command`、`/feedback`、`TEARDOWN` |
| 视频 | TCP，流 110（主屏）/ 111（副屏） | 128 字节头：`u32LE bodyLen`，opcode 0 = 加密帧，1 = 明文 avcC/hvcC；AVCC → AnnexB；每个 IDR 前补 SPS/PPS |
| 音频 | UDP RTP，流 100 / 101 / 102 | 按序号重排；AAC-LC 裸帧（宿主补 ADTS）、Opus、LPCM |
| 事件 | TCP `eventPort` | HID 报文；触摸 UUID `2a2a2a2a`、旋钮 `2a2a2a2b`、媒体键 `2a2a2a2c`、电话 `2a2a2a2d` |
| 关键帧恢复 | RTSP `/command` | `{"type":"forceKeyFrame"}`；解码侧有"没有 IDR 不恢复"的门控 |

`/info` 返回二进制 plist，包含 `srcvers=950.7.1`、`protovers=1.1`、`features=0x615653AEE2`、
`flags=0x4`、`model=AllPlay1,1`、`deviceID` = 网卡 MAC、`pi`/`pk`（Ed25519）、主屏 UUID
`b7e6c5a0-1111-4000-8000-000000000001`，以及 HID 声明。缺了 mDNS TXT 字段或 HID 声明，
iPhone 不会连接。

### 5.2 有线 CarPlay

```
USB 枚举
  └─ 配置 6（CarPlay）→ USBMUX 接口 + NCM 控制/数据接口
usbmux v2
  ├─ TCP → lockdown :62078，明文 XML plist，4 字节大端长度前缀
  │    SetValue(UntrustedHostBUID) → GetValue(DevicePublicKey) → Pair
  ├─ StartSession → 同一条连接原地升 TLS
  └─ StartService(com.apple.carkit.service)   [必须保持打开；它返回的端口喂给下一步]
新 TCP 连接
  └─ iAP2 流：裸 FF 55 02 00 EE 10，随后主机每 500 ms 主动发 SYN，
     直到手机应答；链路到 NORMAL 后立刻识别
手机 → 0x4300 CarPlayAvailability
主机 → 0x4301 CarPlayStartSession：每个地址一个独立字符串参数（重复 id 0）、u32 端口、
       deviceIdentifier、Ed25519 公钥、sourceVersion
手机连 fe80::2 → 汇入无线 AirPlay 路径
```

| 坑 | 真机实测行为 |
|---|---|
| 锁屏手机 | 只枚举**一个** USB 配置（`bNumConfigurations <= 1`）。没有 CarPlay 配置通常意味着"解锁"，不是"线坏了" |
| 接口号 | 不要写死。`0x1D01` 里的 `USBHostTransport` 必须等于 USBMUX 接口的 `bInterfaceNumber`（实测 1），否则手机用 `0x1D03` 直接拒绝 |
| 第一步 | 发完标记后主机必须自己发 SYN。等手机先开口会得到一片死寂（实测 260 秒零字节） |
| 识别时限 | 链路到 NORMAL 后立刻识别；静默约 44 秒手机会整个重置 USB |
| NCM 网卡 | 内核会创建它，但**不配** IPv6 地址（`addr_gen_mode=1`）。先从 NetworkManager 手里拿走（`nmcli device set <if> managed no`），`ip link set up`，再 `ip addr add fe80::2/64 dev <if> nodad` |
| 地址绑定 | 链路本地地址 `bind()` 必须带 `sin6_scope_id`，否则 `EINVAL` |
| `0x4301` 地址列表 | 每个地址是**独立的、重复 id 0 的字符串参数**，不是字符串列表 |
| 配对持久化 | 配对记录目前每次运行重建，所以 iOS 每次都会弹"信任此电脑" |

macOS 的控制面是同一套协议，只是跑在系统 `usbmuxd` 隧道上；NCM 网卡是
`AppleUSBNCMData` 创建的 `enX`。

### 5.3 密码学套件

所有原语都直接基于 OpenSSL EVP/BN 实现；不链接任何 AirPlay/iAP2/MFi 协议库。

| 功能 | 构造 | 参数 |
|---|---|---|
| AirPlay pair-setup | SRP-6a | 3072 位群、SHA-512、`g=5`（RFC 5054 素数）、用户名 `Pair-Setup`、PIN `3939`；`K = SHA512(toBytes(S))`（最短字节形式，不补零） |
| AirPlay pair-verify | X25519 + Ed25519 | HKDF-SHA512(`Control-Salt` → `Control-{Write,Read}-Encryption-Key`) |
| MFi-SAP / iAP2 MFi | ECDSA P-256 | 对 `SHA-256(accPub ‖ peerPub)` 签名；输出 64 字节裸 `r‖s`；签名块用 `SHA1("AES-KEY"‖shared)[:16]` / `SHA1("AES-IV"‖shared)[:16]` 做 AES-128-CTR 加密 |
| 控制通道 | ChaCha20-Poly1305 | 帧 `u16LE len ‖ 密文 ‖ 16 字节 tag`；长度头作为 AAD；收发各自独立计数器；nonce = 4 个零字节 ‖ `u64LE` 计数器 |
| 屏幕流 | ChaCha20-Poly1305 | 整个 128 字节头作为 AAD；密钥 = HKDF-SHA512(shared, `DataStream-Salt` + streamConnectionID, `DataStream-Output-Encryption-Key`, 32) |
| 事件通道 | ChaCha20-Poly1305 | HKDF-SHA512(shared, `Events-Salt`, `Events-{Write,Read}-Encryption-Key`, 32) |
| 序列化 | Apple 二进制 plist + TLV8 | `core/bplist`、`core/crypto/tlv8` |

两种身份刻意分开：**AirPlay 身份**是持久化在状态目录里的 Ed25519 密钥对；
**MFi 身份**是 `identity/` 里的 P-256 私钥 + 证书。

### 5.4 有线 Android Auto（协议完整，未上真机）

```
USB → AOAP 控制请求 51/52/53（切进配件模式）
    → AAP VERSION
    → TLS 1.2，记录封装在 0x0003 里
    → 服务发现
    → 通道：control / video / audio / input / sensor
```

`core/projection/androidauto/` 包含 protobuf wire 子集、AV setup、媒体流、PCM 小端 → 大端
转换，以及 `AV_MEDIA_ACK` 流控。`platform/posix/aa_usb_libusb.cpp` 为 Linux 与 macOS
提供 libusb 传输。同一个 USB 口上，有线 CarPlay（NCM/usbmuxd）与 Android Auto（AOAP）
互斥，需要"先探测、再占用、失败回滚"的仲裁。

## 6. 架构与契约

### 6.1 分层规则

依赖只能向内：

```
host/        组合根 —— 窗口、主循环、输入
platform/    适配层 —— POSIX / BlueZ / IOBluetooth / Win32 / JNI
core/port/   纯接口 —— ITcpListener、ITcpStream、IUdpSocket、INetworkInfo、
             IBluetooth、IMdnsAdvertiser、IVideoOutput、IAudioOutput、IByteStream、
             PlatformServices
core/        协议核心 —— RTSP、iAP2、AirPlay 加密、流、HID、后端
```

1. **`core/` 里没有平台头文件。** 不出现 `<sys/socket.h>`、`<SDL.h>`、`<jni.h>`；
   唯一三方依赖是 OpenSSL；`m3_test` 与 `iap2_test` 只链接 `allplay_core`。
2. **平台差异只从 `core/port/*.h` 进入。** 先定义接口，再逐平台实现；不在 core 里写
   `#ifdef`。
3. **宿主是唯一的组合根。** `host/main.cpp`、`jni_bridge.cpp` 与每个 `tools/*.cpp`
   各自装配 `PlatformServices`。
4. **后端之间不互相 include**，媒体面也不搬进 core：后端只交付帧，宿主办解码与渲染。

### 6.2 核心模块

| 模块 | 职责 |
|---|---|
| `projection/backend.h`、`registry.cpp` | 协议无关的生命周期、输入事件、能力集、sink |
| `projection/carplay/airplay/` | RTSP 服务器、`Receiver` 外观、会话状态机、视频/音频/事件流、ControlCipher、`/info` |
| `projection/carplay/iap2/` | iAP2 wire 格式、链路层、无线客户端、有线链路 |
| `projection/carplay/mfi/` | MFi 配件身份（ECDSA P-256 签名器） |
| `projection/carplay/usb/` | usbmux v2、lockdown、配对记录、plist/XML、TLS 流、有线控制 |
| `projection/carplay/bt/` | 蓝牙触发编排 |
| `projection/androidauto/` | AOAP、AAP、TLS、protobuf 子集、后端 |
| `crypto/`、`bplist/` | HKDF、ChaCha20-Poly1305、SRP-6a、X25519、Ed25519、TLV8、AES-CTR；二进制 plist |
| `platform/device_identity`、`platform/pairing_store` | AirPlay Ed25519 身份与配对记录 |
| `port/control.h` | `MediaKey` + 线程安全 `MediaKeyQueue`；平台推入，iAP2 取出 |

### 6.3 线程模型

`core` 会起这些线程，它们回调宿主的方式不能互换：

| 线程 | 回调进宿主 |
|---|---|
| RTSP accept / 每连接处理线程 | SETUP 期间调 `IAudioOutput::configure()`；关键帧恢复时调 `IVideoOutput::reset()` |
| ScreenStream | `IVideoOutput::configure()` 与 `feedAnnexB()` |
| AudioStream | `IAudioOutput::feed()` |
| EventChannel | — |
| 关键帧 / NTP / keep-alive | — |

关键的不对称：**`reset()` 与 `feedAnnexB()` 跑在不同线程**，却操作同一份解码器状态。
平台视频 sink 必须自己加锁；`core` 不加。SDL 只能在主线程调用，所以桌面音频 sink 用
原子 pending 标志把设备开关推迟到主循环。

### 6.4 输出契约

| 接口 | 契约 |
|---|---|
| `IVideoOutput::configure(codec, w, h, codecData)` | `codecData` 是去掉 fourcc 的 avcC/hvcC 记录体（首字节 `0x01`）。可能被反复调用；配置没变时必须跳过重建，否则会出现周期性黑帧。真实分辨率以码流里的 SPS 为准 |
| `IVideoOutput::feedAnnexB()` | 返回 `false` 代价很高：core 会重新武装关键帧门控并立刻向手机要 IDR。只有解码器确实不可用时才返回 false |
| `IVideoOutput::reset()` | 清解码器**和**已排队的帧 |
| `IAudioOutput::configure()` | 每次重建音频流调用一次；≤ 12 字节的配置包要忽略 |
| `IAudioOutput::feed()` | 无 ADTS 的裸 AAC-LC、裸 Opus、裸 LPCM；`sample` 是 RTP 时间戳 |
| `IBluetooth::connectRFCOMM()` | 返回的流**不归调用方所有**，不要 delete |
| `PlatformServices` | 工厂是"每次使用新建一个"；`Receiver` 拷贝整个 bundle，因为 SETUP 可能在 `start()` 返回之后才调 `makeTcpListener` |

### 6.5 后端生命周期

```text
probe() → ProjectionTarget
connect(target, services, sink) → IProjectionSession | nullptr + err
session.start()      // 不阻塞；握手在内部线程跑
  → sink.onState(Connecting / Negotiating / Running / Failed)
session.stop()       // 幂等；返回后不再有回调
```

`sendInput()` 与 `requestKeyFrame()` 可从任意线程调用，实现内部加锁。Sink 回调可能同时
来自视频、音频、状态线程 —— 锁由 sink 自己负责。

## 7. 验证

### 7.1 测试层级

| 层级 | 命令 | 覆盖 | 需要硬件 |
|---|---|---|---|
| L0 | `bash scripts/build.sh` | 所有目标，包括全部自测二进制 | 否 |
| L1 | `./build/m3_test` | SRP 向量、假手机握手、屏幕流加密 | 否 |
| L1 | `./build/iap2_test identity` | iAP2 wire 格式、链路层、MFi 签名 | 否（需 `identity/`） |
| L1 | `./build/aa_wire_test` | AAP 分帧/拼帧、protobuf 子集 | 否 |
| L1 | `./build/wired_probe --selftest` | lockdown plist、证书、TLS 回环 | 否 |
| L2 | `./build/core_selftest` + `bash scripts/m1-check.sh` | RTSP、`/info` bplist 字段、mDNS 广播 | 否 |
| L2 | `./build/allplay_host` + `./build/fake_phone` | 完整 RTSP + 加密视频，端到端 | 否 |
| L2 | `./build/video_probe --h264 x.h264` | 解码 + letterbox 上屏 | 否 |
| L3 | `./build/wired_probe --link --mfi-dir identity` | 真实 USB 控制面 | iPhone + `sudo` |
| L3 | `./build/allplay_host --wired` | 有线 CarPlay 端到端 | iPhone + `sudo` |
| L3 | `./build/allplay_host --bt` | 无线触发端到端 | iPhone + 蓝牙 |

预期标记：`m3_test` / `iap2_test` 打印 `ALL PASSED (0 failures)`；`aa_wire_test` 打印
`aa_wire_test: OK`；`m1-check.sh` 打印 `M1 acceptance: all passed`。

### 7.2 真机验收记录

| 平台 | 路径 | 证据 |
|---|---|---|
| Ubuntu 桌面 | 无线 | v0.3（2026-10-12）：触发 → mDNS → RTSP → 视频 → 触摸 |
| Ubuntu 桌面 | 有线 | v0.4（2026-10-13）：视频；3000 帧上屏；连续 200 秒未掉线 |
| Android 4.4 / API 19（32 位 Amlogic） | 无线 | v0.5（2026-10-14） |
| macOS | 无线 | v0.6（2026-11-05），含手机离开后自动重入 |
| HarmonyOS Mate 60 | 无线 | v0.7 / v0.8（2026-11-05/06）：视频 + 触摸 + AAC-LC 音频 |
| macOS | 有线 | 真实 iPhone，视频 + 音频，经系统 usbmuxd + `AppleUSBNCMData` |

### 7.3 无硬件端到端

`tools/fake_phone` 用真实 socket 与真实加密扮演 iPhone：

```bash
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=60 -t 10 -c:v libx264 \
  -preset veryfast -pix_fmt yuv420p -g 60 -bf 0 -bsf:v h264_mp4toannexb -f h264 out.h264

# 终端 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build/allplay_host \
  --no-avahi --port 7100 --dump /tmp/x.ppm

# 终端 2
./build/fake_phone --h264 out.h264 --port 7100 --fps 60
```

四条断言：fake_phone 打印的触摸坐标与注入值一致；它推的音频帧数与宿主解码帧数一致；
`--dump` 出的 PPM 转 PNG 后是标准彩条且红色确实是红（红蓝互换说明 YUV 平面顺序错了）；
`m3_test` 仍然全绿。

### 7.4 里程碑

| 里程碑 | 范围 | 状态 |
|---|---|---|
| M0 | CMake + SDL 窗口/状态栏 + 脚本 | ✅ |
| M1 | RTSP + `/info`（bplist）+ `_airplay._tcp` | ✅ |
| M2 | 蓝牙 iAP2 触发（识别 / MFi / 订阅 / `0x5703` / `0x4301`） | ✅ |
| M3 | 会话 + 视频：pair-setup/verify、auth-setup、SETUP、ControlCipher、屏幕流 | ✅ |
| M4 | 有线 USB：libusb 切配置 + usbmux/lockdown/TLS + 内核 `cdc_ncm` | ✅ |
| M5 | 状态栏、重连、关键帧恢复、看门狗 | 部分 |
| M6 | 原生状态页 / 叠加层 | 未开始 |
| M7 | 后端外观 + CarPlay 后端（v1.1） | ✅ 宿主尚未切到 registry |
| M8 | 有线 Android Auto（v1.1） | 🟡 协议 + 传输完成，待真机 |

### 7.5 明确缺口

- Android Auto **没有接过真机**，也没有宿主 UI。
- Linux 有线音频未验证；macOS 有线验证了音频。
- Windows 宿主是骨架。
- CarLife 与 HiCar 只是接口占位。

## 8. 失败语义与健壮性

- **无线触发**：整链每 3 s 重试，共 `--bt-retry` 次（默认 10）。发出 `0x4301` 后启动
  30 s 看门狗等手机的 AirPlay TCP 连接；超时则放弃本次并重试。
- **心跳**：`0x4301` 之后 iAP2 链路一直保持打开，作为无线心跳。`--bt` 模式下手机离开
  5 s 后自动重新触发。
- **视频恢复**：core 武装关键帧门控、请求 `forceKeyFrame` 并调用 `reset()`；sink 必须
  丢弃排队中的旧帧。
- **音频**：RTP 包按序号重排，迟到的重复包丢弃。HarmonyOS 用 1.5 s 环形缓冲，
  700 ms 恢复水位。
- **端口**：RTSP 从 7000 起被占就 +1；实际端口会写进 mDNS TXT 与 `0x4301`。
- **有线重试**：按 `W` 重新跑 USB 准备；错过"信任此电脑"弹窗后约 40 s USB 链路会被
  重置（日志表现为设备消失，而不是超时）。

## 9. 已知限制与操作陷阱

| 现象 | 原因 | 处理 |
|---|---|---|
| 锁屏 iPhone 只有一个 USB 配置 | iOS 行为 | 解锁手机 |
| 手机完全不连，日志里没有一行 RTSP | mDNS TXT 缺失/不对 | `avahi-browse -t -r -p _airplay._tcp \| grep -i allplay` 应能看到 `deviceid=`、`features=`、`flags=0x4`、`srcvers=`、`pi=` |
| macOS 上 `:7000` 绑不上 | ControlCenter 的 AirPlay 接收器长期占用 | 宿主与 `fake_phone` 都显式用 `--port 7100` |
| 静态页面 fps 掉到 0 | AirPlay 只在画面变化时发帧 | 正常，不是卡死 |
| 每次有线都弹"信任此电脑" | 配对记录每次运行重建 | 已知限制，待配对记录持久化 |
| Linux 有线卡在 NetworkManager | NM 把点对点 NCM 链路当 DHCP | `nmcli device set <if> managed no` |
| 有线与 AA 在同一口都失败 | AOAP 与 NCM/usbmuxd 互斥 | 先探测、只占一个、失败回滚 |
| 黑屏但协议日志正常 | 解码侧问题，或 SETUP 没建视频流 | 先用 `video_probe` 二分；检查 SETUP 是否返回了 `dataPort` |
| 播放时没有声音 | CarPlay 只在真正出声时创建音频流 | 在手机上放点声音 |

## 10. 仓库结构

```
core/                    协议核心（无平台头文件；只用 OpenSSL）
  projection/            backend.h + registry + carplay / androidauto / carlife / hicar
    carplay/             airplay · iap2 · mfi · usb · bt · backend_carplay
  port/                  ITcpListener/ITcpStream · IUdpSocket · IBluetooth ·
                         IMdnsAdvertiser · INetworkInfo · IVideoOutput · IAudioOutput ·
                         IByteStream · PlatformServices · MediaKeyQueue
  crypto/ bplist/        HKDF · ChaCha20-Poly1305 · SRP-6a · X25519 · Ed25519 · TLV8 ·
                         AES-CTR · 二进制 plist
  platform/              device_identity · pairing_store
platform/
  posix/                 net · mdns（avahi/dns_sd）· bt（BlueZ/IOBluetooth）·
                         usb_libusb · usbmuxd_mac · aa_usb_libusb
  windows/               Winsock2 · AF_BTH（骨架）
host/                    桌面组合根（SDL2/ffmpeg）
  bt_task · wired_task · media/{ffmpeg_decoder,sdl_video_sink,sdl_audio_sink}
android/                 Gradle：Kotlin 宿主 + NDK C++ core + JNI
harmony/                 ArkTS 宿主 + NAPI → liballplay_core.so + AVCodec/OHAudio
tools/                   core_selftest · iap2_test · m3_test · aa_wire_test ·
                         fake_phone · video_probe · usb_probe · wired_probe
docs/                    架构 · 协议笔记 · 后端规划 · 图片
scripts/                 bootstrap · build · deploy · m1-check · OHOS OpenSSL
```

`identity/` 已被 gitignore，`*.pk8`、`*.p7b`、`*.pem`、`*.key` 也一并忽略。不要把 MFi
凭证复制进任何仓库；`scripts/deploy.sh` 会把它同步到目标机器，只是因为程序在那边需要它。

## 11. 依赖、许可与归属

| 组件 | 依赖 | 许可 |
|---|---|---|
| `core/` | OpenSSL（libcrypto） | Apache-2.0 |
| 桌面宿主 | SDL2、SDL2_ttf | zlib |
| 桌面宿主 | ffmpeg：libavcodec / libavutil / libswscale | LGPL-2.1+（若以 `--enable-gpl` 构建则为 GPL-2.0+） |
| Linux 适配 | avahi-client、libbluetooth（BlueZ）、dbus-1、libusb-1.0 | LGPL-2.1+ / AFL-2.1 |
| macOS 适配 | IOBluetooth、CoreWLAN、dns_sd、AA 用的 libusb-1.0 | 系统框架 / LGPL-2.1+ |
| Android | MediaCodec、AudioTrack、BluetoothSocket、NsdManager | 系统 API |

刻意不用：`libswresample`、`fdk-aac`、`libavformat`、protobuf 运行时、Boost、AASDK。
Android Auto 的 protobuf 子集与 PCM 转换都是本地实现。

AllPlay 以 **GPL-3.0** 发布，是 CarPlay 接收端 **DiPlay** / **xcertplay**（GPL-3.0）的
C++ 重实现；Android Auto 协议事实参考 **AASDK / OpenAuto**（GPL-3.0），但未复制其源码。
完整许可见 [`LICENSE`](LICENSE)；上游归属与三方许可见
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)；版本历史见
[`CHANGELOG.md`](CHANGELOG.md)。
