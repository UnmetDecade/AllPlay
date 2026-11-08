# 更新日志

版本号对应**平台能力**：每个版本打通一个平台的端到端渲染。
协议核心（`core/`）是所有平台共用的那一份，不在版本之间分叉。

格式参照 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

---

## [v1.1] — 多协议后端骨架 · 2026-11-08

把 `core/` 拆成"协议后端"结构：CarPlay 成为第一个后端，Android Auto 有线模块落地，
CarLife / HiCar 先占接口。宿主行为不变（仍走原路径），桌面构建与现有自测全绿。

### 新增

- **协议后端接口**（`core/projection/backend.h`）：`IProjectionBackend` /
  `IProjectionSession` / `IProjectionSink` / `BackendRegistry`。宿主只认协议中立的
  目标、输入事件与媒体配置，不再直接碰 iAP2 / RTSP；迁移方案见
  `docs/projection-backend.md`。
- **CarPlay 后端**（`core/projection/carplay/backend_carplay.*`）：无线（bt_trigger +
  Receiver）与有线（usbmux / WiredControl / iAP2）两种会话都包进同一接口；平台能力经
  `WiredPorts` 注入，core 里没有平台头。
- **Android Auto 有线**（`core/projection/androidauto/`）：AOAP 51/52/53 → AAP VERSION →
  TLS 1.2（记录装在 0x0003）→ 服务发现 → 通道 / AV SETUP / 媒体流。自带 protobuf wire
  子集与 Annex-B → avcC 合成，PCM 小端转大端后交给宿主；实现 AV_MEDIA_ACK 流控。
  `platform/posix/aa_usb_libusb.cpp` 提供 Linux/macOS 共用的 libusb 传输层。
- **CarLife / HiCar 占位后端**：接口齐全，`probe()` 返回空、`connect()` 报未实现。
- **自测** `tools/aa_wire_test.cpp`：AAP 分帧/拼帧、6 字节长度字段、半帧、protobuf 编解码。

### 变更

- `core/airplay` / `core/iap2` / `core/mfi` / `core/usb` / `core/bt` 整体迁到
  `core/projection/carplay/`（纯 `git mv` + include 路径兼容），协议代码零改动。
- 鸿蒙与 Android 的构建文件跟着新路径更新。

### 已知限制

- 宿主仍走原来的 `Receiver` / `WiredTask` / `BtTask`，尚未切到 `BackendRegistry`；
  Android Auto 也还没有 UI 入口。
- Android Auto 只完成协议与传输层实现，**未上真机验证**；无线 AA、CarLife、HiCar 未实现。

---

## [v1.0] — 真机稳定性 · 2026-11-06

iPhone SE2 / 16 连续上手用下来修的一批问题。

### 修复

- **一直滑动会卡死**（`core/airplay/event_channel.cpp`）：触摸原来是 UI 线程直接
  加密 + `send()`，手机忙着重绘时发送缓冲写满，UI 线程就卡在系统调用里；这条通道的
  读线程（要给手机回 ACK）也被同一把锁挡着。现在拆出独立写线程，调用方只入队；
  触摸按"位置快照"处理，新的直接顶掉还没发出去的旧的。
- **开流那一下的噪音**：手机开流时先囤约两秒再一口气吐过来，把 1s 的环形池撑爆后，
  旧逻辑"超出多少丢多少"，连续几十个 9~21ms 的断口连成一片。改成一次切到 70%，
  一个断口代替几十个。
- **开流起手的卡顿**：头一拍水位从 250ms 提到 800ms，开声后再降回续播水位，
  扛住手机开流时的第一次停顿。
- **听歌时偶尔卡一下（没有任何操作）**：手机送音频是**成团**的——实测连续两包之间
  能空 569ms，RTP 序号连着、一个包都没丢，只是没发。水位低于这个空档就会每来一波
  抽干一次。环形池 1s→1.5s、续播水位 250ms→700ms（569ms + 余量），按实测空档定，
  不再拍脑袋定。
- **0x4301 之后手机不拨号**：iOS 有概率不主动连 AirPlay 端口（实测空了 14.5 秒，
  黑屏那几次干脆一直没连），只有人工关开一次 Wi-Fi 才会重连。现在 3 秒没等到连接
  就重发 `0x5703 + 0x4301`，最多催两次。

### 新增

- **Wi-Fi 加密方式可选**（设置页）：无密码 / WEP / WPA2 / WPA3，`0x5703` 与
  `0x4301` 里的 `securityType` 跟着走（以前写死 2）；开放网络不再要求填密码。

### 诊断

- 音频侧补齐了四路信号：收包空档（>80ms，带 RTP 序号差）、环形池溢出、
  **每一次**欠载、声卡回调停摆（>300ms）与系统打断/报错。

---

## [v0.9] — 方控接口 · 2026-11-06

`core` 里立起方控（方向盘媒体键）这一层：上层把按键放进队列，无线/有线两条 iAP2 链路
各自编码成 HID 报表发给手机。接口与编码已自测通过；`0x4E0x` 消息号、报表格式和按键来源
都还等真机与各平台去接。

### 新增

- **方控接口**（`core/port/control.h`）：`MediaKey`（播放/暂停、上一首/下一首、Siri、
  接听/挂断…）+ 线程安全的 `MediaKeyQueue` + `IMediaKeySink`。方向是**车机 → 手机**，
  与 `media.h` 相反。按键从哪来（界面按钮、系统媒体键、将来的硬件驱动）由上层实现。
- **HID 编码**（`core/iap2/hid.h`）：`MediaKey` → HID usage（消费者页 0x0C / 电话页 0x0B）
  → 报表字节 → `0x4E02` 报文。无线的 `WirelessTrigger` 在 `run()` 里排干队列，
  有线的 `WiredLink` 提供 `sendMediaKey` / `pumpMediaKeys` 由调用方在 pump 之间调用。
  **`0x4E0x` 这一族消息号与报表格式尚未在真机上核验**，接真机时用抓包对一遍（要改只有一处）。
- 自测补 ①b 段：usage 映射、按下/抬起报表、报文往返、队列 FIFO 与溢出。

---

## [v0.8] — 鸿蒙音频出声 · 2026-11-06

Mate 60 上 CarPlay 的声音出来了：手机建的音频流（type=102、AAC-LC、48kHz 立体声、裸帧）
在本机解码后由系统声卡播放。视频、触摸、音频三条流第一次同时跑通。

### 新增

- **音频输出**（`harmony/entry/src/main/cpp/harmony_audio.cpp`）：`core::IAudioOutput` 的鸿蒙实现。
  解码用 `OH_AudioCodec` 的**同步模式**（`OH_MD_KEY_ENABLE_SYNC_MODE=1`，默认的异步模式下
  `QueryInputBuffer` 一律返回 `AV_ERR_OPERATE_NOT_PERMIT`），播放用 `OHAudio` 渲染器的
  `OnWriteData` 回调。解码在两把锁的里侧、播放回调只碰环形池，10ms 的解码查询永远堵不到声卡。

### 修复

- **裸 AAC 帧必须补 ADTS 头**：`OH_MD_KEY_AAC_IS_ADTS=0` 是 LATM，喂裸帧时解码器只吃不吐
  （实测 3000 包解出 0 帧）。改成每帧前补 7 字节 ADTS 头、该键置 1，48 帧解出 47 帧；
  带外 ASC（`OH_MD_KEY_CODEC_CONFIG`）那条路在同一台机上解出 0 帧。
- **"先接住不播"变成能播**：此前 `feed()` 是空实现，现在整条链路走通。
- **卡顿三连**：回调块从 93ms 钉小又放开、续播水位低于一次回调、`QueryOutputBuffer` 用 0 超时
  导致 PCM 积在解码器里——三处合起来表现为持续爆点（600 次回调欠载 193 次）。
  现在环形池 700ms + 续播水位 250ms + 2ms 输出轮询，实测欠载降到 0～3/600。
- **滑动时偶发卡顿**是发送端的问题：同一台 Mate 60 换 iPhone 16 作为发送端后不再复现，
  iPhone 13 mini 在滑动时自身掉帧。

### 已知限制

- **iOS 13 及更老系统连不上**：iPhone SE2（iOS 13）在 0x1D01 识别这一步被回 0x1D03，
  报的参数 id 含 6/7/10。全量消息清单、只报 CarPlay 必需的最小集、完全不报清单三组都试过，
  连报空了也被拒——是手机侧不接受，不是报文格式问题（同样的字节 iOS 16/18 收）。
  iOS 13 的 SE2 升级系统后可正常连；代码里保留了"被拒就逐级收窄重试"的兜底。

## [v0.7] — HarmonyOS 无线真机端到端 · 2026-11-05

鸿蒙（**Mate 60**）上也走通了：蓝牙 iAP2 把 iPhone 叫醒 → 同网 Wi-Fi → RTSP 协商 →
**硬解上屏、横屏铺满、触摸可点**。协议核心一行没分叉，`core/airplay` 原样经 NAPI 编进 HAP。

### 新增

- **鸿蒙平台**：ArkTS + hvigor 工程（`harmony/`）。蓝牙 SPP（`socket.sppConnect`）、mDNS 广播、
  屏幕尺寸走系统 API；协议全部在 `core/`，由 `liballplay_core.so` 经 NAPI 桥接。
- **AVCodec 硬解上屏**（`harmony_decoder.cpp`）：`OH_VideoDecoder_CreateByMime` + 回调式输入输出，
  `RenderOutputBuffer` 直接画到 `XComponent` 的 surface（surface id → `OHNativeWindow`）。
- **全屏**：`/info` 里协商的显示分辨率改成**按屏幕长宽比**（1080p 档、宽封 1920，实测 1920×868），
  iPhone 会按这个尺寸重排 CarPlay 界面；会话进来转横屏，断开转回竖屏。
- **触摸回传**：`XComponent.onTouch` → 归一化坐标 → 事件通道 HID `touchReport`（双触点）。
- **构建脚本**：`scripts/build-ohos-openssl.sh`（交叉编译 OHOS OpenSSL）、
  `scripts/sync-harmony-identity.sh`（把 `identity/` 临时拷进 HAP rawfile，仍在 `.gitignore`）。

### 修复

- **avcC 的 SPS/PPS 必须 in-band**：`OH_MD_KEY_CODEC_CONFIG` 在这台机的解码器上被忽略
  （系统日志 `pps is null` / `EVNT_PARAMETER_SETS_LOST`），改成每个 IDR 前补一份 AnnexB。
- **手机每 ~5 秒重发同一份 avcC**：原先每次重建解码器，参考链清空、画面周期性卡住；
  改成按 `csd + 尺寸 + 编码类型` 比对，相同就跳过重建。
- **横竖屏切换后画面停住**：转屏会重建 `XComponent`，旧的 `OHNativeWindow` 指向已回收的 surface。
  surface id 变化时拆掉重建，并用上次的 csd 立刻恢复，不等手机重发配置。

### 已知限制

- **音频只接住不播**：`IAudioOutput` 收下数据后丢弃，鸿蒙侧还没接 AudioRenderer。
- **流式播放时没有退出入口**：画面铺满整块屏，启动页上的"取消"被盖住，只能从手机或系统侧退出。
- 设备选择与热点 SSID/密码未持久化，应用重启后要重新选一次。

---

## [v0.6] — macOS 无线真机端到端 · 2026-11-05

真 iPhone 在 Mac 上走**蓝牙 iAP2 → 同网 Wi-Fi 投屏**跑通，并把"车机该有的常驻行为"补齐：
手机离开自动回启动页，回来自动重新进 CarPlay，全程不用碰宿主。启动页整个重做。

### 新增

- **启动页重做**（`host/launcher`）：按 1540×850 设计稿坐标绘制后整体缩放，线性采样让圆角/圆弧
  不再出台阶；显示**蓝牙适配器名**（手机蓝牙列表里看到的那个，不是 Bonjour 名）、中/EN 语言切换、
  连接状态与等待动效。
- **高 DPI 渲染**：`SDL_WINDOW_ALLOW_HIGHDPI`，渲染面按物理像素（Retina 2×）出图；同时补上
  窗口"点"与渲染"像素"的换算 —— 否则启动页鼠标命中与触摸坐标都会偏（实测 Mac 上触摸 y 轴偏上）。
- **双通道 `--dual`**：有线（USB）+ 无线**同时**监听，谁先连上算谁；另一条把后来的连接挡在门外，
  会话结束自动放行。
- **常驻重连**：`--bt` 下手机离开后隔 5 秒自动重新触发，不需要按键，也不需要重启宿主。

### 修复

- **重连后整场 CarPlay 总在 5 秒定时器上被掐断**：事件通道对象跨会话复用（监听端口不变），
  但读写密钥是**每场** pair-verify 出来的。宿主周期发的 `forceKeyFrame` 用旧密钥加密，手机解不开，
  iOS 直接结束会话。新增 `EventChannel::rekey()`，每条会话首次 SETUP 用本场 sharedSecret 重算密钥。
- **会话活到 90 秒自己断**：0x4301 发出后这条 iAP2 链路就是无线 CarPlay 的心跳线，iOS 一直在上面
  发消息、配件必须继续读并 ACK。触发窗口（90s）一到就停读，iOS 约 30 秒后判定配件掉线并 TEARDOWN。
  改成发出 0x4301 后长期保持，直到链路真的断。
- **手机回来不会自动进 CarPlay**：常驻心跳让触发线程一直守在旧链路上，永远等不到新的 0x4301。
  增加取消旗标：会话结束（或空等 45 秒）就撤掉这次触发，重排下一次。
- **手机离开后回启动页卡 10 秒**：控制连接空闲判定 10s → 5s，并在日志里打出断线原因
  （读超时 = 手机不再发包 / 读返回断开 = 对端关闭），不再靠猜。

---

## [v0.5] — Android 4.4（API 19）· 2026-10-14

安卓端降到 **Android 4.4（API 19）**，在 32 位 Amlogic S905M 电视盒子上真机跑通
（蓝牙 iAP2 触发 → mDNS 发现 → RTSP 协商 → 720p 投屏 → 音频）。意义：**最弱的安卓盒子也能当 CarPlay 屏。**

### 新增

- **Android 4.4 移植**：`minSdk 28→19`。Kotlin 层把 NotificationChannel / AudioAttributes+AudioFormat.Builder /
  MediaCodec buffer 访问 / setOutputSurface / startLocalOnlyHotspot / startForegroundService /
  运行时权限 / 电池优化 / 对话框主题 / letterSpacing 等 20 多处 API 26+ 调用全部加 `SDK_INT` 守卫；
  新增 `MediaCodecCompat`（旧数组式 buffer 访问，API 19 用 `getInputBuffers()`）。
- **原生层 API 19 兼容**：`cpp/CMakeLists` 启用 C++17；`net_posix` 用 SIOCGIFCONF 替代
  API 24 才有的 `getifaddrs`；新增 `stdio_compat` 提供 API<23 缺失的 `stdin/stdout/stderr` 符号；
  `log.cpp` 安卓上只走 logcat（不引 stderr）。
- **双 ABI + 降 NDK**：`abiFilters` 加 `armeabi-v7a`（32 位盒子），OpenSSL 按 ABI 分目录交叉编译；
  NDK 降至 r25c（r26 起不支持 API 19）。

### 修复

- **mDNS 在安卓盒子上广播失败**：JmDNS 抢不到 5353（被系统 netd 占用，EADDRINUSE），退回系统 NsdManager。
  API 19 上不设 TXT —— `setTxtRecord(byte[])` 不存在、`setTxtRecord(DnsSdTxtRecord)` 会导致
  NsdServiceInfo Binder 序列化失败；实测 iPhone 靠蓝牙 0x4301 + RTSP `/info` 就能连，TXT 非必需。
- **性能模式改为固定 1280x720@30fps**（原先是"分辨率减半"）。
- **断开后不自动重连**：AirPlay 会话断开后，蓝牙触发改为关旧 RFCOMM 再重发 0x4301
  （iPhone 的 iAP2 SPP 服务只接受一条连接，旧的不关新的连不上）。

### 已知限制

- 手动断开 iPhone 的 WiFi 后，iOS 会主动退出 CarPlay，盒子无法强制重进 —— iOS 行为，
  真实场景（WiFi 因干扰短暂掉线）不受影响。
- 盒子的系统热点（SoftAP）被 Amlogic 框架锁定为"仅系统包可调"（`setWifiApEnabled`
  抛 `SecurityException: Package android does not belong to ...`），App 内反射开热点不可用；
  需同 WiFi 或外部方式开热点。

---

## [v0.4] — 有线（USB）CarPlay · 2026-10-13

**插上数据线就行。** 前几版走的都是无线（蓝牙叫醒 + 同一局域网）。这一版加了**有线**那条路：
USB 控制面整条自己写，媒体面交给内核，最后把手机引到和无线**同一套** RTSP / 加密 / 屏幕流代码上。

![真 iPhone 的有线 CarPlay 界面渲染在 Ubuntu 上](docs/images/carplay-wired.jpg)

（上图：真 iPhone 插着数据线，宿主 `--wired` 跑在 Ubuntu 上 —— 这一轮宿主侧**渲染满 3000 帧**
正常退出，零 WARN/ERR。另有一轮探针侧的 200 秒长跑：收 1320 帧 / 12.4 MB，全程不掉线。）

![注入一次触摸后，CarPlay 跟着进了"外观"页](docs/images/carplay-wired-touch.jpg)

（上图：`--inject-touch 0.5,0.72` 打在"外观"那一行上，手机**真的导航进了这一页** ——
投出去的事件通道 HID 有线这条路上一样被接受。）

### 新增

**有线控制面（`core/usb/`，平台无关）**
- `usbmux` v2：版本协商 + setup + 迷你 TCP（手机那侧是个 TCP 栈，成帧要自己来）。
- `wired_control`：串起手机要的三条连接 —— ① 明文 lockdown 配对 → ② 会话**原地升 TLS** →
  ③ `StartService(carkit)` 拿到的 iAP2 流。三条缺一条都拿不到最后那条服务流。
- `platform/posix/usb_libusb`：只做两件事 —— 把 iPhone 切进 **CarPlay 配置**（锁屏时它只报
  1 个配置，这是 iOS 的行为不是线的问题），以及把 USBMUX 口的 bulk 端点包成 `IByteStream`。

**有线 iAP2（`core/iap2/wired_link`）**
- 链路层**由主机发起**：发完 6 字节 marker 之后不等任何回话，自己立刻发 SYN 并每 500ms 重发。
  只发 marker 的话手机一声不吭（干等 260 秒零字节）—— 这是最早那版 naive 实现的死法。
- `0x4300` 解析 + `0x4301 CarPlayStartSession` 组装。有线组的报文结构容易搭错：
  地址是**每个地址一个 string 参数、重复 id 0**，不是字符串列表。

**有线媒体面**——**自己不写**。切进 CarPlay 配置后内核的 `cdc_ncm` 会建出一块真网卡，
AirPlay 直接绑到那块网卡的 IPv6 链路本地地址上，NTB16 / 以太网 / IPv6 全是内核的事。

**宿主 `--wired`（`host/wired_task`）**：和 `--bt` 对称，但**相位不同** —— 无线是
"Receiver 先起、手机找过来"，有线是"手机从 `0x4301` 里**读**地址"，所以 Receiver 必须等
USB 侧拆到知道 NCM 网卡叫什么、地址钉成哪个之后才能起。任务因此分两段（准备 / 保活），
`rx` 的生命周期仍然只有一个线程在管。准备失败时按 `W` 重来一次，不用重启程序。

**触摸**：有线这条路上触摸走的是**和无线完全相同**的那条事件通道（`POST /command` + 加密 HID），
一行都没改。真机验证：注入一次点击打在"外观"那一行，手机跟着进了该页（见上图）。

**探针**：`tools/usb_probe`（切配置）、`tools/wired_probe`（控制面，含 `--selftest` 离线自检）。

### 修复

下面这些**只有真机才暴露**。

- **`0x4301` 发了，手机却不来连 —— Receiver 绑错地址。** 有线这条路上地址是**我们先定好的**
  （NCM 网卡上的链路本地），`0x4301` 报的必须和 Receiver 实际监听的一模一样。原 `Receiver::start`
  拿"有没有 IPv4"当成功判据，而 NCM 网卡上**根本没有 IPv4**，于是它去绑通配 —— 手机照着
  `0x4301` 里的地址连过来，扑空。新增 `ReceiverConfig::bindAddr`：显式绑定跳过 IPv4 判据。
- **链路本地地址 `bind()` 报 EINVAL。** `fe80::/10` 的地址绑定必须带 `sin6_scope_id`，
  否则内核直接拒绝（而且报的是 EINVAL，看着像参数写错了）。现在 `bindAddr` 支持 `%网卡` 后缀，
  裸 `fe80:` 地址由 `Receiver` 自己补上 `%ifname`。
- **NCM 网卡上一个 IPv6 地址都没有。** 内核在它上面 `addr_gen_mode=1`（不生成任何地址），
  同时 NetworkManager 把这根点对点链路当"有线连接"去要 DHCP（那儿根本没有 DHCP 服务器），
  一直卡在 connecting 并随时可能清掉地址。新增 `allplay::posix::prepareWiredNcm`：
  `nmcli device set … managed no` → `ip link set up` → 钉 `fe80::2/64 … nodad`，
  网卡名按 USB `busnum`/`devnum` 反查 `/sys/class/net/*/device/..`（名字由 MAC 来，不能写死）。
- **控制面全绿、`RECORD` 也过了，然后卡在 `/feedback`** —— 每 2 秒一次，永远等不到画面。
  根因是 `SETUP` 里手机要屏幕流、我们没接 `IVideoOutput` 就没给 `dataPort`，手机拿不到数据端口
  只能干等。宿主本来就接了 sink，所以只有探针中招；已在探针里补一个只点数的 sink。
- **`pkill -f wired_probe` 会把执行它的那个 shell 一起杀掉**（shell 自己的命令行里就含这个串），
  于是"杀掉旧进程再重新编译"那条命令链在第 1 步就断了，后面的编译根本没跑，看起来却像编译失败。

### 注意

- 有线**只在 Linux 上做**：媒体面依赖内核 `cdc_ncm`，macOS / Windows 没有对应物。
- 没有 `libusb-1.0-dev` 时 CMake 会跳过整条有线链路，`--wired` 会明确报"这个构建没带 libusb"。
- 跑之前的三件前置（停 `usbmuxd`、`pkill gvfsd-gphoto2`、blacklist `apple_mfi_fastcharge`）
  和 `sudo` 环境下的两个坑见 [README 的有线前置条件](README.md#有线的前置条件linux缺一不可)。

---

## [v0.3] — 真 iPhone 端到端 · 2026-10-12

**手机是被宿主自己叫来的。** v0.2 证明的是"拿到流能渲染"，这一版证明的是"能主动把真机叫来"：
宿主发起蓝牙 iAP2 触发 → iPhone 经 mDNS 发现本机 → RTSP 协商 → 屏幕流上屏。
**CarPlay 画面出现在 Ubuntu 桌面上，触摸回传可点。**

![真 iPhone 的 CarPlay 界面渲染在 Ubuntu 桌面上](docs/images/carplay-home.jpg)

### 新增

**蓝牙触发（桌面宿主）**
- `host/bt_task`：宿主从"只等手机来连"变成"自己把手机叫起来"。`runBtTrigger` 是全阻塞
  且**没有取消接口**的，所以放独立线程跑；失败按 3 秒间隔整链重试。线程把自己需要的
  transport 与参数全部 move 进闭包，退出时 detach 而非 join —— 否则一次退出最多卡 90 秒。
- 开关一律 opt-in，**不加 `--bt` 时行为与之前完全一致**：
  `--bt` / `--bt-addr` / `--bt-device` / `--bt-mode` / `--ssid` / `--wifi-pass` / `--bt-retry`。
  运行时按 `B` 可重新触发一次。

### 修复

下面这些**只有真机才暴露**，假手机一个都测不出来。

- **mDNS 的 TXT 记录是二进制垃圾 —— "手机根本不连"的根因。**
  `avahi_entry_group_add_service()` 是**可变参数**接口（`...` 是一串以 NULL 结尾的
  `const char*`），原代码传进去的是 `const char**`，avahi 于是把**指针数组自身的内存**
  当成第一个 TXT 字符串读，广播出去只有几个字节的乱码 —— 表象是"手机压根不来"，
  日志里一行 RTSP 都没有。iPhone 靠 TXT 里的 `features` / `flags=0x4` / `deviceid` /
  `srcvers` 认接收端，读不到就不连。改用 `avahi_entry_group_add_service_strlst()` +
  `AvahiStringList`（`avahi_string_list_add` 是头插，要倒着加才能保持原序）。
- **点图标毫无反应，拖地图却正常。** 触摸上报在抬起那一帧只发 `down=false`，
  坐标留在 (0,0) —— 手机收到的是"在图标处按下 → 在左上角抬起"，被判定成滑动而非点击。
  改为**抬起也带最后的位置**，与 Android 侧 `MainActivity.kt` 的 `ACTION_UP` 语义对齐。
- **断开后回不到启动页，画面卡在最后一帧。** "画面停住而不是消失"本来是给会话中途掉帧
  设计的，结果会话结束时纹理里那张末帧被 `present()` 一直重画 —— 日志明明打了"回启动页"，
  屏上却纹丝不动。新增 `SdlVideoSink::dropFrame()`，会话结束时丢掉。
- **开 `--statusbar` 就有黑边，且状态栏根本看不见。** 状态栏画在 `y = screenH`，
  可窗口只有 `screenH` 高 —— 整条落在窗口之外；渲染循环又按 `winH - barH` 排视频，
  16:9 被缩进 1280x694，左右各多出 23px 黑边。窗口高度改为 `screenH + barH`，两个毛病一起消失。
- **屏幕流被跳过时静默失败。** `video_` 为空时 `session.cpp` 原来直接 `continue`：
  不回 `dataPort`，手机干等 10 秒断开，而日志里一点提示都没有（排查时因此绕了很久）。
  补一条 WARN，只记日志、不改行为。

### 文档

- README 更新"当前状态"；新增排障条目 **"手机压根不来连 → 先查 mDNS TXT"**。

---

## [v0.2] — Ubuntu 平台渲染 · 2026-10-12

桌面宿主从"只有窗口"补齐到**能真正渲染出 CarPlay 画面**，并接上 Linux 蓝牙传输层。
与 v0.1 的 Android 端共用同一份 `core/`，只换了 `platform/` 和 `host/`。

### 新增

**视频**
- `host/media/ffmpeg_decoder` + `sdl_video_sink`：ffmpeg 软解 H.264/HEVC → SDL2 纹理，letterbox 上屏。
- 解码在 ScreenStream 线程做（对 TCP 收包形成自然背压），主线程只管抽帧渲染；
  帧队列有界，满时丢最旧帧保低延迟；尺寸/格式变化才重建纹理。

**音频**
- `host/media/sdl_audio_sink`：AAC-LC / Opus / LPCM 三种格式，SDL2 输出。
- **有意不引 libswresample** —— 它整套 channel-layout 参数在 ffmpeg 4.4 与 5.1+ 之间签名不同，
  而这里只需要"平面 float → 交织 s16"这一种转换，自己写反而更省事。
  版本差异全部收敛在 `host/media/ffmpeg_compat.h` 一个文件里。

**界面与输入**
- `host/launcher`：启动页（双色 logo + 蓝牙水纹 + 状态文字），与 Android 启动页对齐；
  连上后切全屏视频，状态栏退化为 `--statusbar` 显式开启的调试叠加。
- 触摸回传：鼠标 / 手指 → 按 **letterbox 后的视频矩形**归一化 → 事件通道。

**蓝牙（Linux）**
- `platform/posix/bt_bluez.cpp`：BlueZ 传输层，客户端模式（我们主动连手机的 iAP2 SPP 服务）。
  链路：SDP 查到 `00000000-deca-fade-deca-deafdecacafe` → **RFCOMM 通道 1** → 连上 →
  iAP2 识别 → 收到手机主动发来的 `0xAA00 请求证书`。
  真机（iPhone）验证到此为止，再往后需要 MFi 身份。

**验收工具**
- `tools/fake_phone`：假 iPhone。真 socket 跑完 RTSP + pair-setup/verify + ControlCipher 加密
  + 加密屏幕流推帧 —— **端到端验收不再需要 iPhone**。
- `tools/video_probe`：本地喂 AnnexB H.264 的解码冒烟工具。黑屏时先跑它二分：
  有画面 = 协议侧的锅，没画面 = 解码侧的锅。
- `tools/phone_crypto.h`：手机侧密码学从 `m3_test` 抽出来两边共用 ——
  抄两份的话它们会一起错，还会让"自测全绿但端到端失败"变成可能。

### 修复

- **SETUP 阶段 SIGSEGV**：`Receiver::start()` 把 `PlatformServices` 的**参数地址**存进了 Session，
  而 Session 到 SETUP 才第一次用它去建数据/事件通道，那时 `start()` 早已返回。改为自己持有一份。
- **关键帧判不出来、一直黑屏**：`ScreenStream::isRandomAccess()` 只看访问单元的第一个 NAL，
  而关键帧 AU 的常见排布是 `[SPS][PPS][IDR]`，首 NAL 是 SPS(7) 就被判成"非关键帧"——
  放行闸门永远打不开。改为扫完整个 AU。真机与假手机都会踩。

### 构建

- CMake 接 ffmpeg（avcodec/avutil/swscale，**不含** swresample）、bluez、dbus-1；
  缺 ffmpeg 时宿主降级为桩而不是整个跳过。
- 新增 `fake_phone` / `video_probe` 两个 target。

---

## [v0.1] — Android 平台渲染 · 2026-10-11

首个版本。协议核心从零实现，并在 Android 上端到端跑通。

### 新增

**协议核心 `core/`**（平台无关 C++，唯一三方依赖 OpenSSL）
- `airplay/rtsp`：极简 RTSP 服务器（控制通道，pair-verify 后加密）
- `airplay/session`：会话状态机 —— pair-setup / pair-verify / auth-setup / SETUP / RECORD
- `airplay/video_stream`：屏幕流 —— 128B 头解密 → AVCC→AnnexB → 关键帧闸门
- `airplay/audio_stream`：音频流 —— RTP over UDP 解密 + 序号乱序重排
- `airplay/event_channel`：事件通道 —— 触摸 HID 回传（最多 2 触点）+ forceKeyFrame 命令
- `airplay/control_cipher`：控制通道帧加密（ChaCha20-Poly1305）
- `iap2/`：iAP2 协议栈 —— 线格式 / 链路层（marker→SYN→ACK→NORMAL）/ 无线控制流程
- `mfi/mfi_identity`：MFi 配件身份接口（证书 + ECDSA 签名），**接口按硬件芯片的形状设计**，
  将来换成鉴权芯片实现时 core 不用动
- `crypto/`：加密原语全部从零实现（hkdf / chacha20poly1305 / srp6a / x25519 / ed25519 / tlv8 / aes_ctr）
- `bplist`：Apple 二进制 plist 编解码
- `bt/bt_trigger`：蓝牙触发编排（选设备 → RFCOMM → iAP2 → 0x4301 引导）

**Android 宿主**
- `MediaCodecSink`：MediaCodec 硬解 → Surface 直出
- `AudioSink`：AudioTrack 播放
- `BluetoothService` / `MdnsService` / `HotspotManager`：蓝牙、mDNS、热点
- 触摸回传、启动页与状态栏
- Gradle + NDK：Kotlin 外壳 + C++ core，经 `jni_bridge.cpp` 接系统 API
- `minSdk 28` / `targetSdk 34` / 仅 `arm64-v8a`

**平台适配**
- `platform/posix`：POSIX socket、avahi（Linux）/ dns_sd（macOS）、IOBluetooth、BlueZ（桩）
- `platform/windows`：Winsock2 + `AF_BTH`（骨架，未接宿主）

**自测**
- `core_selftest`（RTSP + `/info` + mDNS 广播）
- `iap2_test`（iAP2 线格式 + 链路层 + MFi 签名）
- `m3_test`（SRP 对 Python 参考向量 + 假手机全流程 + mock 屏幕流）

---

## 未发布

- **产品化的 MFi 身份**：`0xAA00 请求证书` 那道关要求 Apple MFi 计划签发的配件身份
  （`identity/`，不入库）。开发期用自备身份可以走通全链路 —— v0.3 的真机验证就是这么做的 ——
  但**纯软件身份不是产品路径**，要作为配件出货仍得走 MFi 计划。
- **桌面端音频**：真机那一轮 iPhone 只请求了屏幕流和 `type=130`，**没有请求任何音频流**，
  所以桌面端的音频链路至今只有假手机验证过，真机行为待观察。
