# 贡献指南

<div align="left"><a href="CONTRIBUTING.md">English</a> · <b>简体中文</b></div>

感谢参与。AllPlay 是协议实现，有一条硬规则：**`core/` 与平台无关，且必须保持如此。**
其余部分都是适配层。

## 开始之前

- 先读 [`docs/architecture.zh-CN.md`](docs/architecture.zh-CN.md)；如果要动投影后端，再读
  [`docs/projection-backend.zh-CN.md`](docs/projection-backend.zh-CN.md)。
- 协议行为的变化记录到 [`docs/PROTOCOL-NOTES.zh-CN.md`](docs/PROTOCOL-NOTES.zh-CN.md)，
  给出观测依据或上游参考，不要猜。
- 安全问题**不要**走 issue 或 PR，见 [`SECURITY.zh-CN.md`](SECURITY.zh-CN.md)。

## 构建与测试

### Linux

```sh
sudo apt-get install -y cmake g++ pkg-config libssl-dev \
  libavahi-client-dev libbluetooth-dev libdbus-1-dev libusb-1.0-0-dev \
  libsdl2-dev libsdl2-ttf-dev libavcodec-dev libavutil-dev libswscale-dev
scripts/build.sh
./build/iap2_test && ./build/m3_test && ./build/aa_wire_test
```

### macOS

```sh
brew install cmake openssl@3 avahi libusb sdl2 sdl2_ttf ffmpeg
scripts/build.sh
./build/iap2_test && ./build/m3_test && ./build/aa_wire_test
```

缺少 SDL2 时桌面主机会自动跳过，这在无界面机器上是预期行为。

### Android

Android 主机通过 Android Studio 或 Android SDK/NDK + 交叉编译的 OpenSSL
（`OPENSSL_BASE_DIR`）构建。CI 不构建它，因为 GitHub 托管 runner 上没有 NDK 和交叉编译的
OpenSSL。改到 `android/` 的 PR 请先在本地构建。

### 仅核心构建（快，不需要 GUI/蓝牙栈）

```sh
cmake -S . -B build-core -DALLPLAY_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j"$(nproc)"
./build-core/iap2_test && ./build-core/m3_test && ./build-core/aa_wire_test
```

CI 的必需检查 `core` 用的就是这个配置。

## Pull Request

- 一个 PR 只做一件事，标题用约定前缀（`core:`、`host:`、`android:`、`docs:`、`ci:`）。
- 测试：行为变化要在 `tools/` 下补自测。确实无法测试的改动，请在描述里说明原因。
- 文档：用户可见行为变化要同时更新 `README.md` **和** `README.zh-CN.md`；需要记版本的改动在
  `CHANGELOG.md` **和** `CHANGELOG.zh-CN.md` 的 `[Unreleased]` 下各加一条。
- 所有必需检查必须通过。`main` 受保护：禁止直接 push、禁止强推、禁止删除分支。
- 绝不提交 `identity/`、配对记录、签名密钥或任何凭据。`.gitignore` 覆盖了常见扩展名，
  提交前仍请检查 `git diff --cached`。

## 提交信息

用祈使句说明改了什么、为什么，例如：

```text
core: reject AAP frames whose declared length exceeds the buffer

The old parser trusted the length field, so a malformed frame could read past
the end of the receive buffer. Add the bounds check and a regression case in
aa_wire_test.
```

提交信息、代码注释、日志输出和文档默认使用英文；中文文档以 `.zh-CN.md` 对照维护。

## 代码风格

- C++17，4 空格，LF，文件末尾换行（`.editorconfig`）。
- `core/` 不得包含平台头文件，也不得链接平台库。
- 新增第三方代码要在 `THIRD_PARTY_NOTICES.md` 及其 `.zh-CN.md` 中登记许可证。

## 发布

版本从 `main` 打 tag 发布，历史见 `CHANGELOG.md`。`v*` tag 会触发 release job，把源码归档和
`SHA256SUMS` 附到 GitHub Release。
