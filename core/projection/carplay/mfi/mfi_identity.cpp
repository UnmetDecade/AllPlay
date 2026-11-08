#include "mfi/mfi_identity.h"

#include "common/log.h"

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/x509.h>

#include <fstream>

namespace core::mfi {

namespace {

std::vector<uint8_t> readFile(const std::string& path, bool* ok)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { *ok = false; return {}; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    *ok = !data.empty();
    return data;
}

}  // namespace

CertificateMfiIdentity::~CertificateMfiIdentity()
{
    if (key_) EVP_PKEY_free(key_);
}

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::load(const std::string& dir, std::string* err)
{
    bool ok = false;
    auto keyDer = readFile(dir + "/identity.pk8", &ok);
    if (!ok) {
        if (err) *err = "读不到 " + dir + "/identity.pk8（先跑 scripts/extract-identity.sh）";
        return nullptr;
    }
    auto cert = readFile(dir + "/certificate.p7b", &ok);
    if (!ok) {
        if (err) *err = "读不到 " + dir + "/certificate.p7b";
        return nullptr;
    }
    return loadFromMemory(keyDer.data(), keyDer.size(), cert.data(), cert.size(), err);
}

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::loadFromMemory(
    const uint8_t* keyDer, size_t keyLen, const uint8_t* certDer, size_t certLen, std::string* err)
{
    if (!keyDer || keyLen == 0) {
        if (err) *err = "identity.pk8 是空的";
        return nullptr;
    }
    if (!certDer || certLen == 0) {
        if (err) *err = "certificate.p7b 是空的";
        return nullptr;
    }

    auto self = std::unique_ptr<CertificateMfiIdentity>(new CertificateMfiIdentity);

    const unsigned char* p = keyDer;
    self->key_ = d2i_AutoPrivateKey(nullptr, &p, long(keyLen));
    if (!self->key_) {
        if (err) *err = "identity.pk8 不是有效的私钥（PKCS#8 DER）";
        return nullptr;
    }
    if (EVP_PKEY_base_id(self->key_) != EVP_PKEY_EC) {
        if (err) *err = "identity.pk8 不是 EC 私钥";
        return nullptr;
    }
    self->cert_.assign(certDer, certDer + certLen);
    LOG_INFO("MFi 身份已载入：证书 %zu 字节", self->cert_.size());
    return self;
}

std::vector<uint8_t> CertificateMfiIdentity::signDigest(const uint8_t* digest, size_t len, bool* ok) const
{
    if (ok) *ok = false;
    if (!key_ || len != 32) return {};   // P-256 的摘要固定 32 字节

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(key_, nullptr);
    if (!ctx) return {};
    std::vector<uint8_t> out;
    do {
        if (EVP_PKEY_sign_init(ctx) <= 0) break;
        size_t derLen = 0;
        if (EVP_PKEY_sign(ctx, nullptr, &derLen, digest, len) <= 0) break;
        std::vector<uint8_t> der(derLen);
        if (EVP_PKEY_sign(ctx, der.data(), &derLen, digest, len) <= 0) break;

        // DER → raw r||s（各 32 字节）
        const unsigned char* q = der.data();
        ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &q, long(derLen));
        if (!sig) break;
        out.resize(64);
        const BIGNUM *r = nullptr, *s = nullptr;
        ECDSA_SIG_get0(sig, &r, &s);
        BN_bn2binpad(r, out.data(), 32);
        BN_bn2binpad(s, out.data() + 32, 32);
        ECDSA_SIG_free(sig);
        if (ok) *ok = true;
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    return out;
}

}  // namespace core::mfi
