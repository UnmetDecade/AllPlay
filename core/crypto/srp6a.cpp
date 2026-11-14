#include "crypto/srp6a.h"

#include "crypto/hkdf.h"

#include <openssl/rand.h>

#include <cstring>
#include <string>

namespace core::crypto {

namespace {

// RFC 5054 3072-bit prime (group 15). Byte-for-byte identical to the RFC 5054 group 15 prime.
const char* const kNHex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

constexpr int kNBytes = 384;

std::vector<uint8_t> fromHex(const char* hex, size_t hexLen)
{
    std::vector<uint8_t> out(hexLen / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        out[i] = uint8_t((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    }
    return out;
}

std::vector<uint8_t> bytesOf(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

/// BIGNUM → minimal big-endian bytes (protocol requirement: strip the leading sign 0).
std::vector<uint8_t> minimalBytes(const BIGNUM* v)
{
    int n = BN_num_bytes(v);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    BN_bn2bin(v, out.data());
    return out;
}

/// BIGNUM → 384 bytes, left-zero-padded (protocol requirement).
std::vector<uint8_t> pad384(const BIGNUM* v)
{
    int n = BN_num_bytes(v);
    if (n > kNBytes) n = kNBytes;
    std::vector<uint8_t> out(kNBytes, 0);
    BN_bn2bin(v, out.data() + (kNBytes - n));
    return out;
}

bool constTimeEq(const uint8_t* a, const uint8_t* b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

}  // namespace

Srp6aServer::Srp6aServer() { init(nullptr, nullptr); }

Srp6aServer::Srp6aServer(const uint8_t salt[16], const uint8_t b[32])
{
    init(salt, b);
}

void Srp6aServer::init(const uint8_t* salt, const uint8_t* b)
{
    const std::vector<uint8_t> nbytes = fromHex(kNHex, std::strlen(kNHex));
    n_ = BN_bin2bn(nbytes.data(), int(nbytes.size()), nullptr);
    g_ = BN_new();
    BN_set_word(g_, 5);

    BN_CTX* ctx = BN_CTX_new();

    // k = SHA512(toBytes(N) ‖ pad384(g))
    {
        std::vector<uint8_t> in = nbytes;               // toBytes(N) = 384 bytes
        std::vector<uint8_t> pg = pad384(g_);
        in.insert(in.end(), pg.begin(), pg.end());
        auto kh = sha512(in);
        k_ = BN_bin2bn(kh.data(), int(kh.size()), nullptr);
    }

    // Salt s = 16 bytes (random or fixed)
    salt_.resize(16);
    if (salt) std::memcpy(salt_.data(), salt, 16);
    else RAND_bytes(salt_.data(), 16);

    // x = SHA512(salt ‖ SHA512("Pair-Setup":"3939"))
    BIGNUM* x = nullptr;
    {
        auto inner = sha512(bytesOf("Pair-Setup:3939"));
        std::vector<uint8_t> in = salt_;
        in.insert(in.end(), inner.begin(), inner.end());
        auto xh = sha512(in);
        x = BN_bin2bn(xh.data(), int(xh.size()), nullptr);
    }

    // v = g^x mod n
    v_ = BN_new();
    BN_mod_exp(v_, g_, x, n_, ctx);
    BN_free(x);

    // b = 32 bytes (random or fixed, interpreted unsigned)
    {
        uint8_t bb[32];
        if (b) std::memcpy(bb, b, 32);
        else RAND_bytes(bb, sizeof bb);
        b_ = BN_bin2bn(bb, sizeof bb, nullptr);
    }

    // B = (k*v + g^b) mod n
    B_ = BN_new();
    BIGNUM* gb = BN_new();
    BN_mod_exp(gb, g_, b_, n_, ctx);
    BN_mod_mul(B_, k_, v_, n_, ctx);
    BN_mod_add(B_, B_, gb, n_, ctx);
    BN_free(gb);

    pubB_ = pad384(B_);

    BN_CTX_free(ctx);
}

Srp6aServer::~Srp6aServer()
{
    BN_free(n_); BN_free(g_); BN_free(k_);
    BN_free(v_); BN_free(b_); BN_free(B_);
}

const std::vector<uint8_t>& Srp6aServer::salt() const { return salt_; }
const std::vector<uint8_t>& Srp6aServer::publicKey() const { return pubB_; }

bool Srp6aServer::verify(const uint8_t* A, size_t aLen,
                         const uint8_t* m1, size_t m1Len,
                         std::vector<uint8_t>* K, std::vector<uint8_t>* m2)
{
    if (aLen != kNBytes || m1Len != 64 || !K || !m2) return false;

    BIGNUM* a = BN_bin2bn(A, int(aLen), nullptr);
    if (!a) return false;
    BN_CTX* ctx = BN_CTX_new();

    bool ok = false;
    BIGNUM *tmp = nullptr, *u = nullptr, *vu = nullptr, *S = nullptr;

    do {
        // A mod N == 0 → reject
        tmp = BN_new();
        BN_nnmod(tmp, a, n_, ctx);
        if (BN_is_zero(tmp)) break;

        // u = SHA512(pad(A) ‖ pad(B))
        {
            std::vector<uint8_t> in(A, A + aLen);
            const std::vector<uint8_t> pB = pubB_;
            in.insert(in.end(), pB.begin(), pB.end());
            auto uh = sha512(in);
            u = BN_bin2bn(uh.data(), int(uh.size()), nullptr);
        }

        // S = (a * v^u)^b mod n
        vu = BN_new();
        BN_mod_exp(vu, v_, u, n_, ctx);
        S = BN_new();
        BN_mod_mul(S, a, vu, n_, ctx);
        BN_mod_exp(S, S, b_, n_, ctx);

        // K = SHA512(toBytes(S))
        auto Kh = sha512(minimalBytes(S));

        // hashN = SHA512(toBytes(N)), hashG = SHA512(toBytes(g))
        auto hashN = sha512(minimalBytes(n_));
        auto hashG = sha512(minimalBytes(g_));
        std::vector<uint8_t> hashXor(64);
        for (int i = 0; i < 64; ++i) hashXor[i] = hashN[i] ^ hashG[i];

        // Expected M1 = SHA512(hashXor ‖ SHA512("Pair-Setup") ‖ salt ‖ pad(A) ‖ pad(B) ‖ K)
        auto hUser = sha512(bytesOf("Pair-Setup"));
        std::vector<uint8_t> e = hashXor;
        e.insert(e.end(), hUser.begin(), hUser.end());
        e.insert(e.end(), salt_.begin(), salt_.end());
        e.insert(e.end(), A, A + aLen);
        e.insert(e.end(), pubB_.begin(), pubB_.end());
        e.insert(e.end(), Kh.begin(), Kh.end());
        auto expected = sha512(e);

        if (!constTimeEq(expected.data(), m1, 64)) break;

        // M2 = SHA512(pad(A) ‖ M1 ‖ K)
        std::vector<uint8_t> m2in(A, A + aLen);
        m2in.insert(m2in.end(), m1, m1 + 64);
        m2in.insert(m2in.end(), Kh.begin(), Kh.end());

        *K = std::move(Kh);
        *m2 = sha512(m2in);
        ok = true;
    } while (false);

    BN_free(tmp); BN_free(u); BN_free(vu); BN_free(S);
    BN_free(a);
    BN_CTX_free(ctx);
    return ok;
}

}  // namespace core::crypto
