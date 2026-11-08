# Third-Party Notices

## 一、本项目的来源

**AllPlay** 是 **DiPlay** / **xcertplay** 无线 CarPlay 接收端的 **C++ 重新实现**。
本项目的协议行为、流程与 wire 格式参照它们的实现与整理，因此以 **GPL-3.0** 发布并保留上游署名。

### DiPlay — GPL-3.0

- 仓库：https://github.com/shihabal3amri/DiPlay
- 作者：shihabal3amri
- 说明：Android 无线/有线 CarPlay 接收端（public preview）。

### xcertplay — GPL-3.0

- 作者：shilapi
- 说明：DiPlay 的上游项目，CarPlay 接收端协议实现。

### 协议研究参考

- **LIVI**、**Showcase**：AirPlay / iAP2 / CarPlay 协议研究（DiPlay 亦在其署名中致谢）。
- **AASDK**（https://github.com/f1xpl/aasdk，GPL-3.0）与 **OpenAuto**：
  Android Auto 有线链路的协议事实来源（AOAP、AAP 分帧、TLS 1.2、protobuf 字段号）。
  `core/projection/androidauto/` 是照公开协议事实**独立编写**的，没有拷贝 AASDK 源码，
  也不依赖 Boost / protobuf / AASDK 的任何运行时组件；许可仍与 AASDK 兼容（GPL-3.0）。

本项目是**独立实现**：不包含 Apple 的任何代码、文档或二进制。协议事实来自公开的上游实现
与公开资料，整理在 [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md)。

## 二、运行时依赖

原则是**只用系统库**，不引入需要自行构建的第三方。

| 组件 | 依赖 | 许可 | 用在哪 |
|---|---|---|---|
| `core/` | OpenSSL（libcrypto） | Apache-2.0 | 全部加密原语（HKDF / ChaCha20-Poly1305 / SRP-6a / X25519 / Ed25519 / AES-CTR） |
| 桌面宿主 | SDL2 | zlib | 窗口、纹理、音频输出、输入事件 |
| 桌面宿主 | SDL2_ttf | zlib | 状态栏与启动页字体 |
| 桌面宿主 | ffmpeg：libavcodec / libavutil / libswscale | LGPL-2.1+ | H.264/HEVC 软解、AAC/Opus 解码、像素格式回落 |
| Linux 适配 | avahi-client | LGPL-2.1+ | mDNS 广播 `_airplay._tcp` |
| Linux 适配 | bluez（libbluetooth） | LGPL-2.1+ | SDP 查询 + 裸 RFCOMM |
| Linux 适配 | dbus-1 | AFL-2.1 / GPL-2.0+ | 枚举已配对设备、读取当前 SSID |
| Linux / macOS 适配 | libusb-1.0 | LGPL-2.1+ | Android Auto 的 AOAP 控制请求与 bulk 端点 |
| macOS 适配 | IOBluetooth / CoreWLAN / dns_sd | 系统框架 | 蓝牙、Wi-Fi、mDNS |
| Android | MediaCodec / AudioTrack / BluetoothSocket / NsdManager | 系统 API | 解码、播放、蓝牙、mDNS |

**有意不使用的**：`libswresample`、`fdk-aac`、`libavformat`（取舍原因见 README「依赖」一节）。

> ffmpeg 的实际许可取决于构建配置：官方预编译包通常为 **LGPL-2.1+**；
> 若发行版以 `--enable-gpl` 构建则为 **GPL-2.0+**；`--enable-nonfree` 的构建不可再分发。
> 分发二进制前请确认你所链接的那一份属于哪种。

## 三、凭证与身份文件

- **MFi 配件身份不在本仓库**（`identity/`，见 `.gitignore`）。
- 它是 Apple 通过 **MFi 计划**签发的配件凭证，**不是本项目的一部分**，
  也不适用本项目的 GPL-3.0 许可。
- 它是运行本项目真机链路（iAP2 `0xAA00` 之后的握手）的**必需前提**，需使用者自行准备，
  并自行承担相应的合规责任。

`.gitignore` 里已按模式屏蔽了常见的凭证扩展名（`*.pk8` / `*.p7b` / `*.pem` / `*.key` / …），
但**这只是兜底**——别把凭证放进工作区。
