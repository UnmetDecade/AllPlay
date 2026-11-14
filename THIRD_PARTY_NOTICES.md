# Third-Party Notices

<div align="left"><b>English</b> · <a href="THIRD_PARTY_NOTICES.zh-CN.md">简体中文</a></div>

## 1. Where this project comes from

**AllPlay** is a **C++ reimplementation** of the **DiPlay** / **xcertplay** wireless
CarPlay receiver. This project's protocol behavior, flows, and wire formats follow their
implementations and write-ups; it is therefore released under **GPL-3.0** and retains
upstream attribution.

### DiPlay — GPL-3.0

- Repository: https://github.com/shihabal3amri/DiPlay
- Author: shihabal3amri
- Description: Android wireless/wired CarPlay receiver (public preview).

### xcertplay — GPL-3.0

- Author: shilapi
- Description: upstream project of DiPlay; a CarPlay receiver protocol implementation.

### Protocol research references

- **LIVI**, **Showcase**: AirPlay / iAP2 / CarPlay protocol research (also credited by
  DiPlay in its own attribution).
- **AASDK** (https://github.com/f1xpl/aasdk, GPL-3.0) and **OpenAuto**: the source of
  protocol facts for the wired Android Auto path (AOAP, AAP framing, TLS 1.2, protobuf
  field numbers). `core/projection/androidauto/` was written **independently** from public
  protocol facts; it does not copy AASDK source and does not depend on any Boost /
  protobuf / AASDK runtime component. Its license remains compatible with AASDK
  (GPL-3.0).

This project is an **independent implementation**: it contains no Apple code,
documentation, or binaries. Protocol facts come from public upstream implementations and
public material, organized in [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md).

## 2. Runtime dependencies

The principle is **system libraries only** — no third-party components that you have to
build yourself.

| Component | Dependency | License | Used for |
|---|---|---|---|
| `core/` | OpenSSL (libcrypto) | Apache-2.0 | all cryptographic primitives (HKDF / ChaCha20-Poly1305 / SRP-6a / X25519 / Ed25519 / AES-CTR) |
| Desktop host | SDL2 | zlib | window, textures, audio output, input events |
| Desktop host | SDL2_ttf | zlib | status bar and launcher fonts |
| Desktop host | ffmpeg: libavcodec / libavutil / libswscale | LGPL-2.1+ | H.264/HEVC software decode, AAC/Opus decode, pixel-format fallback |
| Linux adapter | avahi-client | LGPL-2.1+ | mDNS advertisement of `_airplay._tcp` |
| Linux adapter | bluez (libbluetooth) | LGPL-2.1+ | SDP queries + raw RFCOMM |
| Linux adapter | dbus-1 | AFL-2.1 / GPL-2.0+ | enumerate paired devices, read the current SSID |
| Linux / macOS adapter | libusb-1.0 | LGPL-2.1+ | Android Auto AOAP control requests and bulk endpoints |
| macOS adapter | IOBluetooth / CoreWLAN / dns_sd | system frameworks | Bluetooth, Wi-Fi, mDNS |
| Android | MediaCodec / AudioTrack / BluetoothSocket / NsdManager | system APIs | decode, playback, Bluetooth, mDNS |

**Deliberately not used**: `libswresample`, `fdk-aac`, `libavformat` (rationale is in the
README's "Dependencies" section).

> ffmpeg's actual license depends on the build configuration: official prebuilt packages
> are usually **LGPL-2.1+**; if a distribution was built with `--enable-gpl` it is
> **GPL-2.0+**; builds with `--enable-nonfree` are not redistributable. Before
> distributing binaries, confirm which one you are linking.

## 3. Credentials and identity files

- **The MFi accessory identity is not in this repository** (`identity/`, see
  `.gitignore`).
- It is an accessory credential issued by Apple through the **MFi program**. It is **not
  part of this project** and is not covered by this project's GPL-3.0 license.
- It is a **prerequisite** for running this project's real-device path (the iAP2
  handshake after `0xAA00`); users must supply it themselves and bear the corresponding
  compliance responsibility.

`.gitignore` already blocks common credential extensions by pattern (`*.pk8` / `*.p7b` /
`*.pem` / `*.key` / …), but **that is only a safety net** — do not put credentials in the
working tree.
