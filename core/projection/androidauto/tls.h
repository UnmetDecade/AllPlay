/**
 * @file   tls.h
 * @brief  Android Auto 的 TLS 1.2 客户端（记录经 AAP SSL_HANDSHAKE 消息搬运）
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/tls.h —— AA 的 TLS 1.2 客户端
// ============================================================================
// 与普通 TLS 的差别：TLS 记录**不是**裸字节流，握手阶段每条记录是 AAP 控制通道
// 0x0003（SSL_HANDSHAKE）消息的 body；握手完成后，每条业务消息单独过一遍
// SSL_write / SSL_read（aasdk 的做法是 1:1，这里沿用）。
//
// 证书：运行时自己生成 RSA-2048 自签名证书（CN=AllPlay），不复用任何厂商证书。
// 固定 TLS 1.2（aasdk 在旧 OpenSSL 路径上也是 TLSv1_2_client_method）。
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

    /// 对端数据已 ingest 后调用。NeedMore = 继续等对端记录。
    Handshake handshake(std::string* err);

    bool ingest(const uint8_t* data, size_t len);
    std::vector<uint8_t> takeOutput();

    bool encrypt(const std::vector<uint8_t>& plain, std::vector<uint8_t>* cipher,
                 std::string* err);
    /// 解密一帧。对端数据不完整时返回 true 且 plain 为空（等下一帧）。
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
