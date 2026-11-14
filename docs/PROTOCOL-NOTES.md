# Protocol Notes (Implementation Basis)

<div align="left"><b>English</b> · <a href="PROTOCOL-NOTES.zh-CN.md">简体中文</a></div>

> **Nature**: this is an organized record of **protocol behavior facts** (checked
> 2026-10-10), not code.
> This project's implementation is a **C++ reimplementation based on DiPlay / xcertplay
> (GPL-3.0)**, matching the formats/flows here item by item and verified on real devices.
> Every entry carries the "why it is this way" context; when implementing, if something
> conflicts with real-device behavior, **the device wins — and come back and fix this
> document**.

## 0. Overview of the three paths

```
iPhone ──(1) Bluetooth RFCOMM / iAP2 bootstrap (trigger + MFi + push config)──▶ us
       └─(2) same LAN: AirPlay session (RTSP + screen stream TCP)────────────▶ us
       ◀─(3) event channel: HID/touch/commands (encrypted, POST /command)──   us
```

**Wired (USB) is a separate access path**, and after reaching (2)(3) it merges completely
with the above:

```
iPhone ──(0) USB: switch into CarPlay configuration, open USBMUX port ───────▶ us
       ──(1') usbmux v2 → lockdown pairing → in-place TLS → carkit service stream
       ──(2') iAP2 runs on that service stream: host initiates marker+SYN → identify → MFi → power/subscriptions
       ◀──(3') 0x4300 CarPlayAvailability ──────────────────────────────────── us
       ──▶(4') 0x4301 CarPlayStartSession (report local IPv6 link-local + port)
       ──(5') the phone connects to that address → merges into (2)(3)
```

The difference is entirely "who speaks first" and "who chooses the address": wireless is
**we advertise, the phone finds us**; wired is **the phone asks, we answer**, with the
address pinned by us on the kernel NCM NIC and then reported to it. See
[§10](#10-wired-usb-access-2026-10-13).

---

## 1. Bluetooth / iAP2 trigger (M2)

### 1.1 Prerequisites
- The iPhone must **first be Bluetooth-paired (bonded) with this machine**. Reference
  behavior: only pick already-paired devices whose name contains "iPhone", preferring
  "connected" ones.
- We connect to the **SPP service on the phone** (we are the RFCOMM client):
  UUID `00000000-deca-fade-deca-deafdecacafe`.
- Alternative (if real-device verification goes this way): we register an SDP record
  "Wireless iAPv2" and wait for the phone to connect as a client.
  **Leave room for both paths; let the device decide.**

### 1.2 iAP2 link layer
- Start marker: `FF 55 02 00 EE 10` (6 bytes, for link synchronization).
- Data frame 9-byte header: `0xFF5A | u16 length | u8 control | u8 seq | u8 ack | u8
  sessionId | u8 checksum`;
  **checksum = sum of all bytes in the frame (including itself) ≡ 0 (mod 256)**.
- Control bits: SYN 0x80 / ACK 0x40 / EAK 0x20 / RST 0x10; session: 10=control, 11=EA,
  12=file transfer.
- Sync payload v1: maxOutgoing/maxLength/retransmit timeout/max retransmits and ACKs/
  session descriptor table; initial seq=99; retransmit 4s, at most 4 times; sliding
  window.

### 1.3 CSM message layer
- Frame: `0x4040 | u16 length | u16 msgId | parameter list` (big-endian, no checksum).
- Parameter: `u16 length | u16 paramId | payload`; fragmented according to the peer's
  session-10 maximum payload.

### 1.4 Message flow (wireless)
1. **Identification**: `0x1D00 StartIdentification` → `0x1D01 IdentificationInformation`
   → `0x1D02 accepted`.
   The identification information contains: our EA protocol name, Bluetooth link role
   (BOOTSTRAP), message list (what we send/receive/subscribe to),
   **group 17 "blue" using this machine's Bluetooth MAC**, group 24 SSID, etc.
2. **MFi authentication**: `0xAA00 RequestCertificate` → `0xAA01 (certificate)` → `0xAA02
   challenge` → `0xAA03 (signature)` → `0xAA04 failure / 0xAA05 success`.
   The signature and certificate come from the MFi accessory identity (see §5.4).
3. **Subscriptions**: 0x4157 / 0x5200 / 0xAE00 / 0x5000 / 0x4154 (each has semantics; see
   the reference list).
4. Receiving the phone's **`0x5702 RequestAccessoryWiFiConfiguration`** → reply
   **`0x5703 AccessoryWiFiConfiguration`**:
   `ssid / passphrase / securityType / channel / [bssid hint]` (same-LAN mode: fill in the
   **existing** network information).
5. Receiving the phone's **`0x4300 CarPlayAvailability`** → send **`0x4301
   CarPlayStartSession`**:
   - group 1 (wireless): `ssid / passphrase / channel / ipAddresses[] / securityType`
   - base parameters: `airPlayPort`(u32), `deviceIdentifier`, `publicKey`(Ed25519 public
     key), `sourceVersion`
   - **After sending, start a 30s watchdog: wait for the AirPlay TCP connection; on
     timeout, end this attempt and retry.**
6. Receiving `0x4E0E DeviceTransportIdentifier` → **resend 0x5703 once** (the
   "post-transport" path); track `0x4E0D WirelessCarPlayUpdate`.

---

## 2. Bonjour (M1)

- **Advertise** `_airplay._tcp` (using system Avahi), TXT:
  `deviceid`(this machine's NIC MAC), `features`(CarPlay feature bits, including
  CarPlayControl), `flags=0x4`,
  `model`, `srcvers`, `protovers=1.1`, `pi`(pairing UUID), `pk`(Ed25519 public key hex).
- **Discover** the phone's `_carplay-ctrl._tcp`, resolve the address, then probe:
  `GET /ctrl-int/1/connect`, with request header `AirPlay-Receiver-Device-ID: <deviceid>`;
  retry 7 times at 1.5s intervals, connect/read timeout 3s. **This "poke" prompts the phone
  to initiate the RTSP connection.**

---

## 3. AirPlay session (M3)

### 3.1 Endpoints (RTSP over TCP, default :7000, automatically changed if occupied; **the actual port must appear in the Bonjour TXT and in 0x4301**)
| Method | Path | Notes |
|---|---|---|
| GET | `/info` | Returns bplist: sourceVersion, features, statusFlags=4, model/manufacturer, deviceID, pi/pk, displays (main display UUID, etc.), HID device declarations |
| POST | `/auth-setup` | MFi-SAP (see §5.4); `encryptionType=MFiSAPv1` |
| POST | `/pair-setup` | SRP-6a (see §5.1) |
| POST | `/pair-verify` | X25519+Ed25519 (see §5.2) |
| SETUP | — | First batch (no streams): the phone provides timingPort/macAddress etc.; **we reply with eventPort / timingPort**; second batch streams[]: **110 main display / 111 secondary display** → reply `{type, dataPort}`; 100/101/102 audio (decode and play); 130 data (iAP tunnel, can be deferred) |
| RECORD | — | Session active (the milestone where "video is coming") |
| POST | `/command` | See §6; `forceKeyFrame` is used for keyframe recovery |
| POST | `/feedback` | Per stream streamConnectionID/timestamp/sampleTime (NTP-synchronized time) |
| TEARDOWN | — | Teardown |

### 3.2 ControlCipher (**all control traffic is encrypted after pair-verify completes**)
- Frame = `[2-byte LE ciphertext length][ciphertext][16-byte tag]`; **the 2-byte length
  header is used as AAD**; independent counters per direction.
- ChaCha20-Poly1305; **nonce = 4 zero bytes + 8-byte LE counter**.

---

## 4. Screen stream (M3)

- Separate TCP connection (the port is given to the phone in the SETUP response).
- **128-byte header**: offset 0 = `u32LE bodyLen`; offset 4 = opcode:
  - `0` = encrypted video frame; `1` = plaintext config frame (avcC/hvcC, identified by
    fourcc).
- Frame body decryption: ChaCha20-Poly1305, **the entire 128-byte header is used as AAD**,
  nonce rule same as the §3.2 counter.
- Key: `HKDF-SHA512(sharedSecret, "DataStream-Salt" + streamConnectionID,
  "DataStream-Output-Encryption-Key", 32)`.
- Plaintext = **NALU with a 4-byte big-endian length prefix (AVCC)** → convert to AnnexB
  (each NAL must be validated);
  prepend SPS/PPS before the first I-frame (nalType 5/6).
- Keyframe recovery: when needed, `POST /command {"type":"forceKeyFrame"}` (secondary
  display includes its UUID); the decoder side implements a "do not recover without a
  keyframe" gate.
- Timestamps: the header carries displayTicks (NTP-synchronized domain); for now simplify
  to "display each frame as it arrives"; alignment/frame dropping comes in M4.

---

## 5. Cryptography (all implemented with OpenSSL)

### 5.1 pair-setup (SRP-6a)
- 3072-bit, SHA-512, g=5 (RFC 5054 prime), username `Pair-Setup`, **PIN 3939**.
- **Exact formulas** (checked byte by byte against the AirPlay protocol; implemented in
  core/crypto/srp6a.cpp):
  - `k = SHA512(toBytes(N) ‖ pad384(g))` (the first argument is **toBytes(N)**, not
    pad(N)).
  - `x = SHA512(salt(16) ‖ SHA512("Pair-Setup:3939"))`; `v = g^x mod N`; `b` is 32 random
    bytes.
  - `B = (k*v + g^b) mod N`; `u = SHA512(pad384(A) ‖ pad384(B))` (first verify `A mod N !=
    0`).
  - **`K = SHA512(toBytes(S))` (shortest byte form, no zero padding, no RFC5054
    interleaved hashing)** — the easiest trap.
  - `M1 = SHA512((H(N)⊕H(g)) ‖ SHA512("Pair-Setup") ‖ salt ‖ pad384(A) ‖ pad384(B) ‖ K)`;
    `M2 = SHA512(pad384(A) ‖ M1 ‖ K)`.
- Message body TLV8 (type 0xff continuation chunks); HKDF labels
  `Pair-Setup-Encrypt-Salt` / `Pair-Setup-Encrypt-Info`;
  nonces `PS-Msg05` / `PS-Msg06`; store the peer's **long-term Ed25519 public key**.

### 5.2 pair-verify
- Ephemeral X25519 + Ed25519 signature; labels `Pair-Verify-Encrypt-Salt/Info`; nonces
  `PV-Msg02/03`.
- HKDF-SHA512(sharedEcdh, `Control-Salt`, `Control-Write-Encryption-Key` /
  `Control-Read-Encryption-Key`) → the keys from §3.2.

### 5.3 ChaCha20-Poly1305
- 128-bit tag; nonce64 = 4 zero bytes + 8-byte LE counter (independent increment per
  direction).

### 5.4 MFi-SAP (`/auth-setup` shares identity with iAP2)
- MFi accessory identity: EC P-256 private key (PKCS#8) + accessory certificate (PKCS#7,
  **verified 607 bytes < 640**), provided externally and not checked into the repository.
- Signature: ECDSA over the **32-byte digest** (no additional hashing), output as **64-byte
  raw r‖s** (**not DER**).
  - Digest composition (MFi-SAP protocol): `digest = SHA256(accPub(32) ‖ peerPub(32))`;
    **our identity is `LocalMfiAuthenticationClient` (protocolMajor=3) → use the SHA256
    branch**;
    the SHA1 branch belongs only to the hardware coprocessor (protocolMajor==2). The BAA
    certificate branch signs `pub‖peerPub` directly (no hashing), which does not apply
    here.
- `/auth-setup` flow: request body `[u8 version=1][32-byte peer X25519 public key]`;
  we generate an X25519 private key → `shared` → `aesKey=SHA1("AES-KEY"‖shared)[:16]`,
  `aesIv=SHA1("AES-IV"‖shared)[:16]`;
  response `[32 our public key][u32BE cert length][cert][u32BE signature length][AES-128-CTR(signature)]`.
- The iAP2 side (§1.4 step 2) uses the same certificate and signer.

---

## 6. Event channel / HID reports (Android already wired up)

- The `eventPort` in the SETUP response is a ServerSocket; after the phone connects:
  encrypted with `HKDF-SHA512(shared, "Events-Salt",
  "Events-Write-Encryption-Key"/"Events-Read-Encryption-Key", 32)` (§3.2 frame format).
- Touch: `POST /command`, bplist: `{"type":"hidSendReport","uuid":"2a2a2a2a","hidReport":…}`;
  touch report: 2 contacts × 6 bytes (slot, down, x u16le, y u16le); coordinates are
  normalized to display pixels.
- Others: knob uuid `2a2a2a2b`, media keys `2a2a2a2c`, phone `2a2a2a2d`; `setNightMode`,
  `requestSiri`.

---

## 7. /info key points (reference values; re-check on device)

- `features` follows the protocol's wireless value (including CarPlay feature bits; start
  from the protocol-recorded value and adjust against the device when implementing).
- `flags=4`; `model`/`manufacturer` are our choice; `statusFlags`; `deviceID` = NIC MAC
  (consistent with the Bonjour TXT).
- displays: main display `MAIN_UUID = b7e6c5a0-1111-4000-8000-000000000001` (secondary
  display …0002, not implemented yet).
- HID declarations: touch (+ knob/media/phone optional); descriptors follow the protocol's
  shape.

## 8. Items awaiting real-device confirmation
1. RFCOMM direction (client connecting to the phone vs server waiting for a connection).
2. Effect of the features bits and concrete /info values on current iOS.
3. NTP: stub first (whether unanswered NTP requests are accepted); if that fails, add a
   minimal NTP responder.
4. IPv6/IPv4 address selection in same-network mode (start with IPv4 preferred).

## 9. Sources and boundaries
- The above is an organized record of **protocol facts** (checked 2026-10-10).
- This project is a **C++ reimplementation based on DiPlay / xcertplay (GPL-3.0)**;
  dependencies are system libraries only.

---

## 10. Wired (USB) access (2026-10-13)

Derived from section-by-section verification against a real device; everything works on
Linux + iPhone (3000 frames displayed on device, 200 seconds without dropping the link).

### 10.1 Prerequisites (Linux, all required)

- `sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd` — the USBMUX port can have
  only one owner.
- `pkill -f gvfsd-gphoto2` — otherwise gvfs races to mount the photos.
- blacklist `apple_mfi_fastcharge` (in `/etc/modprobe.d/`) — this kernel module claims the
  device.
- `/dev/bus/usb/*` is root-only, so **run the whole program under `sudo`**. `sudo` resets
  the environment:
  logs need `sudo env ALLPLAY_LOG=…`, and the state directory must be given explicitly
  (otherwise `HOME=/root`, a separate local identity is created, and the phone has to be
  paired again every time).

### 10.2 USB side: switch into the CarPlay configuration

- An iPhone normally enumerates **one** configuration (PTP). Only **after unlocking** does
  it expose the CarPlay configuration (value=6 measured on device).
  → the test is `bNumConfigurations <= 1`; **"this phone has no CarPlay configuration" is
  usually a locked screen, not a cable problem**.
- That configuration exposes two interfaces: the **USBMUX port** (the carrier for iAP2;
  bulk in/out wrapped as `IByteStream`) and
  the **NCM control/data port** (hand it back to the kernel; do not detach it yourself).
  Read interface numbers from the descriptors; do not hard-code them.
- After switching configurations the device re-enumerates (bus/addr change); wait for it
  to come back before opening it.

### 10.3 Control plane: three mini TCP connections

The phone implements a TCP stack on its side, so framing is up to us (`usbmux v2`:
version negotiation + setup + connect request).

1. **Plaintext lockdown `:62078`** — `SetValue(UntrustedHostBUID)` →
   `GetValue(DevicePublicKey)` → `GetValue(WiFiAddress)` → `Pair` (**plaintext, not SRP**;
   SRP is the AirPlay pair-setup). Messages are **XML plist** (not this project's
   `core/bplist` binary format), prefixed with a 4-byte big-endian length. Close after
   pairing.
2. **Upgrade the same connection to TLS in place** — `StartSession` → upgrade →
   `StartService(com.apple.carkit.service)`.
   This connection **must stay open**; if it drops, the third one dies too. The client
   identity is the **root certificate**, not the host one.
3. The port returned by `StartService` — **a new connection**, optionally TLS again.
   **This is the iAP2 stream.**
   The first byte on the stream is an iAP2 link frame; there is no additional handshake or
   encapsulation in between.

The pairing record (HostID / SystemBUID / certificate) is **currently created fresh on
every run**, so the phone asks you to tap "Trust" each time; persisting a pair record
would remove that step.

### 10.4 iAP2 link: on wired, the **host** initiates

After `FF 55 02 00 EE 10` (a bare 6-byte marker, **with no length or encapsulation
before it**):

- **You must immediately send SYN yourself and resend every 500ms**, until the phone's SYN
  arrives.
  Sending only the marker leaves the phone **completely silent** (measured: 260 seconds
  of zero bytes) — on the wireless path the phone sends the marker first and only then do
  we enter negotiation; copying that is fatal.
- Link parameters (following DiPlay's `WIRED_LINK_CONFIG`): `maxOutgoing=4`, control
  session version 2, **zero-acknowledgement declared**
  (USB is inherently reliable).

In identification, the **USBHostTransport interface number** in `0x1D01` must be the
USBMUX port's `bInterfaceNumber`
(measured = 1); otherwise the phone rejects it outright (`0x1D03`). After the link reaches
NORMAL, do identification **immediately** — waiting ~44 seconds
makes the phone reset USB entirely.

### 10.5 Media plane: kernel `cdc_ncm`, not us

After switching into the CarPlay configuration, the kernel binds the CDC-NCM function and
creates a real NIC itself (measured: `cdc_ncm 1-3:6.3 usb0: register …`).
**NTB16 / Ethernet / IPv6 need not be written at all**; AirPlay can bind directly to it.
Two traps:

- The kernel **generates no IPv6 address** on this NIC (`addr_gen_mode=1`), so you must
  pin it yourself; and NetworkManager treats this point-to-point link as a "wired
  connection" and asks for DHCP (there is no server), leaving it stuck in connecting.
  → `nmcli device set <if> managed no` → `ip link set up` → `ip addr add fe80::2/64 …
  nodad`.
- The NIC name derives from the MAC (`enx…`) and **must not be hard-coded**. Look it up by
  USB `busnum`/`devnum` via
  `/sys/class/net/*/device/../{busnum,devnum}`.
- The link-local address **must include `sin6_scope_id` in `bind()`**, otherwise EINVAL
  (which looks like a malformed argument).

### 10.6 `0x4300` / `0x4301`

- `0x4300 CarPlayAvailability`: `group(0)`=wired, `group(1)`=wireless,
  `group(2)`=themeAssets;
  within each group `u8(0)=available or not`, `str(1)=identifier`. On device, it reports
  wired available (identifier = phone UDID) and wireless available (identifier = Wi-Fi
  MAC).
- `0x4301 CarPlayStartSession`: `group(0)` wired — inside it **each address is a separate
  string parameter with repeated id 0**
  (**not a string list**; this is the easiest place to build it wrong); followed by
  `u32(2)=AirPlay port`, optional `str(3)=deviceIdentifier`,
  `str(4)=Ed25519 public key hex`, `str(5)=sourceVersion`.
- The public key must be the **persisted one** (`core::DeviceIdentity`) — it is what the
  phone will recognize during pair-verify.
- On receiving `0x4300`, reply `0x4301` **without filtering on `available`** (DiPlay also
  replies on receipt; let the phone decide availability).
- The reported address **must be the one the Receiver actually bound and listens on**, and
  so must the port. Announcing one thing while listening on another
  means the phone's very first action after connecting will mismatch.

### 10.7 A trap that "looks like a network failure"

The control plane is all green, `/pair-verify` passed, `RECORD` happened, and then the
phone POSTs `/feedback` every 2 seconds,
**but the picture never arrives**. The root cause is not the network: in `SETUP` the phone
asks for the screen stream and we do not provide `dataPort`
(no stream is created when `IVideoOutput` is null), so the phone has no data port and can
only wait.
