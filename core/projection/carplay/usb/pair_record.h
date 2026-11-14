/**
 * @file   pair_record.h
 * @brief  Local certificate material for wired pairing (root / host / device)
 * @author blt
 */

// ============================================================================
// core/usb/pair_record.h — self-generated certificates for wired pairing
// ============================================================================
// Wired pairing is **not** the wireless SRP scheme (pair-setup/pair-verify); it is a plaintext Pair,
// with all certificates **generated locally on the spot**; the phone's public key is only used to "sign a certificate belonging to it":
//
//   root   — self-signed (empty DN, serial 0, CA:TRUE). Goes into the Pair request; **it is also the TLS client identity afterward**
//   host   — signed by root (empty DN, CA:FALSE)
//   device — signed by root, but **the public key in the certificate is the one the phone gave via GetValue(DevicePublicKey)**
//             (so the private key lives on the phone; we cannot sign anything with it)
//
// Shape copied from DiPlay: RSA-2048 / SHA256withRSA / 10-year validity / empty issuer and subject DN /
// serial always 0. **Do not add a CN or change the serial on your own** — whether the phone accepts it was determined empirically; any change requires re-testing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core::usb {

/// All local material needed for one pairing
struct WiredPairIdentity {
    std::string rootCertPem;    ///< self-signed root certificate (PEM text)
    std::string rootKeyPem;     ///< root private key (PKCS#8 PEM) — used for the TLS client identity
    std::string hostCertPem;    ///< host certificate (PEM text)
    std::string hostKeyPem;
    std::string deviceCertPem;  ///< device certificate signed for the phone (PEM text)
    std::string hostId;         ///< uppercase UUID
    std::string systemBuid;     ///< uppercase UUID
};

/// Generate pairing material. devicePublicKey = raw bytes from lockdown GetValue("DevicePublicKey").
///
/// **Do not assume it is DER**: docs and reference implementations say PKCS#1 RSAPublicKey DER, but on the real device
/// we get 431 bytes of **PEM text**. This function detects the format itself (accepts PEM / PKCS#1 / SPKI),
/// so callers can pass it through unchanged; do not transcode it beforehand.
///
/// hostId / systemBuid are supplied by the caller: **SetValue(UntrustedHostBUID) must match
/// the SystemBUID written in the later Pair**, so this pair of UUIDs must be fixed before the pairing flow starts. Empty strings are generated on the spot.
bool makeWiredPairIdentity(const std::vector<uint8_t>& devicePublicKey, const std::string& hostId,
                           const std::string& systemBuid, WiredPairIdentity* out,
                           std::string* err);

/// Uppercase UUID (used for both HostID / SystemBUID; the phone wants uppercase)
std::string makeUuidUpper();

// ----------------------------------------------------------------------------
// Persistence / reload: **avoid tapping "Trust" on the phone every time**
// ----------------------------------------------------------------------------
// iOS identifies "this computer" by **HostID + this certificate set** (it keeps its own copy of the pair record).
// Generating a new set on every run makes the phone treat us as a stranger each time → it shows "Trust this computer?" every time.
// Store and reuse it, and the prompt should appear only the first time.
//
// key is the **phone serial number** (available in the USB descriptor), not DevicePublicKey — that is impossible order-wise:
// before Pair we must first send SetValue(UntrustedHostBUID), and that BUID lives in this record,
// but at that point DevicePublicKey has not been read yet.
//
// Directory: <stateDir>/wired-pair/<key>/ (empty stateDir = defaultStateDir()).

/// Read back only HostID / SystemBUID (needed for reporting UntrustedHostBUID). Returns false when there is no record; not an error.
bool loadWiredPairIds(const std::string& stateDir, const std::string& key, std::string* hostId,
                      std::string* systemBuid);

/// Read back the whole set. **If devicePublicKey does not match, treat it as absent** (phone swapped, phone reinstalled, record stale):
/// in that case *found returns false and the caller generates a new set and overwrites.
bool loadWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, WiredPairIdentity* out,
                           bool* found, std::string* err);

/// Persist to disk (private keys 0600). Returns false on failure — but **the caller should not fail the whole pairing flow because of it**:
/// at worst it means one more trust prompt next time.
bool saveWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, const WiredPairIdentity& id,
                           std::string* err);

/// Read only the certificates (**no phone public key verification**). Used by the "skip Pair" fast path:
/// on that path the device certificate is not needed at all (it only appears in PairRecord), and verifying the fingerprint would require
/// first requesting DevicePublicKey from the phone — the whole point of the fast path is not to.
bool loadWiredPairCerts(const std::string& stateDir, const std::string& key, WiredPairIdentity* out,
                        bool* found, std::string* err);

// ---- Mark that "this material pair has paired successfully" ------------------
// Why record this separately: **pairing success ≠ a reusable record**. Last time we got stuck at the prompt, the material was already
// persisted (saved at that point, otherwise HostID could not be fixed), but the phone never accepted it —
// using such a record to skip Pair means opening an encrypted session with certificates the phone has never seen, which is bound to fail.
// So only **a genuinely successful pairing** gets this stamp, and only then may the next round skip Pair.
bool markWiredPairTrusted(const std::string& stateDir, const std::string& key, std::string* err);
bool isWiredPairTrusted(const std::string& stateDir, const std::string& key);

}  // namespace core::usb
