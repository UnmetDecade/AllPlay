/**
 * @file   pair_record.cpp
 * @brief  有线配对要用的本地证书材料的实现
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
/// 有效期 10 年（按 3650 天算，跟 DiPlay 一致）
constexpr long        kValiditySeconds = 3650L * 24 * 60 * 60;
/// notBefore 往前挪一天：手机要是时钟慢一点，不至于判成"还没生效"。
/// 这只动时间，不动手机能看见的任何结构字段。
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
    return out.empty() ? "（OpenSSL 没给错误码）" : out;
}

bool fail(std::string* err, const std::string& what)
{
    if (err != nullptr) *err = what + "：" + opensslError();
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
        fail(err, "建 RSA 密钥上下文失败");
        return nullptr;
    }
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        fail(err, "生成 RSA 密钥失败");
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

/// 出错时把开头几个字节带上 —— 这一段全是字节级约定，"解不开"这三个字本身没有
/// 任何信息量，而头 16 字节立刻能分出是 DER（0x30）、PEM 文本（'-'）还是别的。
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

/// 把手机给的 DevicePublicKey 解成 EVP_PKEY。
/// 文档/参照实现都说这里是 **PKCS#1 RSAPublicKey DER**，但真机实测拿到的是 **PEM 文本**
/// （431 字节），所以三种形态都试：PEM 文本、PKCS#1、SubjectPublicKeyInfo。
EVP_PKEY* parseDevicePublicKey(const std::vector<uint8_t>& der, std::string* err)
{
    if (der.empty()) {
        if (err != nullptr) *err = "手机给的 DevicePublicKey 是空的";
        return nullptr;
    }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    struct Candidate {
        const char* inputType;
        const char* structure;  ///< nullptr = 让解码器自己认
    };
    // 顺序按可能性排。PEM 那条放最前面是因为成本只有一次字符串比较。
    const Candidate kCandidates[] = {
        {"PEM", nullptr},              // 万一是 PEM 文本（<data> 里塞 PEM 并非没有先例）
        {"DER", "type-specific"},      // 文档说的 PKCS#1 RSAPublicKey
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
            LOG_INFO("[%s] DevicePublicKey 按 %s/%s 解出来了", kTag, c.inputType,
                     c.structure != nullptr ? c.structure : "自动");
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
        rsa = d2i_RSA_PUBKEY(nullptr, &p, long(der.size()));  // 退而求其次：SPKI
    }
    if (rsa != nullptr) {
        EVP_PKEY* pkey = EVP_PKEY_new();
        if (pkey != nullptr && EVP_PKEY_assign_RSA(pkey, rsa) == 1) return pkey;
        EVP_PKEY_free(pkey);
        RSA_free(rsa);
    }
#endif

    if (err != nullptr) {
        *err = "手机给的 DevicePublicKey 解不开（" + std::to_string(der.size()) + " 字节，开头 " +
               hexPreview(der, 16) + "）：" + opensslError();
    }
    return nullptr;
}

/// 加一条扩展。value 用的是 openssl.cnf 里那套写法（"critical,CA:TRUE" 这种）。
bool addExt(X509* x, X509V3_CTX* v3, int nid, const char* value, std::string* err)
{
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, v3, nid, const_cast<char*>(value));
    if (ext == nullptr) return fail(err, std::string("造扩展 ") + value + " 失败");
    const int rc = X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    if (rc != 1) return fail(err, std::string("挂扩展 ") + value + " 失败");
    return true;
}

/// 签一张证书。subjectKey 是"证书里写的公钥"——device 证书传的是手机那把。
X509* makeCert(EVP_PKEY* subjectKey, EVP_PKEY* signKey, bool isCa, bool withSki, std::string* err)
{
    X509* x = X509_new();
    if (x == nullptr) {
        fail(err, "X509_new 失败");
        return nullptr;
    }
    X509_set_version(x, 2);  // v3：不加扩展的手机可能不认
    ASN1_INTEGER_set(X509_get_serialNumber(x), 0);  // 恒 0，照抄 DiPlay
    // 整段窗口往前平移一天（notBefore 与 notAfter 一起挪），这样**跨度仍是整整 10 年**
    X509_gmtime_adj(X509_getm_notBefore(x), -kBackdateSeconds);
    X509_gmtime_adj(X509_getm_notAfter(x), kValiditySeconds - kBackdateSeconds);
    if (X509_set_pubkey(x, subjectKey) != 1) {
        X509_free(x);
        fail(err, "X509_set_pubkey 失败");
        return nullptr;
    }

    // 空 DN：issuer 与 subject 都是空 NAME（不是"没设"）
    X509_NAME* empty = X509_NAME_new();
    if (empty == nullptr) {
        X509_free(x);
        fail(err, "X509_NAME_new 失败");
        return nullptr;
    }
    const int s1 = X509_set_subject_name(x, empty);
    const int s2 = X509_set_issuer_name(x, empty);
    X509_NAME_free(empty);
    if (s1 != 1 || s2 != 1) {
        X509_free(x);
        fail(err, "设空 DN 失败");
        return nullptr;
    }

    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    // x 要传进去：subjectKeyIdentifier 的 "hash" 得拿证书里的公钥现算
    X509V3_set_ctx(&v3, nullptr, x, nullptr, nullptr, 0);

    if (!addExt(x, &v3, NID_basic_constraints, isCa ? "critical,CA:TRUE" : "critical,CA:FALSE",
                err)) {
        X509_free(x);
        return nullptr;
    }
    if (!isCa) {
        // 叶子证书的 keyUsage：digitalSignature + keyEncipherment（编码出来正好是 0xA0/5）
        if (!addExt(x, &v3, NID_key_usage, "critical,digitalSignature,keyEncipherment", err)) {
            X509_free(x);
            return nullptr;
        }
    }
    if (withSki) {
        // 只有 device 证书有这条，且**不是 critical**。"hash" = 公钥 PKCS#1 的 SHA-1
        if (!addExt(x, &v3, NID_subject_key_identifier, "hash", err)) {
            X509_free(x);
            return nullptr;
        }
    }

    if (X509_sign(x, signKey, EVP_sha256()) <= 0) {
        X509_free(x);
        fail(err, "X509_sign 失败");
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

/// PKCS#8（"-----BEGIN PRIVATE KEY-----"）。**不能用传统 PKCS#1 的 "RSA PRIVATE KEY"**，
/// DiPlay 那边就是 PKCS#8，手机对这两者未必一视同仁。
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
        // 理论上到不了这儿。真到了也别返回空串——空 HostID 手机会直接拒。
        LOG_WARN("[%s] RAND_bytes 失败，HostID 退化（%s）", kTag, opensslError().c_str());
        const uint64_t t = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
        std::memcpy(b, &t, sizeof t);
        std::memcpy(b + 8, &t, sizeof t);
    }
    b[6] = uint8_t((b[6] & 0x0f) | 0x40);  // 版本 4
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
        if (err != nullptr) *err = "out 是空的";
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
    // device 证书：公钥是手机的，签名者还是我们的 root，只有它带 SKI
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
        if (err != nullptr) *err = "证书转 PEM 出来是空的";
        return false;
    }

    LOG_INFO("[%s] 证书造好了（RSA-%d，root/host/device 各一），HostID=%s", kTag, kRsaBits,
             id.hostId.c_str());
    *out = std::move(id);
    return true;
}

// ---------------------------------------------------------------------------
// 落盘 / 复读
// ---------------------------------------------------------------------------
namespace {

namespace fs = std::filesystem;

/// 目录名。序列号本来就只有字母数字，但这是从外部字符串直接拼路径，还是过一道。
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

/// 手机公钥的指纹。**只用来判断"还是不是同一台手机"**，不进任何报文、不影响配对结果。
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
        // 尽力而为：设不上权限不影响功能（这是我们自己造的密钥，不是 Apple 发的凭据）
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
        if (err != nullptr) *err = "out 是空的";
        return false;
    }

    const std::string dir = pairDir(stateDir, key);

    // 指纹：记录里存的手机公钥跟这次读到的是不是同一把。对不上就是另一台手机
    // （或这台手机重装过）—— 那份记录对它无效，当没有，重新配。
    std::string storedFp;
    if (!readWholeFile((fs::path(dir) / "device.fp").string(), &storedFp)) return true;  // 没存过
    // 写的时候末尾带了个换行（便于 cat 看），比较前得去掉 —— 不然 "abc\n" != "abc"，
    // 指纹永远对不上，等于没存。
    while (!storedFp.empty() && (storedFp.back() == '\n' || storedFp.back() == '\r'))
        storedFp.pop_back();

    const std::string wantFp = deviceFingerprint(devicePublicKey);
    if (wantFp.empty() || storedFp != wantFp) {
        LOG_INFO("[%s] 存的配对材料不是这台手机的（公钥指纹对不上），重新配一套", kTag);
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
        if (err != nullptr) *err = "out 是空的";
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
        // 残缺的记录比没有更糟（会拿半套材料去开会话），当没有处理，让上层重造覆盖。
        LOG_WARN("[%s] %s 下的配对材料读不全，当没有", kTag, dir.c_str());
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
        if (err != nullptr) *err = "建配对材料目录失败：" + dir + "（" + ec.message() + "）";
        return false;
    }
    // 内容只是给人看的（什么时候配成功的），判据是"文件在不在"。
    const auto  now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string stamp = std::to_string(now) + "\n";
    if (!writeWholeFile((fs::path(dir) / "paired").string(), stamp, false)) {
        if (err != nullptr) *err = "写 paired 印记失败：" + dir;
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
        if (err != nullptr) *err = "建配对材料目录失败：" + dir + "（" + ec.message() + "）";
        return false;
    }

    const auto f = [&dir](const char* n) { return (fs::path(dir) / n).string(); };
    const std::string fp = deviceFingerprint(devicePublicKey);

    // ids.txt 先写：它里面是 HostID / SystemBUID，下一轮 SetValue(UntrustedHostBUID)
    // 就靠它。私钥那两个文件单独标 0600。
    const bool ok = !fp.empty() && writeWholeFile(f("ids.txt"), id.hostId + "\n" + id.systemBuid + "\n", false) &&
                    writeWholeFile(f("device.fp"), fp + "\n", false) &&
                    writeWholeFile(f("root.crt"), id.rootCertPem, false) &&
                    writeWholeFile(f("root.key"), id.rootKeyPem, true) &&
                    writeWholeFile(f("host.crt"), id.hostCertPem, false) &&
                    writeWholeFile(f("host.key"), id.hostKeyPem, true) &&
                    writeWholeFile(f("device.crt"), id.deviceCertPem, false);
    if (!ok) {
        if (err != nullptr) *err = "写配对材料失败：" + dir;
        return false;
    }
    LOG_INFO("[%s] 配对材料已存到 %s —— 下次同一台手机不再弹信任框", kTag, dir.c_str());
    return true;
}

}  // namespace core::usb
