// ============================================================================
// tools/m3_test.cpp -- M3 crypto-layer self-test (the most that can be verified without hardware)
// ============================================================================
// Covers:
//   ① TLV8: encode/decode round trip, >255 fragmentation, same-type separator, hand-written byte decoding
//   ② ChaCha20-Poly1305: encrypt/decrypt round trip, tampered-tag failure, nonce shape
//   ③ HKDF-SHA512: determinism, output length
//   ④ X25519: both sides derive the same shared secret
//   ⑤ Ed25519: sign/verify round trip, wrong signature fails
//   ⑥ SRP-6a: reference vectors (B/K/M1/M2) computed independently in Python with fixed salt/exponent,
//      plus negative controls for A%N==0 and a tampered M1
//
// Usage: m3_test
#include "airplay/control_cipher.h"
#include "airplay/session.h"
#include "airplay/video_stream.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/ed25519.h"
#include "crypto/hkdf.h"
#include "crypto/srp6a.h"
#include "crypto/tlv8.h"
#include "crypto/x25519.h"
#include "platform/device_identity.h"

#include "phone_crypto.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("   ✅ %s\n", msg); }                       \
        else      { std::printf("   ❌ %s\n", msg); ++g_fail; }             \
    } while (0)

// Phone-side crypto (SRP client / TLV constants / Ed25519 generation) lives in tools/phone_crypto.h,
// shared with the fake phone -- two copies would go wrong together and could make "self-tests all green but end-to-end fails"
// possible, which is the hardest class of bug to debug.
using allplay::phone::genEd25519;
using allplay::phone::hex;
using allplay::phone::sb;
using allplay::phone::SrpPhoneSide;
using allplay::phone::T_ENCRYPTED_DATA;
using allplay::phone::T_IDENTIFIER;
using allplay::phone::T_PROOF;
using allplay::phone::T_PUBLIC_KEY;
using allplay::phone::T_SALT;
using allplay::phone::T_SIGNATURE;
using allplay::phone::T_STATE;

// ---------------------------------------------------------------------------
// ① TLV8
// ---------------------------------------------------------------------------
void testTlv8()
{
    std::printf("== ① TLV8 ==\n");
    using namespace core::crypto;

    std::vector<Tlv> items = {
        {0x06, {2}},
        {0x03, std::vector<uint8_t>(64, 0xAB)},
        {0x05, std::vector<uint8_t>(300, 0xCD)},
    };
    auto enc = tlv8Encode(items);
    auto dec = tlv8Decode(enc.data(), enc.size());
    const auto* state = tlv8Get(dec, 0x06);
    const auto* pub   = tlv8Get(dec, 0x03);
    const auto* encd  = tlv8Get(dec, 0x05);
    CHECK(state && state->size() == 1 && (*state)[0] == 2, "STATE single-byte round-trip");
    CHECK(pub && pub->size() == 64 && pub->front() == 0xAB, "64-byte value round-trip");
    CHECK(encd && encd->size() == 300 && encd->back() == 0xCD, "300-byte fragment reassembly");

    // >255 fragmentation: 300 bytes should split into 255+45, both parts carrying a [type][len] header
    // find the 0x05 fragments: type 0x05 appears twice in enc, with len 255 and 45
    int frag255 = 0, frag45 = 0;
    for (size_t i = 0; i + 1 < enc.size();) {
        uint8_t t = enc[i], l = enc[i + 1];
        if (t == 0x05) { if (l == 255) ++frag255; else if (l == 45) ++frag45; }
        i += 2 + l;
    }
    CHECK(frag255 == 1 && frag45 == 1, "300 bytes split into 255+45 fragments");

    // two entries with the same type -> separator (0xff,0x00)
    std::vector<Tlv> dup = {{0x06, {1}}, {0x06, {9}}};
    auto enc2 = tlv8Encode(dup);
    bool hasSep = false;
    for (size_t i = 0; i + 1 < enc2.size(); ++i)
        if (enc2[i] == 0xFF && enc2[i + 1] == 0x00) { hasSep = true; break; }
    CHECK(hasSep, "adjacent same-type entries separated by 0xff/0x00");
    auto dec2 = tlv8Decode(enc2.data(), enc2.size());
    const auto* v6 = tlv8Get(dec2, 0x06);
    CHECK(v6 && v6->size() == 1 && (*v6)[0] == 9, "later same-type entry overrides the earlier one");
}

// ---------------------------------------------------------------------------
// ② ChaCha20-Poly1305
// ---------------------------------------------------------------------------
void testChaCha()
{
    std::printf("== ② ChaCha20-Poly1305 ==\n");
    using namespace core::crypto;

    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
    const std::vector<uint8_t> pt = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const std::vector<uint8_t> aad = {0x01, 0x02, 0x03};

    auto n = nonce64(0);
    auto sealed = chachaSeal(key, n, aad.data(), aad.size(), pt.data(), pt.size());
    CHECK(sealed.size() == pt.size() + 16, "ciphertext = plaintext + 16-byte tag");

    std::vector<uint8_t> back;
    CHECK(chachaOpen(key, n, aad.data(), aad.size(), sealed.data(), sealed.size(), &back) &&
          back == pt, "encrypt/decrypt round-trip matches");

    // tampered tag -> fail
    auto bad = sealed;
    bad.back() ^= 0xFF;
    CHECK(!chachaOpen(key, n, aad.data(), aad.size(), bad.data(), bad.size(), &back),
          "tampered tag fails decryption (negative)");

    // tampered ciphertext -> fail
    bad = sealed;
    bad[0] ^= 0x01;
    CHECK(!chachaOpen(key, n, aad.data(), aad.size(), bad.data(), bad.size(), &back),
          "tampered ciphertext fails decryption (negative)");

    // nonce shape
    auto n1 = nonce64(1);
    CHECK(n1[0] == 0 && n1[1] == 0 && n1[2] == 0 && n1[3] == 0 &&
          n1[4] == 1 && n1[8] == 0, "nonce64(1) = 4 zero bytes + LE counter");

    auto nl = nonceLabel("PV-Msg02");
    CHECK(nl[4] == 'P' && nl[5] == 'V' && nl[8] == 's' && nl[11] == '2',
          "nonceLabel(\"PV-Msg02\") shape");
}

// ---------------------------------------------------------------------------
// ③ HKDF-SHA512
// ---------------------------------------------------------------------------
void testHkdf()
{
    std::printf("== ③ HKDF-SHA512 ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> ikm = {0x00, 0x01, 0x02, 0x03};
    auto k1 = hkdfSha512(ikm, "Salt", "Info", 32);
    auto k2 = hkdfSha512(ikm, "Salt", "Info", 32);
    auto k3 = hkdfSha512(ikm, "Salt", "Other", 32);
    auto k64 = hkdfSha512(ikm, "Salt", "Info", 64);
    CHECK(k1.size() == 32 && k1 == k2, "deterministic + 32-byte output");
    CHECK(k1 != k3, "different info → different key");
    CHECK(k64.size() == 64, "64-byte output");
}

// ---------------------------------------------------------------------------
// ④ X25519
// ---------------------------------------------------------------------------
void testX25519()
{
    std::printf("== ④ X25519 ==\n");
    using namespace core::crypto;

    auto a = x25519Generate();
    auto b = x25519Generate();
    auto sa = x25519Shared(a.priv.data(), b.pub.data());
    auto sb = x25519Shared(b.priv.data(), a.pub.data());
    CHECK(!sa.empty() && sa == sb, "both sides agree on the shared secret (32 bytes)");
}

// ---------------------------------------------------------------------------
// ⑤ Ed25519
// ---------------------------------------------------------------------------
void testEd25519()
{
    std::printf("== ⑤ Ed25519 ==\n");
    using namespace core::crypto;

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* key = nullptr;
    bool okgen = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &key) > 0;
    CHECK(okgen, "generate Ed25519 key");
    if (!okgen) { if (ctx) EVP_PKEY_CTX_free(ctx); return; }

    uint8_t pub[32]; size_t plen = 32;
    EVP_PKEY_get_raw_public_key(key, pub, &plen);

    const uint8_t msg[7] = {'p', 'a', 'i', 'r', 'v', 'e', 'r'};
    auto sig = ed25519Sign(key, msg, sizeof msg);
    CHECK(sig.size() == 64, "signature = 64 bytes");
    CHECK(ed25519Verify(pub, msg, sizeof msg, sig.data()), "verification passes");
    uint8_t bad[7]; std::memcpy(bad, msg, 7); bad[0] ^= 1;
    CHECK(!ed25519Verify(pub, bad, sizeof bad, sig.data()), "wrong message fails verification (negative)");

    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
}

// ---------------------------------------------------------------------------
// ⑥ SRP-6a (against reference vectors computed independently in Python)
// ---------------------------------------------------------------------------
void testSrp()
{
    std::printf("== ⑥ SRP-6a ==\n");
    using namespace core::crypto;

    // fixed salt(00..0f) + b(01..20); reference vectors computed independently in Python
    const std::vector<uint8_t> salt = hex("000102030405060708090a0b0c0d0e0f");
    const std::vector<uint8_t> b = hex(
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    const std::vector<uint8_t> A = hex(
        "352dd9b14781c00e07d64153a8c8d095cbb05ec357d048100af82fcc5b6a5b57"
        "9b701db5545cb0aecb51fee6bd68cd66de988f4ff86c9b477594564150d00fba"
        "526d7b8dc106489618e404e4393beaffe3855d5a0c530ad39b1b80de5b31612f"
        "724e4caa49654436336976cd5e52b7f4aa5bf376115e6b142967cca7357c95cd"
        "08e6fa0c86e3118dab9732db62beb729084256af7759d4a0cf4fc2bfbaa04e7e"
        "79b9dc1e5790d43296faebc78407c4d7a8bad16df2f64d60b6f8e0b05eb07197"
        "03a7e2dd562d2f12330a86dac49c3dd84830a273c880858dc2de20b7b554b4a8"
        "10666a027a1c105764f1695aae5544b4e4acd950409fb4233e5dfaa1cb51becb"
        "9b4ffc79dafc9d022d162fda4913f642537b4dc67b9970530997fed9d2015a24"
        "318aae5896b2b1f684d09d7577bb43549caeaa96a56d5ca99e43a4841e4f2b0d"
        "3f2a8d3aa8e45b4113b4603cbfe272ef28f38c85e3a3108d93876c0076c1cf40"
        "3f38f462b1884f2371067d537584ee9907034638dab0e6be43bb30f170060db8");
    const std::vector<uint8_t> Bref = hex(
        "73a722136dd3ef4c25cab9627da94b06f71057181457c774d77485e790fe9793"
        "a0f12ac46fadb2a6ba38f9452de3c4c0fdbdcf927476affc2cb7a3849bb20a97"
        "4c69ca9e70d7d78b7a3d8535b75bbafa47559addb49548113279fbdb0b385272"
        "a2f4d0acf85689370ccc0ac1746e057a13e3f725d2a5ce1bc5857c49f9b982fe"
        "f36c3b19da4c14be368ec00a7f2140919f8163d372428a119a125eddfa9f4cc8"
        "74681f1e6f2a3debf91610ebe493e85435845fdf623246e6e79b79942fc855f8"
        "b549b19145c583cbbe38518d01f0031e9c4612d3987562aa3a9230d881a4d361"
        "32ff948d459d00c8cfef259a925caa4ae10617c7a90aee895bd282cce8bb24b5"
        "b6e8b4d68b21360754789de6f56c4abf4a709b8d84db579fa6685899dad3511d"
        "99a8c005e53c72e58af0eb92e85a879a22132936039a3d5e6cbba4a52febdf90"
        "382f455caed759596985822984eb4257d2511540621e7d080bf4cfa4df2e6545"
        "94d0fd2cf258550e1810eba6220e67f1c66618f9c62becb4eb2335a116890cac");
    const std::vector<uint8_t> M1 = hex(
        "3bcd5542bbe26311daf1e9622e101687cb991e406ed216c71975bd62bafde550"
        "f3a8f930e3b4040e6bdba938f9b916890f66d9754f9d16b2936fc2f34cc4d44a");
    const std::vector<uint8_t> Kref = hex(
        "1cc3a02b04edc6c7f28c6dd6ac73bc1ad1dcada43eacab30a55615925a78c83c"
        "9581959e4482f957315ebd73c53f0f750a42ad5b42b8f76a69869a17a7648333");
    const std::vector<uint8_t> M2ref = hex(
        "3bca3cea127ca8ce31cd8bf3584af063a5f28a5e16a9cda7fc8a0f9e0a6b5be3"
        "956372be1ba6c81472edc29075eafddbba7c1f4bf67808aec6adf3349fbdc09e");

    Srp6aServer srv(salt.data(), b.data());
    CHECK(srv.salt() == salt, "fixed 16-byte salt");
    CHECK(srv.publicKey() == Bref, "B = (k*v + g^b) mod N (384 bytes, matches reference vector)");

    std::vector<uint8_t> K, M2;
    bool ok = srv.verify(A.data(), A.size(), M1.data(), M1.size(), &K, &M2);
    CHECK(ok, "verify(A, M1) passes");
    CHECK(K == Kref, "session key K matches reference vector");
    CHECK(M2 == M2ref, "server proof M2 matches reference vector");

    // tampered M1 -> fail
    std::vector<uint8_t> m1bad = M1; m1bad[0] ^= 1;
    CHECK(!srv.verify(A.data(), A.size(), m1bad.data(), m1bad.size(), &K, &M2),
          "tampered M1 fails verification (negative)");

    // A = N (A mod N == 0) -> reject
    const std::vector<uint8_t> Nhex = hex(
        "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74"
        "020bbea63b139b22514a08798e3404ddef9519b3cd3a431b302b0a6df25f1437"
        "4fe1356d6d51c245e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
        "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3dc2007cb8a163bf05"
        "98da48361c55d39a69163fa8fd24cf5f83655d23dca3ad961c62f356208552bb"
        "9ed529077096966d670c354e4abc9804f1746c08ca18217c32905e462e36ce3b"
        "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9de2bcbf695581718"
        "3995497cea956ae515d2261898fa051015728e5a8aaac42dad33170d04507a33"
        "a85521abdf1cba64ecfb850458dbef0a8aea71575d060c7db3970f85a6e1e4c7"
        "abf5ae8cdb0933d71e8c94e04a25619dcee3d2261ad2ee6bf12ffa06d98a0864"
        "d87602733ec86a64521f2b18177b200cbbe117577a615d6c770988c0bad946e2"
        "08e24fa074e5ab3143db5bfce0fd108e4b82d120a93ad2caffffffffffffffff");
    CHECK(!srv.verify(Nhex.data(), Nhex.size(), M1.data(), M1.size(), &K, &M2),
          "A mod N == 0 rejected (negative)");
}

// ---------------------------------------------------------------------------
// ⑦ ControlCipher frame-format round trip
// ---------------------------------------------------------------------------
void testControlCipher()
{
    std::printf("== ⑦ ControlCipher frame format ==\n");
    using core::airplay::ControlCipher;

    uint8_t rk[32], wk[32];
    for (int i = 0; i < 32; ++i) { rk[i] = uint8_t(i); wk[i] = uint8_t(0x80 + i); }
    ControlCipher enc(rk, wk);   // write side: writeKey=wk
    ControlCipher dec(wk, rk);   // read side: readKey=wk (crosses with enc's writeKey)

    // short message
    const std::vector<uint8_t> msg = {'a', 'b', 'c'};
    auto framed = enc.encrypt(msg.data(), msg.size());
    CHECK(framed.size() == 2 + 3 + 16, "frame = 2B header + ciphertext + 16B tag");
    CHECK(framed[0] == 3 && framed[1] == 0, "length header = little-endian 3");

    dec.feed(framed.data(), framed.size());
    std::vector<uint8_t> out;
    CHECK(dec.next(&out) && out == msg, "encrypt→feed→decrypt matches");
    CHECK(!dec.next(&out), "no leftover frame");

    // large-message fragmentation (>0x4000); use a fresh write side (counter from 0, aligned with dec2)
    std::vector<uint8_t> big(0x4000 + 100, 0x5A);
    ControlCipher enc2(rk, wk);
    auto framed2 = enc2.encrypt(big.data(), big.size());
    ControlCipher dec2(wk, rk);
    dec2.feed(framed2.data(), framed2.size());
    std::vector<uint8_t> merged;
    std::vector<uint8_t> part;
    while (dec2.next(&part)) { merged.insert(merged.end(), part.begin(), part.end()); }
    CHECK(merged == big, ">0x4000 frames reassemble identically");
}

// ---------------------------------------------------------------------------
// ⑧ Session flow: fake phone goes pair-setup -> pair-verify -> control keys
// ---------------------------------------------------------------------------
namespace {

using core::crypto::Tlv;

}  // namespace

void testSessionFlow()
{
    std::printf("== ⑧ Session flow (fake phone pair-setup → pair-verify) ==\n");
    using namespace core::crypto;

    // local identity + session
    core::DeviceIdentity ident;
    std::string err;
    CHECK(ident.loadOrCreate("/tmp/allplay-m3-identity", &err), "generate local Ed25519 identity");

    core::airplay::DeviceInfo info;
    info.name = "AllPlay"; info.deviceId = "AA:BB:CC:DD:EE:FF";
    info.pi = ident.pi; info.pkHex = ident.pkHex;

    core::airplay::SessionConfig sc;
    sc.identity = &ident; sc.info = &info;
    sc.stateDir = "/tmp/allplay-m3-pairing";
    sc.mfiDir.clear();   // skip auth-setup this round (MFi signing already covered in iap2_test)
    core::airplay::Session session(sc);

    const std::string controllerId = "11111111-2222-3333-4444-555555555555";
    uint8_t cpub[32];
    EVP_PKEY* cpriv = genEd25519(cpub);

    // ---- pair-setup M1 -> M2 ----
    {
        core::airplay::RtspRequest r; r.method = "POST"; r.uri = "/pair-setup";
        r.body = tlv8Encode({{T_STATE, {1}}});
        auto resp = session.handle(r);
        auto items = tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st = tlv8Get(items, T_STATE);
        const auto* B = tlv8Get(items, T_PUBLIC_KEY);
        const auto* salt = tlv8Get(items, T_SALT);
        CHECK(st && st->front() == 2 && B && B->size() == 384 && salt && salt->size() == 16,
              "M2 = STATE 2 + B(384) + salt(16)");
        CHECK(resp.header("Content-Type") == "application/pairing+tlv8", "pair-setup Content-Type");

        // M3
        SrpPhoneSide phone;
        std::vector<uint8_t> M1;
        phone.prove(*salt, *B, &M1);
        std::vector<Tlv> m3 = {{T_STATE, {3}}, {T_PUBLIC_KEY, phone.Apad}, {T_PROOF, M1}};
        r.body = tlv8Encode(m3);
        auto resp4 = session.handle(r);
        auto it4 = tlv8Decode(resp4.body.data(), resp4.body.size());
        const auto* st4 = tlv8Get(it4, T_STATE);
        const auto* M2 = tlv8Get(it4, T_PROOF);
        CHECK(st4 && st4->front() == 4 && M2 && phone.checkM2(*M2), "M4 = STATE 4 + correct M2");

        // M5 (controller long-term key + signature)
        std::vector<uint8_t> cSignKey = hkdfSha512(phone.K, "Pair-Setup-Controller-Sign-Salt",
                                                   "Pair-Setup-Controller-Sign-Info", 32);
        std::vector<uint8_t> signData = cSignKey;
        signData.insert(signData.end(), controllerId.begin(), controllerId.end());
        signData.insert(signData.end(), cpub, cpub + 32);
        auto cSig = ed25519Sign(cpriv, signData.data(), signData.size());

        std::vector<Tlv> inner = {{T_IDENTIFIER, sb(controllerId.c_str())},
                                  {T_PUBLIC_KEY, std::vector<uint8_t>(cpub, cpub + 32)},
                                  {T_SIGNATURE, cSig}};
        auto encKey = hkdfSha512(phone.K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);
        auto innerBytes = tlv8Encode(inner);
        auto sealed = chachaSeal(encKey.data(), nonceLabel("PS-Msg05"), nullptr, 0,
                                 innerBytes.data(), innerBytes.size());
        std::vector<Tlv> m5 = {{T_STATE, {5}}, {T_ENCRYPTED_DATA, sealed}};
        r.body = tlv8Encode(m5);
        auto resp6 = session.handle(r);
        auto it6 = tlv8Decode(resp6.body.data(), resp6.body.size());
        const auto* st6 = tlv8Get(it6, T_STATE);
        const auto* enc6 = tlv8Get(it6, T_ENCRYPTED_DATA);
        bool m6ok = st6 && st6->front() == 6 && enc6;
        std::vector<uint8_t> accInnerBytes;
        if (enc6) {
            m6ok = m6ok && chachaOpen(encKey.data(), nonceLabel("PS-Msg06"), nullptr, 0,
                                      enc6->data(), enc6->size(), &accInnerBytes);
        }
        CHECK(m6ok, "M6 = STATE 6 + decryptable ENCRYPTED_DATA");
        if (m6ok) {
            auto accInner = tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
            const auto* aid = tlv8Get(accInner, T_IDENTIFIER);
            const auto* apk = tlv8Get(accInner, T_PUBLIC_KEY);
            const auto* asg = tlv8Get(accInner, T_SIGNATURE);
            auto aSignKey = hkdfSha512(phone.K, "Pair-Setup-Accessory-Sign-Salt",
                                       "Pair-Setup-Accessory-Sign-Info", 32);
            std::vector<uint8_t> aData = aSignKey;
            aData.insert(aData.end(), aid->begin(), aid->end());
            aData.insert(aData.end(), apk->begin(), apk->end());
            CHECK(aid && apk && asg && asg->size() == 64 &&
                  *aid == sb(ident.pi.c_str()) && *apk == ident.pub &&
                  ed25519Verify(apk->data(), aData.data(), aData.size(), asg->data()),
                  "M6 inner = local identifier + public key + verifiable signature");
        }
    }

    // ---- pair-verify ----
    {
        auto clientEph = x25519Generate();
        core::airplay::RtspRequest r; r.method = "POST"; r.uri = "/pair-verify";
        r.body = tlv8Encode({{T_STATE, {1}},
                             {T_PUBLIC_KEY, std::vector<uint8_t>(clientEph.pub.begin(), clientEph.pub.end())}});
        auto resp = session.handle(r);
        auto items = tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st = tlv8Get(items, T_STATE);
        const auto* apub = tlv8Get(items, T_PUBLIC_KEY);
        const auto* enc = tlv8Get(items, T_ENCRYPTED_DATA);
        CHECK(st && st->front() == 2 && apub && apub->size() == 32 && enc,
              "PV-M2 = STATE 2 + ephemeral public key + encrypted data");

        auto shared = x25519Shared(clientEph.priv.data(), apub->data());
        auto pvKey = hkdfSha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
        std::vector<uint8_t> accInnerBytes;
        bool ok = chachaOpen(pvKey.data(), nonceLabel("PV-Msg02"), nullptr, 0,
                             enc->data(), enc->size(), &accInnerBytes);
        auto accInner = tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
        const auto* aid = tlv8Get(accInner, T_IDENTIFIER);
        const auto* asg = tlv8Get(accInner, T_SIGNATURE);
        std::vector<uint8_t> sigData(apub->begin(), apub->end());
        sigData.insert(sigData.end(), ident.pi.begin(), ident.pi.end());
        sigData.insert(sigData.end(), clientEph.pub.begin(), clientEph.pub.end());
        CHECK(ok && aid && asg && *aid == sb(ident.pi.c_str()) &&
              ed25519Verify(ident.pub.data(), sigData.data(), sigData.size(), asg->data()),
              "PV-M2 inner signature verifies (local ephemeral key ‖ pi ‖ client ephemeral key)");

        // M3 (controller signature)
        std::vector<uint8_t> sigData2(clientEph.pub.begin(), clientEph.pub.end());
        sigData2.insert(sigData2.end(), controllerId.begin(), controllerId.end());
        sigData2.insert(sigData2.end(), apub->begin(), apub->end());
        auto cSig2 = ed25519Sign(cpriv, sigData2.data(), sigData2.size());
        std::vector<Tlv> inner2 = {{T_IDENTIFIER, sb(controllerId.c_str())}, {T_SIGNATURE, cSig2}};
        auto innerBytes2 = tlv8Encode(inner2);
        auto sealed2 = chachaSeal(pvKey.data(), nonceLabel("PV-Msg03"), nullptr, 0,
                                  innerBytes2.data(), innerBytes2.size());
        r.body = tlv8Encode({{T_STATE, {3}}, {T_ENCRYPTED_DATA, sealed2}});
        auto resp4 = session.handle(r);
        auto it4 = tlv8Decode(resp4.body.data(), resp4.body.size());
        const auto* st4 = tlv8Get(it4, T_STATE);
        CHECK(st4 && st4->front() == 4, "PV-M4 = STATE 4");

        CHECK(session.controlKeysReady(), "control keys ready after pair-verify");
        auto ck = session.takeControlKeys();
        auto readKey = hkdfSha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
        auto writeKey = hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
        CHECK(std::equal(ck.readKey.begin(), ck.readKey.end(), readKey.begin()) &&
              std::equal(ck.writeKey.begin(), ck.writeKey.end(), writeKey.begin()),
              "control keys = HKDF(shared, Control-Salt, ...) (read/write directions crossed)");
    }

    if (cpriv) EVP_PKEY_free(cpriv);
}

// ---------------------------------------------------------------------------
// ⑨ Screen stream: config-frame detection + decryption + AVCC->AnnexB + keyframe gate
// ---------------------------------------------------------------------------
namespace {

struct MockVideo : public core::IVideoOutput {
    std::mutex mu;
    bool gotConfig = false;
    core::VideoCodec codec = core::VideoCodec::H264;
    std::vector<uint8_t> codecData;
    std::vector<std::vector<uint8_t>> frames;

    void configure(core::VideoCodec c, int, int, const std::vector<uint8_t>& d) override
    {
        std::lock_guard<std::mutex> lk(mu);
        gotConfig = true; codec = c; codecData = d;
    }
    bool feedAnnexB(const uint8_t* d, size_t n) override
    {
        std::lock_guard<std::mutex> lk(mu);
        frames.emplace_back(d, d + n);
        return true;
    }
    void reset() override {}
    void flush() override {}
};

struct MockStream : public core::ITcpStream {
    std::vector<uint8_t> buf;
    size_t off = 0;
    int read(uint8_t* b, size_t n, int) override
    {
        if (off >= buf.size()) return 0;   // timeout semantics: no more data
        size_t take = std::min(n, buf.size() - off);
        std::memcpy(b, buf.data() + off, take);
        off += take;
        return int(take);
    }
    bool writeAll(const uint8_t*, size_t) override { return true; }
    void close() override {}
};

struct MockListener : public core::ITcpListener {
    std::unique_ptr<MockStream> stream;
    bool start(const std::string&, uint16_t, std::string*) override { return true; }
    std::unique_ptr<core::ITcpStream> accept(int) override { return std::move(stream); }
    uint16_t port() const override { return 54321; }
    void stop() override {}
};

}  // namespace

void testScreenStream()
{
    std::printf("== ⑨ Screen stream ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> shared(32, 0x33);
    const std::string scid = "123";
    auto key = hkdfSha512(shared, "DataStream-Salt" + scid, "DataStream-Output-Encryption-Key", 32);

    // config frame: body = "avcC" + 4 bytes of fake codecData
    std::vector<uint8_t> codecData = {0x01, 0x64, 0x00, 0x1f};
    std::vector<uint8_t> cfgBody = {'a', 'v', 'c', 'C'};
    cfgBody.insert(cfgBody.end(), codecData.begin(), codecData.end());

    // video frame: AVCC with one IDR NALU (nal type 5)
    std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};

    auto makeFrame = [&](uint8_t opcode, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> header(128, 0);
        uint32_t bl = uint32_t(body.size());
        header[0] = bl & 0xFF; header[1] = (bl >> 8) & 0xFF;
        header[2] = (bl >> 16) & 0xFF; header[3] = (bl >> 24) & 0xFF;
        header[4] = opcode;
        std::vector<uint8_t> out = header;
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    // encrypted video frame body
    auto header128 = [&](const std::vector<uint8_t>& body, uint8_t opcode) {
        std::vector<uint8_t> h(128, 0);
        uint32_t bl = uint32_t(body.size());
        h[0] = bl & 0xFF; h[1] = (bl >> 8) & 0xFF;
        h[2] = (bl >> 16) & 0xFF; h[3] = (bl >> 24) & 0xFF;
        h[4] = opcode;
        return h;
    };
    // video frame: header bodyLen = ciphertext+tag length (6 + 16), AAD uses the same header
    auto encHeader = header128(std::vector<uint8_t>(avcc.size() + 16, 0), 0);
    auto sealedBody = chachaSeal(key.data(), nonce64(0), encHeader.data(), 128,
                                 avcc.data(), avcc.size());

    // assemble the mock stream: config frame + encrypted video frame
    auto mockStream = std::make_unique<MockStream>();
    {
        auto cfgFrame = makeFrame(1, cfgBody);
        auto vidHeader = encHeader;
        std::vector<uint8_t> vidFrame = vidHeader;
        vidFrame.insert(vidFrame.end(), sealedBody.begin(), sealedBody.end());
        mockStream->buf.insert(mockStream->buf.end(), cfgFrame.begin(), cfgFrame.end());
        mockStream->buf.insert(mockStream->buf.end(), vidFrame.begin(), vidFrame.end());
    }
    auto mockListener = std::make_unique<MockListener>();
    mockListener->stream = std::move(mockStream);

    MockVideo video;
    core::airplay::ScreenStream ss(std::move(mockListener), shared, scid, 1280, 720, &video);

    // wait for the screen-stream thread to process both frames (the lock is held only while reading state, released while sleeping)
    bool done = false;
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(video.mu);
            if (video.gotConfig && !video.frames.empty()) { done = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done, "screen stream: received config frame + video frame");

    {
        std::lock_guard<std::mutex> lk(video.mu);
        CHECK(video.gotConfig && video.codec == core::VideoCodec::H264 && video.codecData == codecData,
              "config frame: avcC recognized + 4-byte codecData");
        CHECK(video.frames.size() == 1 && video.frames[0] == annexb,
              "video frame: AVCC→AnnexB after decryption and the keyframe gate lets it through");
    }

    ss.stop();   // stop outside the lock (avoids deadlock with the stream thread)
}

// ---------------------------------------------------------------------------
// ⑩ Audio: RTP out-of-order reordering + decryption
// ---------------------------------------------------------------------------
namespace {

struct MockUdp : public core::IUdpSocket {
    std::mutex mu;
    std::deque<std::vector<uint8_t>> pkts;
    uint16_t p = 0;
    bool closed_ = false;

    bool bind(uint16_t, std::string*) override
    {
        static int c = 50000;
        p = uint16_t(++c);
        return true;
    }
    int recv(uint8_t* buf, size_t n, int) override
    {
        std::vector<uint8_t> v;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (closed_) return -1;
            if (!pkts.empty()) { v = std::move(pkts.front()); pkts.pop_front(); }
        }
        if (v.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }
        const size_t take = std::min(n, v.size());
        std::memcpy(buf, v.data(), take);
        return int(take);
    }
    bool send(const uint8_t*, size_t) override { return true; }
    uint16_t port() const override { return p; }
    void close() override { std::lock_guard<std::mutex> lk(mu); closed_ = true; }
    void inject(const std::vector<uint8_t>& v) { std::lock_guard<std::mutex> lk(mu); pkts.push_back(v); }
};

struct MockAudioOut : public core::IAudioOutput {
    std::mutex mu;
    std::vector<uint32_t> samples;   // RTP timestamps recorded in emission order
    void configure(core::AudioCodec, int, int) override {}
    void feed(const uint8_t*, size_t, uint32_t sample) override
    {
        std::lock_guard<std::mutex> lk(mu);
        samples.push_back(sample);
    }
    void reset() override {}
};

}  // namespace

void testAudioReorder()
{
    std::printf("== ⑩ Audio out-of-order reordering ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> shared(32, 0x44);
    const std::string scid = "7";
    auto key = hkdfSha512(shared, "DataStream-Salt" + scid, "DataStream-Output-Encryption-Key", 32);

    // build a valid encrypted RTP packet: [12B RTP header][ciphertext+16B tag][8B nonce tail]
    auto rtp = [&](uint16_t seq, uint32_t ts) {
        std::vector<uint8_t> hdr(12, 0);
        hdr[2] = uint8_t(seq >> 8); hdr[3] = uint8_t(seq & 0xff);
        hdr[4] = uint8_t(ts >> 24); hdr[5] = uint8_t(ts >> 16);
        hdr[6] = uint8_t(ts >> 8);  hdr[7] = uint8_t(ts);
        std::vector<uint8_t> payload(20, uint8_t(seq));   // fill payload with seq
        std::array<uint8_t, 8> tail{};
        tail[0] = uint8_t(seq); tail[1] = uint8_t(seq >> 8);   // nonce tail takes seq so each packet differs
        std::array<uint8_t, 12> nonce{};
        std::memcpy(nonce.data() + 4, tail.data(), 8);
        auto sealed = chachaSeal(key.data(), nonce, hdr.data() + 4, 8,
                                 payload.data(), payload.size());
        std::vector<uint8_t> out = hdr;
        out.insert(out.end(), sealed.begin(), sealed.end());   // ciphertext + tag
        out.insert(out.end(), tail.begin(), tail.end());       // 8B nonce tail
        return out;
    };

    MockUdp* dataSock = new MockUdp;
    MockUdp* ctrlSock = new MockUdp;
    MockAudioOut out;
    core::airplay::AudioStream as(
        [&]() -> std::unique_ptr<core::IUdpSocket> {
            static int n = 0;
            return n++ == 0 ? std::unique_ptr<core::IUdpSocket>(dataSock)
                            : std::unique_ptr<core::IUdpSocket>(ctrlSock);
        },
        shared, scid, core::AudioCodec::AAC_LC, 48000, 2, &out);

    // first packet seq=0 establishes the baseline, then inject out of order 2,1,4,3,6,5 -> expect emission in order 0..6
    // timestamp is deliberately = seq+1000, used to verify emission order
    for (uint16_t seq : {uint16_t(0), uint16_t(2), uint16_t(1), uint16_t(4),
                         uint16_t(3), uint16_t(6), uint16_t(5)})
        dataSock->inject(rtp(seq, 1000 + seq));

    bool done = false;
    for (int i = 0; i < 300 && !done; ++i) {
        {
            std::lock_guard<std::mutex> lk(out.mu);
            done = out.samples.size() >= 7;
        }
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done, "audio: all 7 out-of-order packets decrypted and emitted in order");

    {
        std::lock_guard<std::mutex> lk(out.mu);
        bool ordered = out.samples.size() == 7;
        for (size_t i = 0; ordered && i < out.samples.size(); ++i)
            if (out.samples[i] != 1000 + i) ordered = false;
        CHECK(ordered, "audio: emitted timestamps strictly ascending by RTP sequence (0..6)");
    }

    // inject a late duplicate packet seq=2 (already played) -> should be dropped with no new output
    dataSock->inject(rtp(2, 1002));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        std::lock_guard<std::mutex> lk(out.mu);
        CHECK(out.samples.size() == 7, "audio: late duplicate packet dropped (no replay)");
    }
}

}  // namespace

int main()
{
    testTlv8();
    testChaCha();
    testHkdf();
    testX25519();
    testEd25519();
    testSrp();
    testControlCipher();
    testSessionFlow();
    testScreenStream();
    testAudioReorder();
    std::printf("\n%s (%d failures)\n", g_fail ? "==== FAILURES ====" : "==== ALL PASSED ====", g_fail);
    return g_fail ? 1 : 0;
}
