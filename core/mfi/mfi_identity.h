/**
 * @file   mfi_identity.h
 * @brief  MFi 配件身份（接口 + 证书实现）
 * @author blt
 */

// ============================================================================
// core/mfi/mfi_identity.h —— MFi 配件身份（接口 + 证书实现）
// ============================================================================
// 协议层（Session / iAP2）只依赖 IMfiIdentity 接口：需要「证书」和「签名」两个能力。
// 具体是谁签的不关心，所以两种实现都能接：
//   1. 证书实现（CertificateMfiIdentity）：identity.pk8 + certificate.p7b，软件私钥签名。
//   2. 硬件实现：MFi 认证芯片（EAP / NXP 等），把 32 字节摘要丢给芯片，芯片回 64 字节 raw r‖s。
//
// 身份文件为单独引入的 MFi 凭证（scripts/extract-identity.sh 提取）：
//   identity.pk8     PKCS#8 DER，EC P-256 私钥
//   certificate.p7b  PKCS#7 DER，单张配件证书（= 0xAA01 要发的内容）
//
// 签名语义（对照 MFi-SAP 认证客户端（LocalMfiAuthenticationClient）的行为）：
//   收到 0xAA02 的挑战（32 字节**摘要**）→ 直接 ECDSA 签（**不再哈希**）→
//   输出 **64 字节 raw r‖s**（不是 DER）。
#pragma once

// ALLPLAY_MFI_STUB：没有 OpenSSL 的构建（HarmonyOS 第一阶段只验链路层/识别，
// 还没交叉编译 OpenSSL）。此时这个类只保留接口形状，load() 恒失败，
// 无线触发在 mfiDir 为空时不加载身份、发空证书 —— 够验证到 0x1D02。
#ifdef ALLPLAY_MFI_STUB
using EVP_PKEY = void;
#else
#include <openssl/evp.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>

namespace core::mfi {

/// MFi 配件身份接口：提供证书 + 对挑战摘要签名。
/// 硬件 MFi 芯片或简单证书，只要实现这两个方法即可接入。
class IMfiIdentity {
public:
    virtual ~IMfiIdentity() = default;

    /// 配件证书（= iAP2 0xAA01 要发的内容）
    virtual const std::vector<uint8_t>& certificate() const = 0;

    /// 签 32 字节摘要 → 64 字节 raw r‖s；失败 ok=false
    virtual std::vector<uint8_t> signDigest(const uint8_t* digest, size_t len,
                                            bool* ok = nullptr) const = 0;
};

/// 证书实现（软件私钥签名，默认路径）
class CertificateMfiIdentity : public IMfiIdentity {
public:
    ~CertificateMfiIdentity() override;
    CertificateMfiIdentity() = default;
    CertificateMfiIdentity(const CertificateMfiIdentity&) = delete;
    CertificateMfiIdentity& operator=(const CertificateMfiIdentity&) = delete;

    /// 从目录加载（identity.pk8 / certificate.p7b）；失败写 err 并返回 nullptr
    static std::unique_ptr<CertificateMfiIdentity> load(const std::string& dir, std::string* err);

    /// 从内存加载（字节来自 HAP rawfile 之类读不到路径的地方）；失败写 err 并返回 nullptr
    static std::unique_ptr<CertificateMfiIdentity> loadFromMemory(const uint8_t* keyDer, size_t keyLen,
                                                                  const uint8_t* certDer, size_t certLen,
                                                                  std::string* err);

    const std::vector<uint8_t>& certificate() const override { return cert_; }
    std::vector<uint8_t> signDigest(const uint8_t* digest, size_t len,
                                    bool* ok = nullptr) const override;

private:
    EVP_PKEY*            key_ = nullptr;
    std::vector<uint8_t> cert_;
};

}  // namespace core::mfi
