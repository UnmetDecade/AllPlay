# 协议笔记（实现依据）

<div align="left"><a href="PROTOCOL-NOTES.md">English</a> · <b>简体中文</b></div>

> **性质**：这是**协议行为事实**的整理（2026-10-10 核查得出），不是代码。
> 本项目的实现是**基于 DiPlay / xcertplay（GPL-3.0）的 C++ 重新实现**，逐条对照这里的格式/流程，并在真机上验证。
> 每条都带"为什么要这样"的语境；实现时如与真机行为冲突，**以真机为准并回来改这份文档**。

## 0. 三条链路总览

```
iPhone ──(1) 蓝牙 RFCOMM / iAP2 bootstrap（触发+MFi+推配置）──▶ 我们
       └─(2) 同一局域网：AirPlay 会话（RTSP + 屏幕流 TCP）────▶ 我们
       ◀─(3) 事件通道：HID/触摸/命令（加密，POST /command）──  我们
```

**有线（USB）是另一条独立的接入路径**，走到 (2)(3) 之后与上面完全汇合：

```
iPhone ──(0) USB：切进 CarPlay 配置，打开 USBMUX 口 ─────────▶ 我们
       ──(1') usbmux v2 → lockdown 配对 → 原地 TLS → carkit 服务流
       ──(2') 该服务流上跑 iAP2：主机主动 marker+SYN → 识别 → MFi → 电源/订阅
       ◀──(3') 0x4300 CarPlayAvailability ──────────────────── 我们
       ──▶(4') 0x4301 CarPlayStartSession（报本端 IPv6 链路本地 + 端口）
       ──(5') 手机照该地址连过来 → 汇入 (2)(3)
```

差别全在"谁先开口"和"地址谁定"：无线是**我们广播、手机找过来**；有线是**手机问、我们答**，
地址由我们钉在内核 NCM 网卡上再报给它。详见 [§10](#10-有线usb接入2026-10-13)。

---

## 1. 蓝牙 / iAP2 触发（M2）

### 1.1 前置
- iPhone 需**先与本机蓝牙配对**（bonded）。参照行为：只挑已配对的名字含 "iPhone" 的设备，优先"已连接"的。
- 连的是**手机上的 SPP 服务**（我方是 RFCOMM 客户端）：
  UUID `00000000-deca-fade-deca-deafdecacafe`。
- 备选（若真机验证走这条）：我方注册 SDP 记录 "Wireless iAPv2" 等服务端连接。
  **两条路都留实现余地，真机定夺。**

### 1.2 iAP2 链路层
- 起始 marker：`FF 55 02 00 EE 10`（6 字节，链路同步用）。
- 数据帧 9 字节头：`0xFF5A | u16 长度 | u8 控制 | u8 seq | u8 ack | u8 sessionId | u8 checksum`；
  **checksum = 帧内所有字节(含自身)之和 ≡ 0 (mod 256)**。
- 控制位：SYN 0x80 / ACK 0x40 / EAK 0x20 / RST 0x10；session：10=控制、11=EA、12=文件传输。
- 同步载荷 v1：maxOutgoing/maxLength/重传超时/最大重传与 ACK 数/session 描述表；初始 seq=99；重传 4s、最多 4 次；滑动窗口。

### 1.3 CSM 消息层
- 帧：`0x4040 | u16 长度 | u16 msgId | 参数列表`（大端，无 checksum）。
- 参数：`u16 长度 | u16 paramId | 载荷`；按对端 session-10 最大载荷分片。

### 1.4 消息流程（无线）
1. **识别**：`0x1D00 StartIdentification` → `0x1D01 IdentificationInformation` → `0x1D02 accepted`。
   识别信息里含：我方 EA 协议名、蓝牙链路角色（BOOTSTRAP）、消息列表（我们发的/收的/订阅的）、
   **群组 17 "blue" 用本机蓝牙 MAC**、群组 24 SSID 等。
2. **MFi 认证**：`0xAA00 RequestCertificate` → `0xAA01 (证书)` → `0xAA02 challenge` → `0xAA03 (签名)` → `0xAA04 失败 / 0xAA05 成功`。
   签名与证书由 MFi 配件身份提供（见 §5.4）。
3. **订阅**：0x4157 / 0x5200 / 0xAE00 / 0x5000 / 0x4154（各有语义，参照清单）。
4. 收到手机 **`0x5702 RequestAccessoryWiFiConfiguration`** → 回 **`0x5703 AccessoryWiFiConfiguration`**：
   `ssid / passphrase / securityType / channel / [bssid 提示]`（同一局域网模式：填**现有**网络信息）。
5. 收到手机 **`0x4300 CarPlayAvailability`** → 发 **`0x4301 CarPlayStartSession`**：
   - group 1（无线）：`ssid / passphrase / channel / ipAddresses[] / securityType`
   - base 参数：`airPlayPort`(u32)、`deviceIdentifier`、`publicKey`(Ed25519 公钥)、`sourceVersion`
   - **发送后起 30s watchdog：等 AirPlay TCP 连入；超时则结束本次尝试并重试。**
6. 收到 `0x4E0E DeviceTransportIdentifier` → **重发一次 0x5703**（走"transport 后"路径）；跟踪 `0x4E0D WirelessCarPlayUpdate`。

---

## 2. Bonjour（M1）

- **广播** `_airplay._tcp`（用系统 Avahi），TXT：
  `deviceid`(本机网卡 MAC)、`features`(CarPlay 特性位，含 CarPlayControl)、`flags=0x4`、
  `model`、`srcvers`、`protovers=1.1`、`pi`(配对 UUID)、`pk`(Ed25519 公钥 hex)。
- **发现** 手机的 `_carplay-ctrl._tcp`，解析出地址后探测：
  `GET /ctrl-int/1/connect`，带请求头 `AirPlay-Receiver-Device-ID: <deviceid>`；
  重试 7 次、间隔 1.5s、连接/读超时 3s。**这个"戳一下"会促使手机发起 RTSP 连接。**

---

## 3. AirPlay 会话（M3）

### 3.1 端点（RTSP over TCP，默认 :7000，被占自动换；**实际端口必须出现在 Bonjour TXT 与 0x4301 里**）
| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/info` | 返回 bplist：sourceVersion、features、statusFlags=4、model/manufacturer、deviceID、pi/pk、displays（主屏 UUID 等）、HID 设备声明 |
| POST | `/auth-setup` | MFi-SAP（见 §5.4）；`encryptionType=MFiSAPv1` |
| POST | `/pair-setup` | SRP-6a（见 §5.1） |
| POST | `/pair-verify` | X25519+Ed25519（见 §5.2） |
| SETUP | — | 第一批（无 streams）：手机给 timingPort/macAddress 等；**我们回 eventPort / timingPort**；第二批 streams[]：**110 主屏 / 111 副屏** → 回 `{type, dataPort}`；100/101/102 音频（解码播放）；130 数据（iAP 隧道，可后置） |
| RECORD | — | 会话活跃（"视频要来了"的里程碑点） |
| POST | `/command` | 见 §6；`forceKeyFrame` 用于关键帧恢复 |
| POST | `/feedback` | 每流 streamConnectionID/timestamp/sampleTime（NTP 同步时间） |
| TEARDOWN | — | 收尾 |

### 3.2 ControlCipher（pair-verify 完成后**所有控制流量加密**）
- 帧 = `[2 字节 LE 密文长][密文][16 字节 tag]`；**2 字节长度头作为 AAD**；双向独立计数器。
- ChaCha20-Poly1305；**nonce = 4 字节零 + 8 字节 LE 计数器**。

---

## 4. 屏幕流（M3）

- 独立 TCP 连接（端口在 SETUP 响应里给手机）。
- **128 字节头**：offset 0 = `u32LE bodyLen`；offset 4 = opcode：
  - `0` = 加密视频帧；`1` = 明文配置帧（avcC/hvcC，按 fourcc 识别）。
- 帧体解密：ChaCha20-Poly1305，**128 字节头整个作为 AAD**，nonce 规则同 §3.2 计数器。
- 密钥：`HKDF-SHA512(sharedSecret, "DataStream-Salt" + streamConnectionID, "DataStream-Output-Encryption-Key", 32)`。
- 明文 = **4 字节大端长度前缀的 NALU（AVCC）** → 转 AnnexB（每个 NAL 要校验合法性）；
  首个 I 帧（nalType 5/6）前补 SPS/PPS。
- 关键帧恢复：需要时 `POST /command {"type":"forceKeyFrame"}`（副屏带其 UUID）；解码器侧做"没关键帧不恢复"的闸门。
- 时间戳：头里带 displayTicks（NTP 同步域）；先简化"来一帧显示一帧"，M4 再做对齐/丢帧。

---

## 5. 密码学（全部用 OpenSSL 实现）

### 5.1 pair-setup（SRP-6a）
- 3072 位、SHA-512、g=5（RFC 5054 素数）、用户名 `Pair-Setup`、**PIN 3939**。
- **精确公式**（参照 AirPlay 协议逐字节核对，实现于 core/crypto/srp6a.cpp）：
  - `k = SHA512(toBytes(N) ‖ pad384(g))`（首参是 **toBytes(N)**，不是 pad(N)）。
  - `x = SHA512(salt(16) ‖ SHA512("Pair-Setup:3939"))`；`v = g^x mod N`；`b` 32 字节随机。
  - `B = (k*v + g^b) mod N`；`u = SHA512(pad384(A) ‖ pad384(B))`（先验 `A mod N != 0`）。
  - **`K = SHA512(toBytes(S))`（最短字节，不补零、不做 RFC5054 交错哈希）**——最易踩的坑。
  - `M1 = SHA512((H(N)⊕H(g)) ‖ SHA512("Pair-Setup") ‖ salt ‖ pad384(A) ‖ pad384(B) ‖ K)`；
    `M2 = SHA512(pad384(A) ‖ M1 ‖ K)`。
- 消息体 TLV8（type 0xff 续段）；HKDF 标签 `Pair-Setup-Encrypt-Salt` / `Pair-Setup-Encrypt-Info`；
  nonce `PS-Msg05` / `PS-Msg06`；存对端 **Ed25519 长期公钥**。

### 5.2 pair-verify
- 临时 X25519 + Ed25519 签名；标签 `Pair-Verify-Encrypt-Salt/Info`；nonce `PV-Msg02/03`。
- HKDF-SHA512(sharedEcdh, `Control-Salt`, `Control-Write-Encryption-Key` / `Control-Read-Encryption-Key`) → §3.2 的密钥。

### 5.3 ChaCha20-Poly1305
- 128 位 tag；nonce64 = 4 零字节 + 8 字节 LE 计数器（每方向独立自增）。

### 5.4 MFi-SAP（`/auth-setup` 与 iAP2 共用身份）
- MFi 配件身份：EC P-256 私钥（PKCS#8）+ 配件证书（PKCS#7，**已验证 607 字节 < 640**），由外部提供、不入库。
- 签名：对 **32 字节摘要** 做 ECDSA（不再哈希），输出 **64 字节 raw r‖s**（**不是 DER**）。
  - 摘要构成（协议 MFi-SAP）：`digest = SHA256(accPub(32) ‖ peerPub(32))`；
    **我们的身份是 `LocalMfiAuthenticationClient`（protocolMajor=3）→ 走 SHA256 分支**；
    SHA1 分支只属于硬件协处理器（protocolMajor==2）。BAA 证书分支直接签 `pub‖peerPub`（不哈希），本机不涉及。
- `/auth-setup` 流程：请求体 `[u8 version=1][32 字节对端 X25519 公钥]`；
  我方生成 X25519 私钥 → `shared` → `aesKey=SHA1("AES-KEY"‖shared)[:16]`、`aesIv=SHA1("AES-IV"‖shared)[:16]`；
  响应 `[32 我方公钥][u32BE 证书长][证书][u32BE 签名长][AES-128-CTR(签名)]`。
- iAP2 侧（§1.4 第 2 步）用同一证书+签名器。

---

## 6. 事件通道 / HID 回传（安卓已接）

- SETUP 响应里的 `eventPort` 是 ServerSocket；手机连入后：
  `HKDF-SHA512(shared, "Events-Salt", "Events-Write-Encryption-Key"/"Events-Read-Encryption-Key", 32)` 加密（§3.2 帧格式）。
- 触摸：`POST /command`，bplist：`{"type":"hidSendReport","uuid":"2a2a2a2a","hidReport":…}`；
  触点报告：2 触点 × 6 字节（slot、down、x u16le、y u16le）；坐标按显示像素归一。
- 其他：旋钮 uuid `2a2a2a2b`、媒体键 `2a2a2a2c`、电话 `2a2a2a2d`；`setNightMode`、`requestSiri`。

---

## 7. /info 要点（参照值、真机再校）

- `features` 参照协议的无线值（含 CarPlay 特性位；实现时以协议记录的值起步再按真机调整）。
- `flags=4`；`model`/`manufacturer` 自选；`statusFlags`；`deviceID` = 网卡 MAC（与 Bonjour TXT 一致）。
- displays：主屏 `MAIN_UUID = b7e6c5a0-1111-4000-8000-000000000001`（副屏 …0002，先不做）。
- HID 声明：触摸（+旋钮/媒体/电话可选）；描述符按协议的形态。

## 8. 待真机确认项
1. RFCOMM 方向（客户端连手机 vs 服务端等连接）。
2. features 位与 /info 具体值在现行 iOS 上的效果。
3. NTP：先桩（不响应 NTP 请求是否被接受），失败再加最小 NTP 响应端。
4. 同网模式下 IPv6/IPv4 地址选择（优先 IPv4 起步）。

## 9. 来源与边界
- 以上为**协议事实**整理（核查得出，2026-10-10）。
- 本项目实现为**基于 DiPlay / xcertplay（GPL-3.0）的 C++ 重新实现**；依赖只用系统库。

---

## 10. 有线（USB）接入（2026-10-13）

对照真机逐段验证得出，全部在 Linux + iPhone 上跑通（真机 3000 帧上屏、200 秒不掉线）。

### 10.1 前置（Linux，缺一不可）

- `sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd` —— USBMUX 口只能有一个主人。
- `pkill -f gvfsd-gphoto2` —— 不然 gvfs 会抢着去挂载照片。
- blacklist `apple_mfi_fastcharge`（`/etc/modprobe.d/`）—— 这个内核模块会把设备认走。
- `/dev/bus/usb/*` 是 root 独占，**整个程序 `sudo` 跑**。`sudo` 会重置环境：
  日志要 `sudo env ALLPLAY_LOG=…`，状态目录要显式给（否则 `HOME=/root`，本机身份另起一份，
  手机每次都要重新配对）。

### 10.2 USB 侧：切进 CarPlay 配置

- iPhone 正常枚举出**一个**配置（PTP）。**解锁后**才报出 CarPlay 那个配置（真机实测 value=6）。
  → 判据是 `bNumConfigurations <= 1`，**"这台手机没有 CarPlay 配置"多半是锁屏，不是线的问题**。
- 该配置里认两个接口：**USBMUX 口**（iAP2 的载体，bulk in/out 包成 `IByteStream`）与
  **NCM 控制/数据口**（交还给内核，别自己 detach）。接口号从描述符里读，不要写死。
- 切换配置后设备会重枚举（bus/addr 变化），要等它回来再打开。

### 10.3 控制面：三条迷你 TCP 连接

手机那侧实现了一个 TCP 栈，所以成帧得自己来（`usbmux v2`：版本协商 + setup + 连接请求）。

1. **明文 lockdown `:62078`** —— `SetValue(UntrustedHostBUID)` → `GetValue(DevicePublicKey)`
   → `GetValue(WiFiAddress)` → `Pair`（**明文，不是 SRP**；SRP 是 AirPlay 那边的 pair-setup）。
   报文是 **XML plist**（不是本项目 `core/bplist` 那套二进制），前面加 4 字节大端长度。配完即关。
2. **同一条连接原地升 TLS** —— `StartSession` → 升级 → `StartService(com.apple.carkit.service)`。
   这条**要一直开着**，它一断第三条也就废了。客户端身份是 **root 证书**，不是 host 那张。
3. `StartService` 返回的端口 —— **新的连接**，可选再做一次 TLS。**这就是 iAP2 那条流。**
   流上第一个字节就是 iAP2 链路帧，中间没有任何额外握手或封装。

配对记录（HostID / SystemBUID / 证书）**目前每跑一次现造**，所以每次都要在手机上点一次"信任"；
落一份 pair record 可以免掉。

### 10.4 iAP2 链路：有线由**主机**发起

`FF 55 02 00 EE 10`（6 字节裸 marker，**前面没有任何长度或封装**）之后：

- **必须立刻自己发 SYN，并每 500ms 重发**，直到收到手机的 SYN。
  只发 marker 的话手机**一声不吭**（真机实测干等 260 秒零字节）—— 无线那条路是手机先发 marker、
  我们才进协商，照抄必死。
- 链路参数（照 DiPlay 的 `WIRED_LINK_CONFIG`）：`maxOutgoing=4`、控制会话版本 2、**声明零确认**
  （USB 这条道本身可靠）。

识别 `0x1D01` 里的 **USBHostTransport 接口号**必须是 USBMUX 那个口的 `bInterfaceNumber`
（真机 = 1），报错手机会直接拒（`0x1D03`）。链路 NORMAL 后要**立刻**做识别 —— 干等约 44 秒
手机会把 USB 整个重置。

### 10.5 媒体面：内核 `cdc_ncm`，不是我们

切进 CarPlay 配置后内核自己绑 CDC-NCM 功能并建出一块真网卡（实测 `cdc_ncm 1-3:6.3 usb0: register …`）。
**NTB16 / 以太网 / IPv6 全不用写**，AirPlay 直接绑上去即可。两个坑：

- 内核在这块网卡上**不生成任何 IPv6 地址**（`addr_gen_mode=1`），要自己钉；而
  NetworkManager 会把这根点对点链路当"有线连接"去要 DHCP（没有服务器），一直卡 connecting。
  → `nmcli device set <if> managed no` → `ip link set up` → `ip addr add fe80::2/64 … nodad`。
- 网卡名由 MAC 来（`enx…`），**不能写死**。按 USB `busnum`/`devnum` 反查
  `/sys/class/net/*/device/../{busnum,devnum}`。
- 链路本地地址 `bind()` **必须带 `sin6_scope_id`**，否则 EINVAL（报得像参数写错了）。

### 10.6 `0x4300` / `0x4301`

- `0x4300 CarPlayAvailability`：`group(0)`=有线、`group(1)`=无线、`group(2)`=themeAssets；
  每组内 `u8(0)=可用否`、`str(1)=标识`。真机报有线可用（标识=手机 UDID）且无线可用（标识=Wi-Fi MAC）。
- `0x4301 CarPlayStartSession`：`group(0)` 有线 —— 里面**每个地址一个 string 参数、重复 id 0**
  （**不是字符串列表**，这里最容易搭错）；随后 `u32(2)=AirPlay 端口`、可选 `str(3)=deviceIdentifier`、
  `str(4)=Ed25519 公钥 hex`、`str(5)=sourceVersion`。
- 公钥必须是**持久化的那把**（`core::DeviceIdentity`）—— 手机后面 pair-verify 认的就是它。
- 收到 `0x4300` 就回 `0x4301`，**不按 `available` 过滤**（DiPlay 也是收到就回，可不可用让手机自己判断）。
- 报的地址**必须是 Receiver 实际绑定监听的那个**，端口同理。说一套自己监听另一套，
  手机连上来第一件事就是对不上。

### 10.7 一个"看着像网络故障"的坑

控制面全绿、`/pair-verify` 过了、`RECORD` 了，然后手机每 2 秒 POST 一次 `/feedback`，
**永远等不到画面**。根因不在网络：`SETUP` 里手机要屏幕流，我们没给 `dataPort`
（`IVideoOutput` 为空时不建流），手机拿不到数据端口只能干等。
