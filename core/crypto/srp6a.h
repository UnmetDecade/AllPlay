/**
 * @file   srp6a.h
 * @brief  SRP-6a server (RFC 5054 3072-bit, SHA-512, AirPlay variant)
 * @author blt
 */

// ============================================================================
// core/crypto/srp6a.h — SRP-6a server (RFC 5054 3072-bit, SHA-512, AirPlay variant)
// ============================================================================
// Follows the AirPlay SRP-6a protocol (RFC 5054; formulas verified byte for byte):
//   username="Pair-Setup", password="3939" are fixed.
//   k = SHA512(toBytes(N) ‖ pad384(g)); u = SHA512(pad384(A) ‖ pad384(B));
//   K = SHA512(toBytes(S)) (minimal bytes, no zero padding, no RFC5054 interleaved hash) — a gotcha!
#pragma once

#include <openssl/bn.h>

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

class Srp6aServer {
public:
    Srp6aServer();               ///< Generates salt s and b, computes B
    /// Fixed salt/exponent (for deterministic tests/cross-validation; real devices use the default random construction).
    Srp6aServer(const uint8_t salt[16], const uint8_t b[32]);
    ~Srp6aServer();
    Srp6aServer(const Srp6aServer&) = delete;
    Srp6aServer& operator=(const Srp6aServer&) = delete;

    const std::vector<uint8_t>& salt() const;      ///< 16 bytes
    const std::vector<uint8_t>& publicKey() const; ///< pad(B), 384 bytes

    /// Receives A(384) and client proof M1(64). On success writes K(64)/M2(64) and returns true.
    bool verify(const uint8_t* A, size_t aLen,
                const uint8_t* m1, size_t m1Len,
                std::vector<uint8_t>* K, std::vector<uint8_t>* m2);

private:
    void init(const uint8_t* salt, const uint8_t* b);

    BIGNUM* n_ = nullptr;   // prime
    BIGNUM* g_ = nullptr;   // generator 5
    BIGNUM* k_ = nullptr;   // multiplier
    BIGNUM* v_ = nullptr;   // verifier
    BIGNUM* b_ = nullptr;   // private exponent
    BIGNUM* B_ = nullptr;   // public key B

    std::vector<uint8_t> salt_;   // 16 bytes
    std::vector<uint8_t> pubB_;   // pad(B), 384 bytes
};

}  // namespace core::crypto
