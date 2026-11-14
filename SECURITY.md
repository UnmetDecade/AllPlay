# Security policy

<div align="left"><b>English</b> · <a href="SECURITY.zh-CN.md">简体中文</a></div>

## Supported versions

AllPlay is a protocol implementation, not a hardened appliance. Security fixes land on
`main` and in the latest tagged release.

| Version | Supported |
| --- | --- |
| `main` (latest release) | ✅ |
| Older tags | ❌ — upgrade to the latest release |

## Reporting a vulnerability

**Do not open a public issue for a security problem.**

Use GitHub's private advisory flow:

1. Open <https://github.com/UnmetDecade/AllPlay/security/advisories/new>.
2. Describe the impact, the affected component (`core/`, `platform/`, `host/`, `android/`),
   the version or commit, and the steps to reproduce.
3. If the report involves a crash or memory corruption, include the sanitizer output
   (ASan/UBSan) when you have it.

You should get an initial response within 7 days. If the report is valid, we will agree on
a disclosure date, prepare the fix on a private branch, and publish an advisory together
with the release notes.

## Scope

In scope:

- Memory-safety bugs in the protocol parsers (`core/projection/**`), the bplist parser,
  the TLV8/plist codecs, and the crypto wrappers.
- Authentication or pairing bypasses in the iAP2, MFi-SAP, AirPlay, or Android Auto
  handshakes.
- Denial of service reachable from a paired or unpaired peer on the local network or USB.
- Credential handling bugs that expose the MFi identity or pairing records.

Out of scope:

- Missing Apple MFi authorization. AllPlay is an independent implementation; see
  `THIRD_PARTY_NOTICES.md`.
- Vulnerabilities in upstream libraries (OpenSSL, ffmpeg, SDL2, ClamAV-free stack) that
  are already public — report them upstream, and open a normal issue here to bump the
  dependency.
- Attacks that require root on the host, physical access to `identity/`, or a modified
  build.

## Handling credentials

`identity/` holds the Apple-issued MFi private key and certificate. It is git-ignored and
must never be committed, pasted into an issue, or attached to a release. Treat any
credential appearing in a log or crash dump as compromised and rotate it.
