/**
 * @file   mfi_identity_stub.cpp
 * @brief  "Empty implementation" of MFi identity (only for builds without OpenSSL; see ALLPLAY_MFI_STUB)
 * @author blt
 */

// ============================================================================
// core/mfi/mfi_identity_stub.cpp - Empty implementation of MFi identity
// ============================================================================
// Compiled only in ALLPLAY_MFI_STUB builds (HarmonyOS phase one). The goal is not "being able to sign";
// it is to let the core/iap2 code compile unchanged and get as far as identification and the MFi request:
//   · mfiDir empty -> WirelessTrigger loads no identity and 0xAA01 sends an empty certificate
//     -> the phone replies 0xAA04 (authentication failed). Seeing 0xAA04 means identification was accepted and
//       both the link layer and CSM work; only MFi remains.
//   · mfiDir non-empty -> load() fails and WirelessTrigger errors out immediately,
//     avoiding a state that "looks like it is running but has never actually signed".
#include "mfi/mfi_identity.h"

#include "common/log.h"

namespace core::mfi {

CertificateMfiIdentity::~CertificateMfiIdentity() = default;

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::load(const std::string& dir,
                                                                     std::string* err)
{
    const std::string why =
        "This build has no OpenSSL and cannot load the MFi identity (" + dir + "); leave mfiDir empty to test identification only";
    LOG_WARN("%s", why.c_str());
    if (err) *err = why;
    return nullptr;
}

std::unique_ptr<CertificateMfiIdentity> CertificateMfiIdentity::loadFromMemory(
    const uint8_t*, size_t, const uint8_t*, size_t, std::string* err)
{
    const std::string why =
        "This build has no OpenSSL and cannot sign (the HarmonyOS crypto ECDSA hashes the digest again, "
        "which cannot satisfy MFi's requirement to sign the digest directly) - run scripts/build-ohos-openssl.sh first";
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
