/**
 * @file   mfi_identity_stub.cpp
 * @brief  MFi 身份的"空实现"（无 OpenSSL 的构建专用，见 ALLPLAY_MFI_STUB）
 * @author blt
 */

// ============================================================================
// core/mfi/mfi_identity_stub.cpp —— MFi 身份的空实现
// ============================================================================
// 只在 ALLPLAY_MFI_STUB 的构建里编（HarmonyOS 第一阶段）。目的不是"能签名"，
// 而是让 core/iap2 的代码按原样编进来、跑到识别与 MFi 请求那一步：
//   · mfiDir 为空 → WirelessTrigger 不加载身份，0xAA01 发空证书
//     → 手机回 0xAA04（认证失败）。看到 0xAA04 就说明识别已被接受、
//       链路层和 CSM 都通了，剩下的只有 MFi 这一块。
//   · mfiDir 非空 → load() 失败，WirelessTrigger 立刻报错退出，
//     避免"看起来跑起来了其实从来没签过名"。
#include "mfi/mfi_identity.h"

#include "common/log.h"

namespace core::mfi {

CertificateMfiIdentity::~CertificateMfiIdentity() = default;

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::load(const std::string& dir,
                                                                     std::string* err)
{
    const std::string why =
        "此构建没有 OpenSSL，不能加载 MFi 身份（" + dir + "）；先让 mfiDir 留空，只验识别";
    LOG_WARN("%s", why.c_str());
    if (err) *err = why;
    return nullptr;
}

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::loadFromMemory(
    const uint8_t*, size_t, const uint8_t*, size_t, std::string* err)
{
    const std::string why =
        "此构建没有 OpenSSL，不能签名（鸿蒙系统 crypto 的 ECDSA 会再哈希一次，"
        "做不了 MFi 要对摘要直接签的要求）—— 先跑 scripts/build-ohos-openssl.sh";
    LOG_WARN("%s", why.c_str());
    if (err) *err = why;
    return nullptr;
}

std::vector<uint8_t> CertificateMfiIdentity::signDigest(const uint8_t*, size_t, bool* ok) const
{
    if (ok) *ok = false;
    return {};
}

}  // namespace core::mfi
