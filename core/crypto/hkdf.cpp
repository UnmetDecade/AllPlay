#include "crypto/hkdf.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstring>

namespace core::crypto {

namespace {

std::vector<uint8_t> hmacSha512(const uint8_t* key, size_t keyLen,
                                const uint8_t* data, size_t dataLen)
{
    std::vector<uint8_t> out(64);
    unsigned int olen = 0;
    if (!HMAC(EVP_sha512(), key, int(keyLen), data, dataLen, out.data(), &olen))
        return {};
    out.resize(olen);
    return out;
}

std::vector<uint8_t> digestOf(const EVP_MD* md, const std::vector<uint8_t>& data)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    unsigned int olen = 0;
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, out.data(), &olen);
    EVP_MD_CTX_free(ctx);
    out.resize(olen);
    return out;
}

}  // namespace

std::vector<uint8_t> hkdfSha512(const uint8_t* ikm, size_t ikmLen,
                                const uint8_t* salt, size_t saltLen,
                                const uint8_t* info, size_t infoLen,
                                size_t outLen)
{
    // Extract: PRK = HMAC(salt, ikm); empty salt → 64 zero bytes
    std::vector<uint8_t> saltv;
    if (saltLen == 0) saltv.assign(64, 0);
    else saltv.assign(salt, salt + saltLen);
    auto prk = hmacSha512(saltv.data(), saltv.size(), ikm, ikmLen);
    if (prk.empty()) return {};

    // Expand: T(i) = HMAC(PRK, T(i-1) || info || counter)
    std::vector<uint8_t> out;
    out.reserve(outLen);
    std::vector<uint8_t> t;
    uint8_t counter = 1;
    while (out.size() < outLen) {
        std::vector<uint8_t> in = t;
        if (infoLen) in.insert(in.end(), info, info + infoLen);
        in.push_back(counter);
        t = hmacSha512(prk.data(), prk.size(), in.data(), in.size());
        if (t.empty()) return {};
        out.insert(out.end(), t.begin(), t.end());
        ++counter;
    }
    out.resize(outLen);
    return out;
}

std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::vector<uint8_t>& salt,
                                const std::vector<uint8_t>& info,
                                size_t outLen)
{
    return hkdfSha512(ikm.data(), ikm.size(), salt.data(), salt.size(),
                      info.data(), info.size(), outLen);
}

std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::string& salt,
                                const std::string& info,
                                size_t outLen)
{
    return hkdfSha512(ikm.data(), ikm.size(),
                      reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                      reinterpret_cast<const uint8_t*>(info.data()), info.size(),
                      outLen);
}

std::vector<uint8_t> sha1(const std::vector<uint8_t>& data)   { return digestOf(EVP_sha1(), data); }
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) { return digestOf(EVP_sha256(), data); }
std::vector<uint8_t> sha512(const std::vector<uint8_t>& data) { return digestOf(EVP_sha512(), data); }

}  // namespace core::crypto
