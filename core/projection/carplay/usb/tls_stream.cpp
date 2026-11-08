/**
 * @file   tls_stream.cpp
 * @brief  套在任意字节流上的 TLS 客户端（自定义 BIO）
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
constexpr int         kBioPollMs = 200;          ///< 底层流一次最多等多久
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
    return out.empty() ? "（OpenSSL 没给错误码）" : out;
}

/// BIO 背后的状态：那条被包住的流，外加"这次调用最多等到什么时候"
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
            // **返回前必须清掉重试标志。** 那个标志是上面那次超时（BIO_set_retry_read）
            // 设下的，会一直黏在 BIO 上；带着它返回 0，OpenSSL 把这次读判成
            // "还没数据，稍后再来"（SSL_ERROR_WANT_READ），**而不是 EOF** ——
            // 于是"对端断了"这件事永远传不到上层：上层只会看到 0（超时，没数据），
            // 以为链路好好的，原地空转。
            // 有线这条路上真出过事：拔线之后泵线程 90% CPU 空转，每 2.5 分钟写 2 GB 日志。
            LOG_DEBUG("[%s] 底层流断开", kTag);
            BIO_clear_retry_flags(b);
            return 0;  // EOF：让 SSL 走"对端关了"那条路
        }
        // 底层只是超时。到点了就告诉 OpenSSL "现在没数据"，别在这儿死等 ——
        // 上层 read(timeoutMs) 的语义（0 = 超时）得保住。
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
    return inl;  // writeAll 是全量写：要么全出去，要么失败
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
        bioCtx_.raw = &raw;  // raw 不归我们所有：调用方保证它比本对象活得久
        BIO* bio = BIO_new(streamBioMethod());
        BIO_set_data(bio, &bioCtx_);
        BIO_set_init(bio, 1);
        SSL_set_bio(ssl_, bio, bio);  // 读写同一个 BIO；所有权归 ssl_，由 SSL_free 释放
    }

    ~TlsStream() override
    {
        closed_ = true;
        if (ssl_ != nullptr) SSL_free(ssl_);  // 连带释放 bio_
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
            LOG_INFO("[%s] 握手完成：%s / %s", kTag, SSL_get_version(ssl_), SSL_get_cipher(ssl_));
            return true;
        }
        const int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (std::chrono::steady_clock::now() >= bioCtx_.deadline) {
                if (err != nullptr) *err = "TLS 握手超时（30s）";
                return false;
            }
            continue;
        }
        if (err != nullptr) *err = "TLS 握手失败：" + opensslError();
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
        if (e == SSL_ERROR_ZERO_RETURN) return -1;  // 对端 close_notify
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (std::chrono::steady_clock::now() >= bioCtx_.deadline) return 0;  // 超时
            continue;
        }
        if (e == SSL_ERROR_SYSCALL && r == 0) return -1;  // 没发 close_notify 就断了
        LOG_WARN("[%s] 读失败：%s", kTag, opensslError().c_str());
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
                LOG_WARN("[%s] 写超时", kTag);
                return false;
            }
            continue;
        }
        LOG_WARN("[%s] 写失败：%s", kTag, opensslError().c_str());
        return false;
    }
    return true;
}

void TlsStream::close()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) return;
    closed_ = true;
    SSL_shutdown(ssl_);  // 尽力而为：发个 close_notify，对面不接也无所谓
}

}  // namespace

std::unique_ptr<IByteStream> startTlsClient(IByteStream& raw, const std::string& certPem,
                                            const std::string& keyPem, std::string* err)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        if (err != nullptr) *err = "建 SSL_CTX 失败：" + opensslError();
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // 对端证书不校验：这条链路里手机的证书是它自己现场签的，我们没有任何可信根。
    // "对面是那台手机"由前面的 usbmux 物理通道（USB 线）保证，不靠证书。
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    if (!certPem.empty() && !keyPem.empty()) {
        BIO* cbio = BIO_new_mem_buf(certPem.data(), int(certPem.size()));
        X509* x = cbio != nullptr ? PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr) : nullptr;
        if (cbio != nullptr) BIO_free(cbio);
        if (x == nullptr) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "客户端证书 PEM 读不出来";
            return nullptr;
        }
        const int rc = SSL_CTX_use_certificate(ctx, x);
        X509_free(x);
        if (rc != 1) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "SSL_CTX 装客户端证书失败：" + opensslError();
            return nullptr;
        }

        BIO* kbio = BIO_new_mem_buf(keyPem.data(), int(keyPem.size()));
        EVP_PKEY* k =
            kbio != nullptr ? PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, nullptr) : nullptr;
        if (kbio != nullptr) BIO_free(kbio);
        if (k == nullptr) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "客户端私钥 PEM 读不出来";
            return nullptr;
        }
        const int rc2 = SSL_CTX_use_PrivateKey(ctx, k);
        EVP_PKEY_free(k);
        if (rc2 != 1 || SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            if (err != nullptr) *err = "客户端证书与私钥不配对：" + opensslError();
            return nullptr;
        }
    }

    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        SSL_CTX_free(ctx);
        if (err != nullptr) *err = "SSL_new 失败：" + opensslError();
        return nullptr;
    }

    auto stream = std::make_unique<TlsStream>(raw, ctx, ssl);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_connect_state(ssl);

    if (!stream->handshake(err)) return nullptr;
    return stream;
}

}  // namespace core::usb
