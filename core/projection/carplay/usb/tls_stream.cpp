/**
 * @file   tls_stream.cpp
 * @brief  TLS client wrapped around any byte stream (custom BIO)
 * @author blt
 */

#include "usb/tls_stream.h"

#include "common/log.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <chrono>
#include <mutex>

namespace core::usb {
namespace {

constexpr const char* kTag = "tls";
constexpr int         kBioPollMs = 200;          ///< max time to wait per read on the underlying stream
constexpr int         kHandshakeTimeoutMs = 30000;

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

/// State behind the BIO: the wrapped stream plus "the latest time this call may wait until"
struct BioCtx {
    IByteStream*                          raw = nullptr;
    std::chrono::steady_clock::time_point deadline{};
};

int bioCreate(BIO* b)
{
    BIO_set_init(b, 1);
    BIO_set_data(b, nullptr);
    return 1;
}

int bioDestroy(BIO* b)
{
    if (b == nullptr) return 0;
    BIO_set_data(b, nullptr);
    BIO_set_init(b, 0);
    return 1;
}

int bioRead(BIO* b, char* out, int outl)
{
    auto* ctx = static_cast<BioCtx*>(BIO_get_data(b));
    if (ctx == nullptr || ctx->raw == nullptr) return -1;

    for (;;) {
        const int n = ctx->raw->read(reinterpret_cast<uint8_t*>(out), size_t(outl), kBioPollMs);
        if (n > 0) return n;
        if (n < 0) {
            // **The retry flag must be cleared before returning.** That flag was set by the earlier
            // timeout (BIO_set_retry_read) and sticks to the BIO; returning 0 with it set makes
            // OpenSSL treat this read as "no data yet, come back later" (SSL_ERROR_WANT_READ), **not EOF** —
            // so "the peer disconnected" never reaches the upper layer: it only sees 0 (timeout, no data)
            // and thinks the link is fine, spinning in place.
            // This really happened on the wired path: after unplugging, the pump thread spun at 90% CPU and wrote 2 GB of logs every 2.5 minutes.
            LOG_DEBUG("[%s] Underlying stream disconnected", kTag);
            BIO_clear_retry_flags(b);
            return 0;  // EOF: make SSL take the "peer closed" path
        }
        // The layer below merely timed out. Once the deadline hits, tell OpenSSL "no data now"; do not block here —
        // the semantics of the upper read(timeoutMs) (0 = timeout) must be preserved.
        if (std::chrono::steady_clock::now() >= ctx->deadline) {
            BIO_set_retry_read(b);
            return -1;
        }
    }
}

int bioWrite(BIO* b, const char* in, int inl)
{
    auto* ctx = static_cast<BioCtx*>(BIO_get_data(b));
    if (ctx == nullptr || ctx->raw == nullptr) return -1;
    if (!ctx->raw->writeAll(reinterpret_cast<const uint8_t*>(in), size_t(inl))) return -1;
    return inl;  // writeAll is all-or-nothing: either everything goes out or it fails
}

long bioCtrl(BIO* b, int cmd, long num, void* ptr)
{
    (void)b;
    (void)num;
    (void)ptr;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

BIO_METHOD* streamBioMethod()
{
    static BIO_METHOD* m = nullptr;
    if (m != nullptr) return m;
    m = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "allplay bytestream");
    BIO_meth_set_write(m, bioWrite);
    BIO_meth_set_read(m, bioRead);
    BIO_meth_set_ctrl(m, bioCtrl);
    BIO_meth_set_create(m, bioCreate);
    BIO_meth_set_destroy(m, bioDestroy);
    return m;
}

class TlsStream final : public IByteStream {
public:
    TlsStream(IByteStream& raw, SSL_CTX* sslCtx, SSL* ssl) : sslCtx_(sslCtx), ssl_(ssl)
    {
        bioCtx_.raw = &raw;  // raw is not ours: the caller guarantees it outlives this object
        BIO* bio = BIO_new(streamBioMethod());
        BIO_set_data(bio, &bioCtx_);
        BIO_set_init(bio, 1);
        SSL_set_bio(ssl_, bio, bio);  // same BIO for read and write; owned by ssl_, freed by SSL_free
    }

    ~TlsStream() override
    {
        closed_ = true;
        if (ssl_ != nullptr) SSL_free(ssl_);  // frees bio_ as well
        if (sslCtx_ != nullptr) SSL_CTX_free(sslCtx_);
    }

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    int  read(uint8_t* buf, size_t n, int timeoutMs) override;
    bool writeAll(const uint8_t* buf, size_t n) override;
    void close() override;

    bool handshake(std::string* err);

private:
    BioCtx     bioCtx_{};
    SSL_CTX*   sslCtx_ = nullptr;
    SSL*       ssl_ = nullptr;
    bool       closed_ = false;
    std::mutex mu_;
};

bool TlsStream::handshake(std::string* err)
{
    bioCtx_.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeTimeoutMs);
    for (;;) {
        ERR_clear_error();
        const int r = SSL_connect(ssl_);
        if (r == 1) {
            LOG_INFO("[%s] Handshake complete: %s / %s", kTag, SSL_get_version(ssl_), SSL_get_cipher(ssl_));
            return true;
        }
        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (std::chrono::steady_clock::now() >= bioCtx_.deadline) {
                if (err != nullptr) *err = "TLS handshake timed out (30s)";
                return false;
            }
            continue;
        }
        if (err != nullptr) *err = "TLS handshake failed: " + opensslError();
        return false;
    }
}

int TlsStream::read(uint8_t* buf, size_t n, int timeoutMs)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return -1;

    bioCtx_.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        ERR_clear_error();
        const int r = SSL_read(ssl_, buf, int(n));
        if (r > 0) return r;

        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_ZERO_RETURN) return -1;  // peer close_notify
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (std::chrono::steady_clock::now() >= bioCtx_.deadline) return 0;  // timeout
            continue;
        }
        if (e == SSL_ERROR_SYSCALL && r == 0) return -1;  // disconnected without close_notify
        LOG_WARN("[%s] Read failed: %s", kTag, opensslError().c_str());
        return -1;
    }
}

bool TlsStream::writeAll(const uint8_t* buf, size_t n)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return false;

    bioCtx_.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
    size_t off = 0;
    while (off < n) {
        ERR_clear_error();
        const int r = SSL_write(ssl_, buf + off, int(n - off));
        if (r > 0) {
            off += size_t(r);
            continue;
        }
        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (std::chrono::steady_clock::now() >= bioCtx_.deadline) {
                LOG_WARN("[%s] Write timed out", kTag);
                return false;
            }
            continue;
        }
        LOG_WARN("[%s] Write failed: %s", kTag, opensslError().c_str());
        return false;
    }
    return true;
}

void TlsStream::close()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return;
    closed_ = true;
    SSL_shutdown(ssl_);  // best effort: send a close_notify; the peer ignoring it is fine
}

}  // namespace

std::unique_ptr<IByteStream> startTlsClient(IByteStream& raw, const std::string& certPem,
                                            const std::string& keyPem, std::string* err)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        if (err != nullptr) *err = "Failed to create SSL_CTX: " + opensslError();
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // No peer certificate verification: the phone's certificate is signed by itself, and we have no trusted root.
    // "The other side is that phone" is guaranteed by the usbmux physical channel (USB cable), not the certificate.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    if (!certPem.empty() && !keyPem.empty()) {
        BIO* cbio = BIO_new_mem_buf(certPem.data(), int(certPem.size()));
        X509* x = cbio != nullptr ? PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr) : nullptr;
        if (cbio != nullptr) BIO_free(cbio);
        if (x == nullptr) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "Cannot read client certificate PEM";
            return nullptr;
        }
        const int rc = SSL_CTX_use_certificate(ctx, x);
        X509_free(x);
        if (rc != 1) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "Failed to load client certificate into SSL_CTX: " + opensslError();
            return nullptr;
        }

        BIO* kbio = BIO_new_mem_buf(keyPem.data(), int(keyPem.size()));
        EVP_PKEY* k =
            kbio != nullptr ? PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr) : nullptr;
        if (kbio != nullptr) BIO_free(kbio);
        if (k == nullptr) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "Cannot read client private key PEM";
            return nullptr;
        }
        const int rc2 = SSL_CTX_use_PrivateKey(ctx, k);
        EVP_PKEY_free(k);
        if (rc2 != 1 || SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "Client certificate and private key do not match: " + opensslError();
            return nullptr;
        }
    }

    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        SSL_CTX_free(ctx);
        if (err != nullptr) *err = "SSL_new failed: " + opensslError();
        return nullptr;
    }

    auto stream = std::make_unique<TlsStream>(raw, ctx, ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_connect_state(ssl);

    if (!stream->handshake(err)) return nullptr;
    return stream;
}

}  // namespace core::usb
