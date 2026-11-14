# Contributing to AllPlay

<div align="left"><b>English</b> · <a href="CONTRIBUTING.zh-CN.md">简体中文</a></div>

Thanks for helping. AllPlay is a protocol implementation with a hard rule: **`core/` is
platform-independent and stays that way.** Everything else is an adapter.

## Before you start

- Read [`docs/architecture.md`](docs/architecture.md) and, if you touch a projection
  backend, [`docs/projection-backend.md`](docs/projection-backend.md).
- Protocol behavior changes belong in [`docs/PROTOCOL-NOTES.md`](docs/PROTOCOL-NOTES.md) —
  cite the observation or upstream reference, do not guess.
- Security issues do **not** go through issues or pull requests; see
  [`SECURITY.md`](SECURITY.md).

## Build and test

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

The desktop host is skipped automatically when SDL2 is missing; this is expected on a
headless machine.

### Android

The Android host is built from Android Studio or with the Android SDK/NDK and a
cross-compiled OpenSSL (`OPENSSL_BASE_DIR`). CI does not build it because the NDK and
cross-compiled OpenSSL are not available on GitHub-hosted runners. Please build
`android/` locally before sending a PR that touches it.

### Core-only build (fast, no system GUI/Bluetooth stack)

```sh
cmake -S . -B build-core -DALLPLAY_CORE_ONLY=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core -j"$(nproc)"
./build-core/iap2_test && ./build-core/m3_test && ./build-core/aa_wire_test
```

This is the configuration CI uses for the required `core` check.

## Pull requests

- One logical change per PR, with a conventional prefix in the title
  (`core:`, `host:`, `android:`, `docs:`, `ci:`).
- Tests: add or update a self-test under `tools/` when behavior changes. A PR without a
  test needs a sentence explaining why the change is untestable.
- Documentation: user-visible behavior changes update `README.md` **and**
  `README.zh-CN.md`; version-worthy changes add an entry under `[Unreleased]` in
  `CHANGELOG.md` **and** `CHANGELOG.zh-CN.md`.
- All required checks must pass. `main` is protected: no direct pushes, no force pushes,
  no branch deletion.
- Never commit `identity/`, pairing records, signing keys, or credentials. The
  `.gitignore` covers the usual extensions; check `git diff --cached` before committing.

## Commit messages

Use the imperative mood and say what changed and why, for example:

```text
core: reject AAP frames whose declared length exceeds the buffer

The old parser trusted the length field, so a malformed frame could read past
the end of the receive buffer. Add the bounds check and a regression case in
aa_wire_test.
```

English is the default for commit messages, comments, log output, and documentation.
Chinese documentation is maintained as `.zh-CN.md` counterparts.

## Code style

- C++17, 4 spaces, LF, final newline (`.editorconfig`).
- `core/` must not include platform headers or link platform libraries.
- New third-party code needs a license entry in `THIRD_PARTY_NOTICES.md` and its
  `.zh-CN.md`.

## Releases

Releases are cut from `main` by tagging; see `CHANGELOG.md` for the version history. A
`v*` tag runs the release job, which attaches a source archive and `SHA256SUMS` to the
GitHub release.
