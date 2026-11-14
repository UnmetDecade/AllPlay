/**
 * @file   pair_record.cpp
 * @brief  Implementation of local certificate material for wired pairing
 * @author blt
 */

#include "usb/pair_record.h"

#include "common/log.h"
#include "platform/device_identity.h"   // defaultStateDir()

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/decoder.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

namespace core::usb {
namespace {

constexpr const char* kTag = "pair";
constexpr int         kRsaBits = 2048;
/// 10-year validity (3650 days, matching DiPlay)
constexpr long        kValiditySeconds = 3650L * 24 * 60 * 60;
/// Shift notBefore back one day so a slightly slow phone clock won't judge it "not yet valid".
/// This only moves time, not any structural field the phone can see.
constexpr long        kBackdateSeconds = 24 * 60 * 60;

std::string opensslError()
{
    std::string out;
    unsigned long e = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256] = "";
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) out += " / ";
        out += buf;
    }
    return out.empty() ? "(OpenSSL provided no error code)" : out;
}

bool fail(std::string* err, const std::string& what)
{
    if (err != nullptr) *err = what + ": " + opensslError();
    return false;
}

std::string bioToString(BIO* b)
{
    char*  p = nullptr;
    const long n = BIO_get_mem_data(b, &p);
    return (p != nullptr && n > 0) ? std::string(p, size_t(n)) : std::string();
}

EVP_PKEY* genRsa(int bits, std::string* err)
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (ctx == nullptr) {
        fail(err, "Failed to create RSA key context");
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        fail(err, "Failed to generate RSA key");
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

/// Include the first few bytes on error — this section is all byte-level conventions; "can't decode"
/// alone carries no information, while the first 16 bytes immediately tell DER (0x30), PEM text ('-'), or something else.
std::string hexPreview(const std::vector<uint8_t>& v, size_t n)
{
    static const char* kHex = "0123456789abcdef";
    std::string        s;
    const size_t       k = v.size() < n ? v.size() : n;
    for (size_t i = 0; i < k; ++i) {
        if (i != 0) s += ' ';
        s += kHex[v[i] >> 4];
        s += kHex[v[i] & 0x0f];
    }
    if (v.size() > k) s += " …";
    return s;
}

/// Decode the phone-supplied DevicePublicKey into an EVP_PKEY.
/// Docs/reference implementations say **PKCS#1 RSAPublicKey DER**, but the real device gives **PEM text**
/// (431 bytes), so try all three forms: PEM text, PKCS#1, SubjectPublicKeyInfo.
EVP_PKEY* parseDevicePublicKey(const std::vector<uint8_t>& der, std::string* err)
{
    if (der.empty()) {
        if (err != nullptr) *err = "DevicePublicKey from the phone is empty";
        return nullptr;
    }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    struct Candidate {
        const char* inputType;
        const char* structure;  ///< nullptr = let the decoder detect it
    };
    // Ordered by likelihood. PEM comes first because it only costs one string comparison.
    const Candidate kCandidates[] = {
        {"PEM", nullptr},              // in case it is PEM text (PEM inside <data> is not unheard of)
        {"DER", "type-specific"},      // the documented PKCS#1 RSAPublicKey
        {"DER", "SubjectPublicKeyInfo"},
    };
    for (const Candidate& c : kCandidates) {
        EVP_PKEY*         pkey = nullptr;
        OSSL_DECODER_CTX* dctx = OSSL_DECODER_CTX_new_for_pkey(
            &pkey, c.inputType, c.structure, "RSA", EVP_PKEY_PUBLIC_KEY, nullptr, nullptr);
        if (dctx == nullptr) continue;
        const unsigned char* p = der.data();
        size_t               len = der.size();
        const int            ok = OSSL_DECODER_from_data(dctx, &p, &len);
        OSSL_DECODER_CTX_free(dctx);
        if (ok == 1 && pkey != nullptr) {
            LOG_INFO("[%s] Decoded DevicePublicKey as %s/%s", kTag, c.inputType,
                     c.structure != nullptr ? c.structure : "auto");
            return pkey;
        }
        EVP_PKEY_free(pkey);
    }
#else
    const std::string asText(der.begin(), der.end());
    if (asText.rfind("-----BEGIN", 0) == 0) {
        BIO* b = BIO_new_mem_buf(asText.data(), int(asText.size()));
        EVP_PKEY* k = b != nullptr ? PEM_read_bio_PUBKEY(b, nullptr, nullptr, nullptr) : nullptr;
        if (b != nullptr) BIO_free(b);
        if (k != nullptr) return k;
    }
    const unsigned char* p = der.data();
    RSA* rsa = d2i_RSAPublicKey(nullptr, &p, long(der.size()));
    if (rsa == nullptr) {
        p = der.data();
        rsa = d2i_RSA_PUBKEY(nullptr, &p, long(der.size()));  // fallback: SPKI
    }
    if (rsa != nullptr) {
        EVP_PKEY* pkey = EVP_PKEY_new();
        if (pkey != nullptr && EVP_PKEY_assign_RSA(pkey, rsa) == 1) return pkey;
        EVP_PKEY_free(pkey);
        RSA_free(rsa);
    }
#endif

    if (err != nullptr) {
        *err = "Cannot decode DevicePublicKey from the phone (" + std::to_string(der.size()) + " bytes, starts with " +
               hexPreview(der, 16) + "): " + opensslError();
    }
    return nullptr;
}

/// Add an extension. value uses the openssl.cnf syntax (like "critical,CA:TRUE").
bool addExt(X509* x, X509V3_CTX* v3, int nid, const char* value, std::string* err)
{
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, v3, nid, const_cast<char*>(value));
    if (ext == nullptr) return fail(err, std::string("Failed to build extension ") + value);
    const int rc = X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    if (rc != 1) return fail(err, std::string("Failed to add extension ") + value);
    return true;
}

/// Sign one certificate. subjectKey is "the public key written into the certificate" — for the device cert, the phone's key.
X509* makeCert(EVP_PKEY* subjectKey, EVP_PKEY* signKey, bool isCa, bool withSki, std::string* err)
{
    X509* x = X509_new();
    if (x == nullptr) {
        fail(err, "X509_new failed");
        return nullptr;
    }
    X509_set_version(x, 2);  // v3: the phone may reject certs without extensions
    ASN1_INTEGER_set(X509_get_serialNumber(x), 0);  // always 0, copied from DiPlay
    // Shift the whole window back one day (notBefore and notAfter together) so the **span is still exactly 10 years**
    X509_gmtime_adj(X509_getm_notBefore(x), -kBackdateSeconds);
    X509_gmtime_adj(X509_getm_notAfter(x), kValiditySeconds - kBackdateSeconds);
    if (X509_set_pubkey(x, subjectKey) != 1) {
        X509_free(x);
        fail(err, "X509_set_pubkey failed");
        return nullptr;
    }

    // Empty DN: both issuer and subject are empty NAMEs (not "unset")
    X509_NAME* empty = X509_NAME_new();
    if (empty == nullptr) {
        X509_free(x);
        fail(err, "X509_NAME_new failed");
        return nullptr;
    }
    const int s1 = X509_set_subject_name(x, empty);
    const int s2 = X509_set_issuer_name(x, empty);
    X509_NAME_free(empty);
    if (s1 != 1 || s2 != 1) {
        X509_free(x);
        fail(err, "Failed to set the empty DN");
        return nullptr;
    }

    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    // x must be passed in: subjectKeyIdentifier's "hash" is computed from the certificate's public key
    X509V3_set_ctx(&v3, nullptr, x, nullptr, nullptr, 0);

    if (!addExt(x, &v3, NID_basic_constraints, isCa ? "critical,CA:TRUE" : "critical,CA:FALSE",
                err)) {
        X509_free(x);
        return nullptr;
    }
    if (!isCa) {
        // Leaf certificate keyUsage: digitalSignature + keyEncipherment (encodes to exactly 0xA0/5)
        if (!addExt(x, &v3, NID_key_usage, "critical,digitalSignature,keyEncipherment", err)) {
            X509_free(x);
            return nullptr;
        }
    }
    if (withSki) {
        // Only the device cert has this, and it is **not critical**. "hash" = SHA-1 of the public key PKCS#1
        if (!addExt(x, &v3, NID_subject_key_identifier, "hash", err)) {
            X509_free(x);
            return nullptr;
        }
    }

    if (X509_sign(x, signKey, EVP_sha256()) <= 0) {
        X509_free(x);
        fail(err, "X509_sign failed");
        return nullptr;
    }
    return x;
}

std::string certToPem(X509* x)
{
    BIO* b = BIO_new(BIO_s_mem());
    if (b == nullptr) return std::string();
    PEM_write_bio_X509(b, x);
    const std::string out = bioToString(b);
    BIO_free(b);
    return out;
}

/// PKCS#8 ("-----BEGIN PRIVATE KEY-----"). **Do not use traditional PKCS#1 "RSA PRIVATE KEY"**;
/// DiPlay uses PKCS#8, and the phone may not treat the two alike.
std::string keyToPem(EVP_PKEY* k)
{
    BIO* b = BIO_new(BIO_s_mem());
    if (b == nullptr) return std::string();
    PEM_write_bio_PKCS8PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
    const std::string out = bioToString(b);
    BIO_free(b);
    return out;
}

}  // namespace

std::string makeUuidUpper()
{
    uint8_t b[16] = {};
    if (RAND_bytes(b, int(sizeof b)) != 1) {
        // Should never get here. If it does, don't return an empty string — the phone rejects an empty HostID outright.
        LOG_WARN("[%s] RAND_bytes failed; HostID degraded (%s)", kTag, opensslError().c_str());
        const uint64_t t = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        std::memcpy(b, &t, sizeof t);
        std::memcpy(b + 8, &t, sizeof t);
    }
    b[6] = uint8_t((b[6] & 0x0f) | 0x40);  // version 4
    b[8] = uint8_t((b[8] & 0x3f) | 0x80);  // variant

    static const char* kHex = "0123456789ABCDEF";
    std::string        s;
    s.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 0x0f];
    }
    return s;
}

bool makeWiredPairIdentity(const std::vector<uint8_t>& devicePublicKey, const std::string& hostId,
                           const std::string& systemBuid, WiredPairIdentity* out,
                           std::string* err)
{
    if (out == nullptr) {
        if (err != nullptr) *err = "out is null";
        return false;
    }

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> rootKey(genRsa(kRsaBits, err),
                                                                EVP_PKEY_free);
    if (rootKey == nullptr) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> hostKey(genRsa(kRsaBits, err),
                                                                EVP_PKEY_free);
    if (hostKey == nullptr) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> devKey(
        parseDevicePublicKey(devicePublicKey, err), EVP_PKEY_free);
    if (devKey == nullptr) return false;

    std::unique_ptr<X509, decltype(&X509_free)> rootCert(
        makeCert(rootKey.get(), rootKey.get(), /*isCa=*/true, /*withSki=*/false, err), X509_free);
    if (rootCert == nullptr) return false;
    std::unique_ptr<X509, decltype(&X509_free)> hostCert(
        makeCert(hostKey.get(), rootKey.get(), /*isCa=*/false, /*withSki=*/false, err), X509_free);
    if (hostCert == nullptr) return false;
    // device certificate: public key is the phone's, signer is still our root; only it carries a SKI
    std::unique_ptr<X509, decltype(&X509_free)> devCert(
        makeCert(devKey.get(), rootKey.get(), /*isCa=*/false, /*withSki=*/true, err), X509_free);
    if (devCert == nullptr) return false;

    WiredPairIdentity id;
    id.rootCertPem = certToPem(rootCert.get());
    id.rootKeyPem = keyToPem(rootKey.get());
    id.hostCertPem = certToPem(hostCert.get());
    id.hostKeyPem = keyToPem(hostKey.get());
    id.deviceCertPem = certToPem(devCert.get());
    id.hostId = hostId.empty() ? makeUuidUpper() : hostId;
    id.systemBuid = systemBuid.empty() ? makeUuidUpper() : systemBuid;

    if (id.rootCertPem.empty() || id.rootKeyPem.empty() || id.hostCertPem.empty() ||
        id.hostKeyPem.empty() || id.deviceCertPem.empty()) {
        if (err != nullptr) *err = "Certificate-to-PEM conversion produced an empty string";
        return false;
    }

    LOG_INFO("[%s] Certificates generated (RSA-%d, one each for root/host/device), HostID=%s", kTag, kRsaBits,
             id.hostId.c_str());
    *out = std::move(id);
    return true;
}

// ---------------------------------------------------------------------------
// Persistence / reload
// ---------------------------------------------------------------------------
namespace {

namespace fs = std::filesystem;

/// Directory name. The serial is alphanumeric anyway, but it is an external string used directly in a path, so sanitize it.
std::string safeKey(const std::string& key)
{
    std::string s;
    s.reserve(key.size());
    for (char c : key) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
        s += ok ? c : '_';
    }
    if (s.empty() || s == "." || s == "..") s = "unknown";
    return s;
}

/// Fingerprint of the phone public key. **Used only to tell whether it is still the same phone**; never in any packet, never affects pairing.
std::string deviceFingerprint(const std::vector<uint8_t>& pk)
{
    unsigned char md[EVP_MAX_MD_SIZE] = {};
    unsigned int  n = 0;
    if (pk.empty() || EVP_Digest(pk.data(), pk.size(), md, &n, EVP_sha256(), nullptr) != 1)
        return std::string();
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.reserve(size_t(n) * 2);
    for (unsigned int i = 0; i < n; ++i) {
        out += kHex[md[i] >> 4];
        out += kHex[md[i] & 0x0f];
    }
    return out;
}

std::string pairDir(const std::string& stateDir, const std::string& key)
{
    const std::string base = stateDir.empty() ? core::defaultStateDir() : stateDir;
    return (fs::path(base) / "wired-pair" / safeKey(key)).string();
}

bool readWholeFile(const std::string& p, std::string* out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return false;
    *out = std::move(s);
    return true;
}

bool writeWholeFile(const std::string& p, const std::string& data, bool secret)
{
    FILE* f = std::fopen(p.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0) return false;
    if (!ok) return false;
    if (secret) {
        // Best effort: failing to set permissions does not affect functionality (this is our own key, not an Apple-issued credential)
        std::error_code ec;
        fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
    }
    return true;
}

}  // namespace

bool loadWiredPairIds(const std::string& stateDir, const std::string& key, std::string* hostId,
                      std::string* systemBuid)
{
    std::string ids;
    if (!readWholeFile((fs::path(pairDir(stateDir, key)) / "ids.txt").string(), &ids)) return false;

    std::istringstream in(ids);
    std::string        h, b;
    std::getline(in, h);
    std::getline(in, b);
    if (h.empty() || b.empty()) return false;
    if (hostId != nullptr) *hostId = h;
    if (systemBuid != nullptr) *systemBuid = b;
    return true;
}

bool loadWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, WiredPairIdentity* out,
                           bool* found, std::string* err)
{
    if (found != nullptr) *found = false;
    if (out == nullptr) {
        if (err != nullptr) *err = "out is null";
        return false;
    }

    const std::string dir = pairDir(stateDir, key);

    // Fingerprint: whether the stored phone public key matches the one read now. A mismatch means another phone
    // (or this phone was reinstalled) — the record is invalid for it, treat as absent and re-pair.
    std::string storedFp;
    if (!readWholeFile((fs::path(dir) / "device.fp").string(), &storedFp)) return true;  // never stored
    // Written with a trailing newline (easier to cat); strip it before comparing — otherwise "abc\n" != "abc"
    // and the fingerprint never matches, as if nothing was stored.
    while (!storedFp.empty() && (storedFp.back() == '\n' || storedFp.back() == '\r'))
        storedFp.pop_back();

    const std::string wantFp = deviceFingerprint(devicePublicKey);
    if (wantFp.empty() || storedFp != wantFp) {
        LOG_INFO("[%s] Stored pairing material does not belong to this phone (public key fingerprint mismatch); generating a new set", kTag);
        return true;
    }

    WiredPairIdentity id;
    if (!loadWiredPairCerts(stateDir, key, &id, found, err)) return false;
    if (!*found) return true;
    if (!loadWiredPairIds(stateDir, key, &id.hostId, &id.systemBuid)) {
        if (found != nullptr) *found = false;
        return true;
    }

    *out = std::move(id);
    return true;
}

bool loadWiredPairCerts(const std::string& stateDir, const std::string& key, WiredPairIdentity* out,
                        bool* found, std::string* err)
{
    if (found != nullptr) *found = false;
    if (out == nullptr) {
        if (err != nullptr) *err = "out is null";
        return false;
    }

    const std::string dir = pairDir(stateDir, key);

    WiredPairIdentity id;
    const bool ok =
        readWholeFile((fs::path(dir) / "root.crt").string(), &id.rootCertPem) &&
        readWholeFile((fs::path(dir) / "root.key").string(), &id.rootKeyPem) &&
        readWholeFile((fs::path(dir) / "host.crt").string(), &id.hostCertPem) &&
        readWholeFile((fs::path(dir) / "host.key").string(), &id.hostKeyPem) &&
        readWholeFile((fs::path(dir) / "device.crt").string(), &id.deviceCertPem);
    if (!ok || id.rootCertPem.empty() || id.rootKeyPem.empty() || id.hostCertPem.empty() ||
        id.hostKeyPem.empty() || id.deviceCertPem.empty()) {
        // A partial record is worse than none (it would open a session with half a set); treat as absent so the caller regenerates and overwrites.
        LOG_WARN("[%s] Pairing material under %s is incomplete; treating it as absent", kTag, dir.c_str());
        return true;
    }

    *found = true;
    *out = std::move(id);
    return true;
}

bool markWiredPairTrusted(const std::string& stateDir, const std::string& key, std::string* err)
{
    const std::string dir = pairDir(stateDir, key);
    std::error_code   ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (err != nullptr) *err = "Failed to create pairing material directory: " + dir + " (" + ec.message() + ")";
        return false;
    }
    // The content is only for humans (when pairing succeeded); the criterion is "does the file exist".
    const auto  now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string stamp = std::to_string(now) + "\n";
    if (!writeWholeFile((fs::path(dir) / "paired").string(), stamp, false)) {
        if (err != nullptr) *err = "Failed to write the paired marker: " + dir;
        return false;
    }
    return true;
}

bool isWiredPairTrusted(const std::string& stateDir, const std::string& key)
{
    std::string s;
    return readWholeFile((fs::path(pairDir(stateDir, key)) / "paired").string(), &s);
}

bool saveWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, const WiredPairIdentity& id,
                           std::string* err)
{
    const std::string dir = pairDir(stateDir, key);

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (err != nullptr) *err = "Failed to create pairing material directory: " + dir + " (" + ec.message() + ")";
        return false;
    }

    const auto f = [&dir](const char* n) { return (fs::path(dir) / n).string(); };
    const std::string fp = deviceFingerprint(devicePublicKey);

    // Write ids.txt first: it holds HostID / SystemBUID, which the next round's SetValue(UntrustedHostBUID)
    // relies on. The two private key files are marked 0600 individually.
    const bool ok = !fp.empty() && writeWholeFile(f("ids.txt"), id.hostId + "\n" + id.systemBuid + "\n", false) &&
                    writeWholeFile(f("device.fp"), fp + "\n", false) &&
                    writeWholeFile(f("root.crt"), id.rootCertPem, false) &&
                    writeWholeFile(f("root.key"), id.rootKeyPem, true) &&
                    writeWholeFile(f("host.crt"), id.hostCertPem, false) &&
                    writeWholeFile(f("host.key"), id.hostKeyPem, true) &&
                    writeWholeFile(f("device.crt"), id.deviceCertPem, false);
    if (!ok) {
        if (err != nullptr) *err = "Failed to write pairing material: " + dir;
        return false;
    }
    LOG_INFO("[%s] Pairing material saved to %s - the same phone will not prompt for trust next time", kTag, dir.c_str());
    return true;
}

}  // namespace core::usb
