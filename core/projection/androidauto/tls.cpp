/**
 * @file   tls.cpp
 * @brief  AA TLS 1.2 client implementation (OpenSSL + memory BIOs)
 */

#include "projection/androidauto/tls.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstring>

namespace core::projection::androidauto {

namespace {

std::string opensslError(const char* what)
{
    const unsigned long e = ERR_get_error();
    if (e == 0) return what;
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return std::string(what) + ": " + buf;
}

/// Generate a self-signed RSA-2048 certificate (CN=AllPlay). Does not touch the filesystem.
EVP_PKEY* generateKey(std::string* err)
{
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx) { if (err) *err = opensslError("EVP_PKEY_CTX_new_id"); return nullptr; }

    EVP_PKEY* key = nullptr;
    bool ok = EVP_PKEY_keygen_init(kctx) == 1 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) == 1 &&
              EVP_PKEY_keygen(kctx, &key) == 1;
    EVP_PKEY_CTX_free(kctx);
    if (!ok) {
        if (err) *err = opensslError("Failed to generate RSA key");
        EVP_PKEY_free(key);
        return nullptr;
    }
    return key;
}

X509* makeCertificate(EVP_PKEY* key, std::string* err)
{
    X509* x = X509_new();
    if (!x) { if (err) *err = opensslError("X509_new"); return nullptr; }

    X509_set_version(x, 2);   // v3 (valid even without extensions)
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    // Backdate by one day so a slightly slow phone clock does not judge the certificate as "not yet valid".
    X509_gmtime_adj(X509_get_notBefore(x), -60L * 60 * 24);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 3650);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("CN"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("AllPlay"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("AllPlay"), -1, -1, 0);
    X509_set_issuer_name(x, name);   // self-signed

    if (X509_set_pubkey(x, key) != 1 ||
        X509_sign(x, key, EVP_sha256()) <= 0) {
        if (err) *err = opensslError("Failed to create self-signed certificate");
        X509_free(x);
        return nullptr;
    }
    return x;
}

}  // namespace

Tls12Client::~Tls12Client() { reset(); }

void Tls12Client::reset()
{
    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }   // BIOs are freed with the SSL object
    rbio_ = nullptr;
    wbio_ = nullptr;
    if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    active_ = false;
}

bool Tls12Client::init(std::string* err)
{
    reset();

    ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ctx_) {
        if (err) *err = opensslError("SSL_CTX_new");
        return false;
    }

    // AA head units have always used TLS 1.2; without pinning, newer OpenSSL may negotiate 1.3, which the phone rejects.
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx_, TLS1_2_VERSION);

    EVP_PKEY* key = generateKey(err);
    if (!key) return false;
    X509* cert = makeCertificate(key, err);
    if (!cert) { EVP_PKEY_free(key); return false; }

    const bool ok = SSL_CTX_use_certificate(ctx_, cert) == 1 &&
                    SSL_CTX_use_PrivateKey(ctx_, key) == 1 &&
                    SSL_CTX_check_private_key(ctx_) == 1;
    X509_free(cert);
    EVP_PKEY_free(key);
    if (!ok) {
        if (err) *err = opensslError("Failed to load self-signed certificate");
        reset();
        return false;
    }

    ssl_ = SSL_new(ctx_);
    if (!ssl_) {
        if (err) *err = opensslError("SSL_new");
        reset();
        return false;
    }

    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    if (!rbio_ || !wbio_) {
        if (err) *err = "BIO_new failed";
        reset();
        return false;
    }
    BIO_set_mem_eof_return(rbio_, -1);
    BIO_set_mem_eof_return(wbio_, -1);
    SSL_set_bio(ssl_, rbio_, wbio_);   // ownership passes to SSL
    SSL_set_connect_state(ssl_);
    SSL_set_verify(ssl_, SSL_VERIFY_NONE, nullptr);   // same as aasdk: the phone side decides
    return true;
}

bool Tls12Client::ingest(const uint8_t* data, size_t len)
{
    if (!rbio_ || len == 0) return true;
    const int n = BIO_write(rbio_, data, int(len));
    return n == int(len);
}

std::vector<uint8_t> Tls12Client::takeOutput()
{
    std::vector<uint8_t> out;
    if (!wbio_) return out;
    int pending = int(BIO_ctrl_pending(wbio_));
    if (pending <= 0) return out;
    out.resize(size_t(pending));
    const int n = BIO_read(wbio_, out.data(), pending);
    if (n <= 0) out.clear();
    else out.resize(size_t(n));
    return out;
}

Tls12Client::Handshake Tls12Client::handshake(std::string* err)
{
    if (!ssl_) { if (err) *err = "TLS not initialized"; return Handshake::Failed; }
    ERR_clear_error();
    const int r = SSL_do_handshake(ssl_);
    if (r == 1) {
        active_ = true;
        return Handshake::Done;
    }
    const int e = SSL_get_error(ssl_, r);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return Handshake::NeedMore;
    if (err) *err = opensslError("TLS handshake failed");
    return Handshake::Failed;
}

bool Tls12Client::encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>* cipher,
                          std::string* err)
{
    if (!active_ || !ssl_) { if (err) *err = "TLS not ready"; return false; }
    cipher->clear();

    ERR_clear_error();
    size_t off = 0;
    while (off < plain.size()) {
        const int n = SSL_write(ssl_, plain.data() + off, int(plain.size() - off));
        if (n <= 0) {
            if (err) *err = opensslError("SSL_write");
            return false;
        }
        off += size_t(n);
        auto chunk = takeOutput();
        cipher->insert(cipher->end(), chunk.begin(), chunk.end());
    }
    return true;
}

bool Tls12Client::decrypt(const uint8_t* data, size_t len, std::vector<uint8_t>* plain,
                          std::string* err)
{
    plain->clear();
    if (!active_ || !ssl_) { if (err) *err = "TLS not ready"; return false; }
    if (!ingest(data, len)) { if (err) *err = "Failed to write TLS input buffer"; return false; }

    ERR_clear_error();
    uint8_t buf[16 * 1024];
    for (;;) {
        const int n = SSL_read(ssl_, buf, int(sizeof buf));
        if (n > 0) {
            plain->insert(plain->end(), buf, buf + n);
            continue;
        }
        const int e = SSL_get_error(ssl_, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return true;
        if (e == SSL_ERROR_ZERO_RETURN) return true;   // peer closed the connection
        if (err) *err = opensslError("SSL_read");
        return false;
    }
}

}  // namespace core::projection::androidauto
