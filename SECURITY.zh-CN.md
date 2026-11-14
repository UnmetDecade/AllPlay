# 安全策略

<div align="left"><a href="SECURITY.md">English</a> · <b>简体中文</b></div>

## 支持版本

AllPlay 是协议实现，不是加固过的整机产品。安全修复进入 `main` 和最新发布的版本。

| 版本 | 支持 |
| --- | --- |
| `main`（最新发布） | ✅ |
| 旧 tag | ❌ —— 请升级到最新版本 |

## 上报漏洞

**安全问题不要开公开 issue。**

请使用 GitHub 的私密安全公告流程：

1. 打开 <https://github.com/UnmetDecade/AllPlay/security/advisories/new>。
2. 说明影响、受影响组件（`core/`、`platform/`、`host/`、`android/`）、版本或 commit，以及复现步骤。
3. 如果问题涉及崩溃或内存破坏，请附上 sanitizer（ASan/UBSan）输出。

我们会在 7 天内给出初步回复。确认有效后，会约定披露时间，在私有分支上准备修复，并与版本说明一起发布安全公告。

## 范围

在范围内：

- 协议解析器（`core/projection/**`）、bplist 解析器、TLV8/plist 编解码器和加密封装中的内存安全问题。
- iAP2、MFi-SAP、AirPlay 或 Android Auto 握手过程中的认证或配对绕过。
- 同一局域网或 USB 上的对端（无论是否已配对）可触发的拒绝服务。
- 导致 MFi 身份或配对记录泄露的凭据处理问题。

不在范围内：

- 缺少 Apple MFi 授权。AllPlay 是独立实现，见 `THIRD_PARTY_NOTICES.md`。
- 上游库（OpenSSL、ffmpeg、SDL2 等）已公开的漏洞 —— 请向上游报告，并在这里开普通 issue 升级依赖。
- 需要主机 root、物理接触 `identity/` 或修改过构建的攻击。

## 凭据处理

`identity/` 存放 Apple 签发的 MFi 私钥和证书，已被 git 忽略，绝不能提交、贴进 issue 或附加到发布产物。任何出现在日志或崩溃转储中的凭据都应视为已泄露并轮换。
