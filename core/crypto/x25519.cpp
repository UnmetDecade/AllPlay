#include "crypto/x25519.h"

#include <openssl/evp.h>

namespace core::crypto {

X25519Key x25519Generate()
{
    X25519Key k{};
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    EVP_PKEY* pkey = nullptr;
    if (ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &pkey) > 0 && pkey) {
        size_t plen = 32, slen = 32;
        EVP_PKEY_get_raw_public_key(pkey, k.pub.data(), &plen);
        EVP_PKEY_get_raw_private_key(pkey, k.priv.data(), &slen);
    }
    if (pkey) EVP_PKEY_free(pkey);
    if (ctx) EVP_PKEY_CTX_free(ctx);
    return k;
}

std::vector<uint8_t> x25519Shared(const uint8_t priv[32], const uint8_t peerPub[32])
{
    EVP_PKEY* pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv, 32);
    EVP_PKEY* pp = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peerPub, 32);
    if (!pk || !pp) {
        if (pk) EVP_PKEY_free(pk);
        if (pp) EVP_PKEY_free(pp);
        return {};
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pk, nullptr);
    std::vector<uint8_t> out;
    if (ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, pp) > 0) {
        size_t olen = 0;
        if (EVP_PKEY_derive(ctx, nullptr, &olen) > 0 && olen == 32) {
            out.resize(olen);
            if (EVP_PKEY_derive(ctx, out.data(), &olen) <= 0) out.clear();
        }
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pk);
    EVP_PKEY_free(pp);

    // Reject an all-zero shared secret (protocol requirement)
    bool allZero = !out.empty();
    for (uint8_t b : out) if (b != 0) { allZero = false; break; }
    if (allZero) out.clear();
    return out;
}

}  // namespace core::crypto
