/**
 * @file   tls.h
 * @brief  Android Auto TLS 1.2 client (records carried over AAP SSL_HANDSHAKE messages)
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/tls.h - AA TLS 1.2 client
// ============================================================================
// Difference from ordinary TLS: records are **not** a raw byte stream. During the handshake each record is the body of an AAP
// control-channel 0x0003 (SSL_HANDSHAKE) message; after the handshake, each application message goes through
// SSL_write / SSL_read individually (aasdk does this 1:1, and we follow suit).
//
// Certificate: a self-signed RSA-2048 certificate (CN=AllPlay) generated at runtime; no vendor certificate is reused.
// TLS 1.2 is pinned (aasdk also uses TLSv1_2_client_method on the old OpenSSL path).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st     SSL;
typedef struct bio_st     BIO;

namespace core::projection::androidauto {

class Tls12Client {
public:
    enum class Handshake { Done, NeedMore, Failed };

    Tls12Client() = default;
    ~Tls12Client();
    Tls12Client(const Tls12Client&) = delete;
    Tls12Client& operator=(const Tls12Client&) = delete;

    bool init(std::string* err);

    /// Call after peer data has been ingested. NeedMore = keep waiting for peer records.
    Handshake handshake(std::string* err);

    bool ingest(const uint8_t* data, size_t len);
    std::vector<uint8_t> takeOutput();

    bool encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>* cipher,
                 std::string* err);
    /// Decrypt one frame. Returns true with an empty plain when peer data is incomplete (wait for the next frame).
    bool decrypt(const uint8_t* cipher, size_t len, std::vector<uint8_t>* plain,
                 std::string* err);

    bool active() const { return active_; }

private:
    void reset();

    SSL_CTX* ctx_    = nullptr;
    SSL*     ssl_    = nullptr;
    BIO*     rbio_   = nullptr;
    BIO*     wbio_   = nullptr;
    bool     active_ = false;
};

}  // namespace core::projection::androidauto
