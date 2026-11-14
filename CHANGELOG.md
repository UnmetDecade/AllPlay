# Changelog

<div align="left"><b>English</b> · <a href="CHANGELOG.zh-CN.md">简体中文</a></div>

Version numbers correspond to **platform capability**: each version gets one platform to
end-to-end rendering.
The protocol core (`core/`) is the single copy shared by all platforms and never forks
between versions.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [v1.1] — Multi-protocol backend skeleton · 2026-11-08

Split `core/` into a "protocol backend" structure: CarPlay becomes the first backend, the
wired Android Auto module lands, and CarLife / HiCar reserve their interfaces. Host
behavior is unchanged (still the original path); the desktop build and existing self-tests
are all green.

### Added

- **Protocol backend interface** (`core/projection/backend.h`): `IProjectionBackend` /
  `IProjectionSession` / `IProjectionSink` / `BackendRegistry`. The host only knows
  protocol-neutral targets, input events, and media configuration, and no longer touches
  iAP2 / RTSP directly; see `docs/projection-backend.md` for the migration plan.
- **CarPlay backend** (`core/projection/carplay/backend_carplay.*`): both wireless
  (bt_trigger + Receiver) and wired (usbmux / WiredControl / iAP2) sessions are wrapped in
  the same interface; platform capabilities are injected through `WiredPorts`, and core
  contains no platform headers.
- **Wired Android Auto** (`core/projection/androidauto/`): AOAP 51/52/53 → AAP VERSION →
  TLS 1.2 (records wrapped in 0x0003) → service discovery → channels / AV SETUP / media
  streams. Includes a protobuf wire subset and Annex-B → avcC synthesis; PCM is converted
  from little-endian to big-endian before handing it to the host; implements AV_MEDIA_ACK
  flow control. `platform/posix/aa_usb_libusb.cpp` provides the libusb transport shared by
  Linux/macOS.
- **CarLife / HiCar placeholder backends**: interfaces complete; `probe()` returns empty
  and `connect()` reports not implemented.
- **Self-test** `tools/aa_wire_test.cpp`: AAP framing/deframing, 6-byte length field,
  half-frames, protobuf encode/decode.

### Changed

- `core/airplay` / `core/iap2` / `core/mfi` / `core/usb` / `core/bt` were moved wholesale
  into `core/projection/carplay/` (pure `git mv` + compatible include paths), with zero
  protocol code changes.
- HarmonyOS and Android build files were updated to follow the new paths.

### Known limitations

- The host still uses the original `Receiver` / `WiredTask` / `BtTask` and has not
  switched to `BackendRegistry`; Android Auto also has no UI entry point.
- Android Auto has only the protocol and transport layers implemented, **not yet verified
  on hardware**; wireless AA, CarLife, and HiCar are not implemented.

---

## [v1.0] — Real-device stability · 2026-11-06

A batch of fixes from continuous hands-on use with an iPhone SE2 / 16.

### Fixed

- **Swiping continuously would freeze** (`core/airplay/event_channel.cpp`): touch
  previously encrypted and called `send()` directly on the UI thread; when the phone was
  busy repainting, the send buffer filled up and the UI thread blocked in the syscall. The
  read thread on this channel (which must ACK the phone) was blocked by the same lock.
  Now there is a dedicated write thread and callers only enqueue; touch is treated as a
  "position snapshot", and a new one directly replaces an old one that has not been sent
  yet.
- **Noise when the stream opens**: when the phone opens the stream it buffers about two
  seconds and then delivers it all at once, which overflows the 1s ring pool; the old
  logic "dropped whatever exceeded the limit", and dozens of 9–21ms gaps in a row merged
  into one long gap. Now it switches to 70% in one step, replacing dozens of gaps with
  one.
- **Stutter at the start of streaming**: the initial water level was raised from 250ms to
  800ms, then lowered back to the resumption level once sound starts, absorbing the
  phone's first pause when it opens the stream.
- **Occasional hitch while listening to music (with no interaction)**: the phone sends
  audio in **bursts** — measured gaps of up to 569ms between consecutive packets, with RTP
  sequence numbers contiguous and not a single packet lost, just not sent. When the water
  level is below this gap, it drains once per burst. Ring pool 1s→1.5s, resumption water
  level 250ms→700ms (569ms + margin), set from measured gaps instead of guesswork.
- **Phone does not dial after 0x4301**: iOS sometimes does not proactively connect to the
  AirPlay port (measured: 14.5 seconds idle, and in the black-screen cases it never
  connected at all); only manually toggling Wi-Fi once would make it reconnect. Now if no
  connection arrives within 3 seconds, resend `0x5703 + 0x4301`, at most twice.

### Added

- **Selectable Wi-Fi security type** (settings page): none / WEP / WPA2 / WPA3, with
  `securityType` in `0x5703` and `0x4301` following it (previously hard-coded to 2); open
  networks no longer require a password.

### Diagnostics

- The audio side now reports four additional signals: packet-arrival gap (>80ms, with RTP
  sequence delta), ring-pool overflow, **every** underrun, audio-callback stall (>300ms),
  and system interruption/errors.

---

## [v0.9] — Steering-wheel control interface · 2026-11-06

Established the steering-wheel-control (wheel media key) layer in `core`: the upper layer
puts keys into a queue, and the two iAP2 links (wireless/wired) each encode them into HID
reports sent to the phone. The interface and encoding have passed self-tests; the `0x4E0x`
message numbers, report format, and key sources still await real-device and per-platform
integration.

### Added

- **Control interface** (`core/port/control.h`): `MediaKey` (play/pause, previous/next,
  Siri, answer/hang up…) + thread-safe `MediaKeyQueue` + `IMediaKeySink`. The direction is
  **head unit → phone**, opposite to `media.h`. Where keys come from (UI buttons, system
  media keys, future hardware drivers) is up to the upper layer.
- **HID encoding** (`core/iap2/hid.h`): `MediaKey` → HID usage (consumer page 0x0C /
  telephony page 0x0B) → report bytes → `0x4E02` messages. The wireless
  `WirelessTrigger` drains the queue in `run()`; the wired `WiredLink` provides
  `sendMediaKey` / `pumpMediaKeys`, called by the caller between pumps.
  **The `0x4E0x` message family and report format have not yet been verified on a real
  device**; check them with a packet capture when integrating (only one place needs
  changing).
- Self-test section ①b: usage mapping, key-down/up reports, message round-trip, queue FIFO
  and overflow.

---

## [v0.8] — HarmonyOS audio · 2026-11-06

CarPlay sound came out on a Mate 60: the audio stream the phone creates (type=102, AAC-LC,
48kHz stereo, raw frames) is decoded locally and played through the system sound card.
Video, touch, and audio all worked simultaneously for the first time.

### Added

- **Audio output** (`harmony/entry/src/main/cpp/harmony_audio.cpp`): the HarmonyOS
  implementation of `core::IAudioOutput`.
  Decoding uses `OH_AudioCodec` in **synchronous mode** (`OH_MD_KEY_ENABLE_SYNC_MODE=1`;
  in the default asynchronous mode `QueryInputBuffer` always returns
  `AV_ERR_OPERATE_NOT_PERMIT`), and playback uses the `OHAudio` renderer's `OnWriteData`
  callback. Decoding is inside two locks, and the playback callback only touches the ring
  pool, so the 10ms decode query can never block the sound card.

### Fixed

- **Raw AAC frames must have an ADTS header prepended**: `OH_MD_KEY_AAC_IS_ADTS=0` means
  LATM, and when fed raw frames the decoder only consumed and produced nothing (measured:
  3000 packets decoded to 0 frames). Changed to prepend a 7-byte ADTS header per frame and
  set that key to 1; 48 frames decoded to 47. The out-of-band ASC path
  (`OH_MD_KEY_CODEC_CONFIG`) decoded 0 frames on the same machine.
- **"Catch but do not play" became playable**: `feed()` was previously an empty
  implementation; now the whole path works.
- **A trio of stutters**: the callback block was pinned smaller and then released, the
  resumption water level was below one callback, and `QueryOutputBuffer` used a 0 timeout,
  causing PCM to accumulate in the decoder — together these produced continuous crackle
  (193 underruns out of 600 callbacks).
  Now: 700ms ring pool + 250ms resumption water level + 2ms output polling, measured
  underruns down to 0–3 per 600.
- **Occasional stutter while swiping** was a sender-side problem: switching the same Mate
  60 to an iPhone 16 as the sender no longer reproduced it; an iPhone 13 mini itself drops
  frames while swiping.

### Known limitations

- **iOS 13 and older cannot connect**: an iPhone SE2 (iOS 13) is answered with 0x1D03 at
  the 0x1D01 identification step, with reported parameter ids including 6/7/10. Tried all
  three of: the full message list, only the minimal CarPlay-required set, and no list at
  all — even an empty list was rejected. It is the phone side rejecting it, not the message
  format (the same bytes are accepted by iOS 16/18).
  The SE2 on iOS 13 connects normally after upgrading the OS; the code retains a
  "narrow down and retry on rejection" fallback.

## [v0.7] — HarmonyOS wireless real-device end-to-end · 2026-11-05

HarmonyOS (**Mate 60**) also works end-to-end: Bluetooth iAP2 wakes the iPhone → same-network
Wi-Fi → RTSP negotiation → **hardware-decoded on screen, landscape full-screen, touch
works**. The protocol core did not fork a single line; `core/airplay` was compiled into the
HAP unchanged through NAPI.

### Added

- **HarmonyOS platform**: ArkTS + hvigor project (`harmony/`). Bluetooth SPP
  (`socket.sppConnect`), mDNS advertisement, screen size from system APIs; the protocol is
  entirely in `core/`, bridged by `liballplay_core.so` through NAPI.
- **AVCodec hardware decoding to screen** (`harmony_decoder.cpp`):
  `OH_VideoDecoder_CreateByMime` + callback-style input/output, with `RenderOutputBuffer`
  drawing directly to the `XComponent` surface (surface id → `OHNativeWindow`).
- **Full screen**: the display resolution negotiated in `/info` is now **based on the
  screen aspect ratio** (1080p tier, width capped at 1920; measured 1920×868), and the
  iPhone rearranges the CarPlay UI to that size; entering a session switches to landscape,
  disconnecting switches back to portrait.
- **Touch reports**: `XComponent.onTouch` → normalized coordinates → event-channel HID
  `touchReport` (two contacts).
- **Build scripts**: `scripts/build-ohos-openssl.sh` (cross-compile OHOS OpenSSL),
  `scripts/sync-harmony-identity.sh` (temporarily copy `identity/` into the HAP rawfile;
  still in `.gitignore`).

### Fixed

- **avcC SPS/PPS must be in-band**: `OH_MD_KEY_CODEC_CONFIG` is ignored by this machine's
  decoder (system log `pps is null` / `EVNT_PARAMETER_SETS_LOST`), so a copy of AnnexB is
  prepended before every IDR.
- **The phone resends the same avcC every ~5 seconds**: previously the decoder was rebuilt
  each time, clearing the reference chain and making the picture periodically freeze;
  changed to compare by `csd + size + codec type` and skip rebuilding when identical.
- **Picture freezes after rotating**: rotating recreates the `XComponent`, and the old
  `OHNativeWindow` points to a reclaimed surface. Now the surface is torn down and rebuilt
  when the surface id changes, and restored immediately from the last csd without waiting
  for the phone to resend configuration.

### Known limitations

- **Audio is received but not played**: `IAudioOutput` discards data after receiving it;
  the HarmonyOS side has not yet integrated AudioRenderer.
- **No exit entry point during streaming**: the picture fills the entire screen and covers
  the "cancel" button on the launcher; you can only exit from the phone or the system side.
- Device selection and hotspot SSID/password are not persisted; you must select them again
  after an app restart.

---

## [v0.6] — macOS wireless real-device end-to-end · 2026-11-05

A real iPhone works on a Mac through **Bluetooth iAP2 → same-network Wi-Fi projection**,
and the "always-on behaviors a head unit should have" were completed: when the phone
leaves it automatically returns to the launcher, and when it returns it automatically
re-enters CarPlay, without touching the host at all. The launcher was fully redesigned.

### Added

- **Launcher redesign** (`host/launcher`): drawn at the coordinates of a 1540×850 design
  and scaled as a whole, with linear sampling so rounded corners/arcs no longer show
  stair-stepping; shows the **Bluetooth adapter name** (the one seen in the phone's
  Bluetooth list, not the Bonjour name), Chinese/English language switching, connection
  state, and waiting animation.
- **High-DPI rendering**: `SDL_WINDOW_ALLOW_HIGHDPI`, with the render surface producing
  physical pixels (Retina 2×); also added conversion between window "points" and render
  "pixels" — otherwise both launcher mouse hit-testing and touch coordinates are off
  (measured: touch y-axis biased upward on Mac).
- **Dual-channel `--dual`**: listen for wired (USB) + wireless **simultaneously**; whichever
  connects first wins. The other path keeps later connections outside and reopens
  automatically when the session ends.
- **Always-on reconnect**: under `--bt`, after the phone leaves it automatically retriggers
  after 5 seconds, with no key press and no host restart.

### Fixed

- **After reconnecting, the whole CarPlay session was always cut off at the 5-second
  timer**: the event channel object is reused across sessions (the listening port does not
  change), but the read/write keys are derived **per session** by pair-verify. The host's
  periodic `forceKeyFrame` encrypted with the old key, the phone could not decrypt it, and
  iOS ended the session immediately. Added `EventChannel::rekey()`; the first SETUP of each
  session recomputes the keys from that session's sharedSecret.
- **The session disconnected itself at 90 seconds**: after 0x4301 is sent, this iAP2 link
  is wireless CarPlay's heartbeat line; iOS keeps sending messages on it and the accessory
  must keep reading and ACKING. When the trigger window (90s) ended, reading stopped, and
  iOS judged the accessory offline about 30 seconds later and sent TEARDOWN. Changed to
  keep it open indefinitely after sending 0x4301, until the link actually breaks.
- **The phone would not automatically re-enter CarPlay after returning**: the always-on
  heartbeat kept the trigger thread on the old link, so it could never see a new 0x4301.
  Added a cancel flag: when the session ends (or after idling 45 seconds), withdraw this
  trigger and schedule the next one.
- **Stuck on the launcher for 10 seconds after the phone leaves**: control-connection
  idle detection shortened from 10s to 5s, and the disconnect reason is logged (read
  timeout = the phone stopped sending / read returned disconnected = peer closed) instead
  of guessing.

---

## [v0.5] — Android 4.4 (API 19) · 2026-10-14

The Android side was lowered to **Android 4.4 (API 19)** and verified on a 32-bit Amlogic
S905M TV box (Bluetooth iAP2 trigger → mDNS discovery → RTSP negotiation → 720p projection
→ audio). The point: **even the weakest Android box can serve as a CarPlay screen.**

### Added

- **Android 4.4 port**: `minSdk 28→19`. The Kotlin layer guards more than 20 API 26+ calls
  with `SDK_INT` (NotificationChannel / AudioAttributes+AudioFormat.Builder / MediaCodec
  buffer access / setOutputSurface / startLocalOnlyHotspot / startForegroundService /
  runtime permissions / battery optimization / dialog themes / letterSpacing, etc.);
  added `MediaCodecCompat` (old array-style buffer access; API 19 uses `getInputBuffers()`).
- **Native-layer API 19 compatibility**: `cpp/CMakeLists` enables C++17; `net_posix` uses
  SIOCGIFCONF instead of `getifaddrs`, which only exists in API 24; added `stdio_compat` to
  provide the `stdin/stdout/stderr` symbols missing before API 23; `log.cpp` uses logcat
  only on Android (no stderr).
- **Dual ABI + older NDK**: `abiFilters` adds `armeabi-v7a` (32-bit boxes); OpenSSL is
  cross-compiled per ABI into separate directories; NDK lowered to r25c (r26 no longer
  supports API 19).

### Fixed

- **mDNS advertisement failed on Android boxes**: JmDNS could not get 5353 (occupied by
  system netd, EADDRINUSE), so it falls back to the system NsdManager.
  On API 19 no TXT is set — `setTxtRecord(byte[])` does not exist, and
  `setTxtRecord(DnsSdTxtRecord)` causes NsdServiceInfo Binder serialization to fail;
  measured: an iPhone can connect using just Bluetooth 0x4301 + RTSP `/info`, so TXT is not
  required.
- **Performance mode changed to a fixed 1280x720@30fps** (previously "halve the
  resolution").
- **No automatic reconnect after disconnecting**: after an AirPlay session disconnects,
  the Bluetooth trigger now closes the old RFCOMM before resending 0x4301
  (the iPhone's iAP2 SPP service accepts only one connection; the new one cannot connect
  until the old one is closed).

### Known limitations

- After manually disconnecting the iPhone's Wi-Fi, iOS proactively exits CarPlay and the
  box cannot force re-entry — that is iOS behavior; real-world scenarios (brief Wi-Fi
  drops due to interference) are unaffected.
- The box's system hotspot (SoftAP) is locked by the Amlogic framework to "system packages
  only" (`setWifiApEnabled` throws `SecurityException: Package android does not belong to
  ...`), so enabling the hotspot by reflection inside the app is unavailable; use the same
  Wi-Fi or an external way to enable the hotspot.

---

## [v0.4] — Wired (USB) CarPlay · 2026-10-13

**Just plug in the cable.** Previous versions all used wireless (Bluetooth wake-up + same
LAN). This version adds the **wired** path: the entire USB control plane is written by us,
the media plane is delegated to the kernel, and finally the phone is guided onto the **same**
RTSP / encryption / screen-stream code as wireless.

![A real iPhone's wired CarPlay rendering on Ubuntu](docs/images/carplay-wired.jpg)

(The image above: a real iPhone with the cable plugged in, host `--wired` running on
Ubuntu — this host-side round **rendered all 3000 frames** and exited normally, with zero
WARN/ERR. Another probe-side 200-second long run: 1320 frames / 12.4 MB received, no
disconnects throughout.)

![After injecting one touch, CarPlay navigated into the "Appearance" page](docs/images/carplay-wired-touch.jpg)

(The image above: `--inject-touch 0.5,0.72` landed on the "Appearance" row, and the phone
**actually navigated into that page** — HID events sent over the event channel are
accepted on the wired path too.)

### Added

**Wired control plane (`core/usb/`, platform-agnostic)**
- `usbmux` v2: version negotiation + setup + mini TCP (the phone side is a TCP stack, so
  framing is up to us).
- `wired_control`: strings together the three connections the phone requires —
  ① plaintext lockdown pairing → ② in-place TLS upgrade of the session →
  ③ the iAP2 stream obtained from `StartService(carkit)`. Miss any one of the three and
  you cannot get the final service stream.
- `platform/posix/usb_libusb`: does only two things — switch the iPhone into the **CarPlay
  configuration** (when locked it reports only 1 configuration; this is iOS behavior, not a
  cable problem), and wrap the USBMUX port's bulk endpoints as `IByteStream`.

**Wired iAP2 (`core/iap2/wired_link`)**
- The link layer is **initiated by the host**: after sending the 6-byte marker, it does not
  wait for any reply; it immediately sends SYN itself and resends every 500ms.
  Sending only the marker leaves the phone completely silent (260 seconds of zero bytes) —
  that is how the earliest naive implementation died.
- `0x4300` parsing + `0x4301 CarPlayStartSession` assembly. The wired group's message
  structure is easy to build wrong: addresses are **each a separate string parameter with
  repeated id 0**, not a string list.

**Wired media plane** — **we do not write it**. After switching into the CarPlay
configuration, the kernel's `cdc_ncm` creates a real NIC, and AirPlay binds directly to that
NIC's IPv6 link-local address; NTB16 / Ethernet / IPv6 are all the kernel's job.

**Host `--wired` (`host/wired_task`)**: symmetric to `--bt`, but with a **different phase**
— wireless is "Receiver starts first, the phone finds us", while wired is "the phone **reads**
the address from `0x4301`", so the Receiver must wait until the USB side has figured out the
NCM NIC name and pinned the address before starting. The task is therefore split into two
stages (prepare / keep-alive), and `rx`'s lifetime is still managed by a single thread. On
prepare failure, press `W` to retry once without restarting the program.

**Touch**: on the wired path, touch goes through **exactly the same** event channel as
wireless (`POST /command` + encrypted HID), with no code change. Real-device verification:
injecting a click on the "Appearance" row made the phone navigate to that page (see the
image above).

**Probes**: `tools/usb_probe` (config switch), `tools/wired_probe` (control plane, including
`--selftest` offline self-check).

### Fixed

The following **only show up on real hardware**; a fake phone cannot catch any of them.

- **`0x4301` was sent but the phone did not connect — the Receiver bound the wrong
  address.** On the wired path the address is **chosen by us in advance**
  (link-local on the NCM NIC), and what `0x4301` reports must exactly match what the
  Receiver actually listens on. The original `Receiver::start` used "is there an IPv4
  address" as its success criterion, but the NCM NIC has **no IPv4 at all**, so it bound the
  wildcard — the phone connected to the address from `0x4301` and hit nothing. Added
  `ReceiverConfig::bindAddr`: an explicit binding bypasses the IPv4 criterion.
- **Link-local address `bind()` returned EINVAL.** Binding a `fe80::/10` address must
  include `sin6_scope_id`, otherwise the kernel rejects it outright (and reports EINVAL,
  which looks like a malformed argument). `bindAddr` now supports a `%interface` suffix,
  and a bare `fe80:` address gets `%ifname` appended by `Receiver` itself.
- **No IPv6 address at all on the NCM NIC.** The kernel sets `addr_gen_mode=1` on it
  (generating no addresses), while NetworkManager treats this point-to-point link as a
  "wired connection" and asks for DHCP (there is no DHCP server there), staying stuck in
  connecting and possibly clearing the address at any time. Added
  `allplay::posix::prepareWiredNcm`: `nmcli device set … managed no` → `ip link set up` →
  pin `fe80::2/64 … nodad`; the NIC name is looked up by USB `busnum`/`devnum` via
  `/sys/class/net/*/device/..` (the name derives from the MAC and must not be hard-coded).
- **Control plane all green, `RECORD` passed, then stuck at `/feedback`** — once every 2
  seconds, never seeing a picture. Root cause: in `SETUP` the phone asks for the screen
  stream, and because we had no `IVideoOutput` we did not provide `dataPort`; the phone had
  no data port and could only wait. The host already had a sink, so only the probe was
  affected; a counting-only sink was added to the probe.
- **`pkill -f wired_probe` also kills the shell running it** (the shell's own command line
  contains that string), so the command chain "kill the old process, then recompile" broke
  at step 1, the subsequent compile never ran, and it looked like a compile failure.

### Notes

- Wired is **only implemented on Linux**: the media plane depends on the kernel's `cdc_ncm`,
  and macOS / Windows have no equivalent.
- Without `libusb-1.0-dev`, CMake skips the entire wired path, and `--wired` explicitly
  reports "this build has no libusb".
- The three prerequisites before running (stop `usbmuxd`, `pkill gvfsd-gphoto2`, blacklist
  `apple_mfi_fastcharge`) and the two `sudo` environment traps are covered in
  [README wired prerequisites](README.md#wired-prerequisites-linux-all-three-required).

---

## [v0.3] — Real iPhone end-to-end · 2026-10-12

**The phone is summoned by the host itself.** v0.2 proved "once we get the stream, we can
render it"; this version proves "we can actively summon a real device": the host initiates
the Bluetooth iAP2 trigger → the iPhone discovers this machine via mDNS → RTSP negotiation
→ screen stream on screen. **The CarPlay picture appears on the Ubuntu desktop, and touch
reports work.**

![A real iPhone's CarPlay rendering on the Ubuntu desktop](docs/images/carplay-home.jpg)

### Added

**Bluetooth trigger (desktop host)**
- `host/bt_task`: the host goes from "only waiting for the phone to connect" to "waking
  the phone up itself". `runBtTrigger` is fully blocking and **has no cancellation
  interface**, so it runs on a dedicated thread; on failure the whole chain is retried
  every 3 seconds. The thread moves all the transport and parameters it needs into the
  closure, and detaches rather than joins on exit — otherwise a single exit can block for
  up to 90 seconds.
- All switches are opt-in, and **behavior is completely unchanged without `--bt`**:
  `--bt` / `--bt-addr` / `--bt-device` / `--bt-mode` / `--ssid` / `--wifi-pass` /
  `--bt-retry`. Pressing `B` at runtime retriggers once.

### Fixed

The following **only show up on real hardware**; a fake phone catches none of them.

- **The mDNS TXT record was binary garbage — the root cause of "the phone never
  connects".**
  `avahi_entry_group_add_service()` is a **variadic** interface (`...` is a series of
  NULL-terminated `const char*`), but the original code passed a `const char**`, so avahi
  read **the pointer array's own memory** as the first TXT string and advertised a few bytes
  of garbage — the symptom was "the phone never comes", with not a single RTSP line in the
  log. The iPhone identifies the receiver by the TXT fields `features` / `flags=0x4` /
  `deviceid` / `srcvers`; if it cannot read them, it does not connect. Switched to
  `avahi_entry_group_add_service_strlst()` + `AvahiStringList` (`avahi_string_list_add`
  prepends, so add in reverse to preserve order).
- **Tapping an icon did nothing, but dragging the map worked.** The touch report sent only
  `down=false` on the lift frame, leaving the coordinates at (0,0) — the phone received
  "pressed at the icon → lifted at the top-left corner", interpreted as a swipe rather than
  a tap. Changed so **the lift also carries the last position**, aligning with the
  `ACTION_UP` semantics in Android's `MainActivity.kt`.
- **After disconnecting it did not return to the launcher; the picture froze on the last
  frame.** "Freeze rather than disappear" was originally designed for dropped frames
  mid-session, but at session end the last frame in the texture kept being redrawn by
  `present()` — the log said "back to launcher" while the screen did not move. Added
  `SdlVideoSink::dropFrame()` and drop it at session end.
- **With `--statusbar` there were black bars and the status bar was invisible.** The status
  bar was drawn at `y = screenH`, but the window was only `screenH` tall — the whole bar fell
  outside the window; the render loop also laid out video as `winH - barH`, squeezing 16:9
  into 1280x694 with 23px of black on each side. Window height changed to
  `screenH + barH`, and both defects disappeared.
- **The screen stream failed silently when skipped.** When `video_` was null,
  `session.cpp` used to just `continue`: no `dataPort` returned, the phone waited 10
  seconds and disconnected, and the log gave no hint at all (which cost a lot of debugging
  time). Added a WARN that only logs and does not change behavior.

### Docs

- README updates the "current status"; adds a troubleshooting entry **"the phone never
  connects → check the mDNS TXT first"**.

---

## [v0.2] — Ubuntu platform rendering · 2026-10-12

The desktop host grew from "window only" to **actually rendering the CarPlay picture**, and
the Linux Bluetooth transport layer was wired up.
It shares the same `core/` with v0.1's Android side, changing only `platform/` and `host/`.

### Added

**Video**
- `host/media/ffmpeg_decoder` + `sdl_video_sink`: ffmpeg software-decodes H.264/HEVC →
  SDL2 texture, displayed with letterboxing.
- Decoding runs on the ScreenStream thread (providing natural backpressure on TCP
  receive); the main thread only pulls frames and renders. The frame queue is bounded;
  when full it drops the oldest frame to keep latency low; the texture is rebuilt only when
  size/format changes.

**Audio**
- `host/media/sdl_audio_sink`: AAC-LC / Opus / LPCM, SDL2 output.
- **Deliberately avoids libswresample** — its whole set of channel-layout parameters has
  different signatures between ffmpeg 4.4 and 5.1+, and here we only need one conversion,
  "planar float → interleaved s16", so writing it ourselves is simpler.
  All version differences are confined to one file, `host/media/ffmpeg_compat.h`.

**UI and input**
- `host/launcher`: launcher page (two-tone logo + Bluetooth ripple + status text), aligned
  with the Android launcher; after connecting it switches to full-screen video, and the
  status bar degrades to a debug overlay explicitly enabled by `--statusbar`.
- Touch reports: mouse / finger → normalized against the **letterboxed video rectangle** →
  event channel.

**Bluetooth (Linux)**
- `platform/posix/bt_bluez.cpp`: BlueZ transport layer, client mode (we actively connect to
  the phone's iAP2 SPP service).
  Path: SDP finds `00000000-deca-fade-deca-deafdecacafe` → **RFCOMM channel 1** → connect →
  iAP2 identification → receive the phone's proactively sent `0xAA00 RequestCertificate`.
  Real-device (iPhone) verification stops here; beyond it requires an MFi identity.

**Acceptance tools**
- `tools/fake_phone`: fake iPhone. Real sockets run RTSP + pair-setup/verify +
  ControlCipher encryption + encrypted screen-stream frame push — **end-to-end acceptance no
  longer needs an iPhone**.
- `tools/video_probe`: a decode smoke tool that feeds local AnnexB H.264. When the screen is
  black, run it first to bisect: picture = protocol side's fault, no picture = decode side's
  fault.
- `tools/phone_crypto.h`: phone-side cryptography extracted from `m3_test` for both to
  share — two copies would go wrong together and make "self-tests green but end-to-end
  failing" possible.

### Fixed

- **SIGSEGV during SETUP**: `Receiver::start()` stored the **address of the
  `PlatformServices` parameter** in the Session, but the Session first uses it at SETUP to
  create data/event channels, long after `start()` returned. Changed to hold its own copy.
- **Keyframes not detected, permanently black screen**: `ScreenStream::isRandomAccess()`
  looked only at the first NAL of an access unit, but the common layout of a keyframe AU is
  `[SPS][PPS][IDR]`; with SPS(7) first it was judged "not a keyframe" — the release gate
  never opened. Changed to scan the entire AU. Affects both real and fake phones.

### Build

- CMake links ffmpeg (avcodec/avutil/swscale, **not** swresample), bluez, dbus-1.
  When ffmpeg is missing, the host degrades to a stub instead of being skipped entirely.
- Added the `fake_phone` / `video_probe` targets.

---

## [v0.1] — Android platform rendering · 2026-10-11

First release. The protocol core was implemented from scratch and verified end-to-end on
Android.

### Added

**Protocol core `core/`** (platform-agnostic C++, sole third-party dependency OpenSSL)
- `airplay/rtsp`: minimal RTSP server (control channel; encrypted after pair-verify)
- `airplay/session`: session state machine — pair-setup / pair-verify / auth-setup / SETUP /
  RECORD
- `airplay/video_stream`: screen stream — 128B header decryption → AVCC→AnnexB → keyframe
  gate
- `airplay/audio_stream`: audio stream — RTP over UDP decryption + out-of-order sequence
  reordering
- `airplay/event_channel`: event channel — touch HID reports (up to 2 contacts) +
  forceKeyFrame commands
- `airplay/control_cipher`: control-channel frame encryption (ChaCha20-Poly1305)
- `iap2/`: iAP2 protocol stack — wire format / link layer (marker→SYN→ACK→NORMAL) /
  wireless control flow
- `mfi/mfi_identity`: MFi accessory identity interface (certificate + ECDSA signature);
  **the interface follows the shape of the hardware chip**, so core does not change when it
  is later replaced by an authentication-chip implementation
- `crypto/`: all cryptographic primitives implemented from scratch (hkdf /
  chacha20poly1305 / srp6a / x25519 / ed25519 / tlv8 / aes_ctr)
- `bplist`: Apple binary plist encoder/decoder
- `bt/bt_trigger`: Bluetooth trigger orchestration (select device → RFCOMM → iAP2 → 0x4301
  bootstrap)

**Android host**
- `MediaCodecSink`: MediaCodec hardware decode → direct Surface output
- `AudioSink`: AudioTrack playback
- `BluetoothService` / `MdnsService` / `HotspotManager`: Bluetooth, mDNS, hotspot
- Touch reports, launcher and status bar
- Gradle + NDK: Kotlin shell + C++ core, wired to system APIs through `jni_bridge.cpp`
- `minSdk 28` / `targetSdk 34` / `arm64-v8a` only

**Platform adapters**
- `platform/posix`: POSIX sockets, avahi (Linux) / dns_sd (macOS), IOBluetooth, BlueZ (stub)
- `platform/windows`: Winsock2 + `AF_BTH` (skeleton, not wired to the host)

**Self-tests**
- `core_selftest` (RTSP + `/info` + mDNS advertisement)
- `iap2_test` (iAP2 wire format + link layer + MFi signature)
- `m3_test` (SRP against Python reference vectors + full fake-phone flow + mock screen
  stream)

---

## Unreleased

- **Productized MFi identity**: the `0xAA00 RequestCertificate` gate requires an accessory
  identity issued by the Apple MFi program (`identity/`, not checked in). During
  development, a self-provided identity can take the full path — that is how v0.3's
  real-device verification was done — but **a pure software identity is not a product
  path**; shipping as an accessory still requires the MFi program.
- **Desktop audio**: in the real-device round, the iPhone only requested the screen stream
  and `type=130`, **without requesting any audio stream**, so the desktop audio path has
  only ever been verified by the fake phone; real-device behavior remains to be observed.
