/**
 * @file   pair_record.h
 * @brief  有线配对要用的本地证书材料（root / host / device 三张）
 * @author blt
 */

// ============================================================================
// core/usb/pair_record.h —— 有线配对的自造证书
// ============================================================================
// 有线这条路的配对**不是**无线那套 SRP（pair-setup/pair-verify），而是明文 Pair，
// 证书全部**本地现造**，手机那把公钥也只用来"签一张属于它的证书"：
//
//   root   —— 自签（空 DN、serial 0、CA:TRUE）。进 Pair 请求；**之后 TLS 的客户端身份也是它**
//   host   —— root 签（空 DN、CA:FALSE）
//   device —— root 签，但**证书里的公钥是手机通过 GetValue(DevicePublicKey) 给的**
//             （所以这把私钥在手机里，我们签不出东西来）
//
// 形状照抄 DiPlay：RSA-2048 / SHA256withRSA / 有效期 10 年 / 空 issuer 与 subject DN /
// serial 恒 0。**别自作主张加 CN 或改 serial** —— 手机认不认是实测出来的事，改了得重测。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core::usb {

/// 一次配对所需的全部本地材料
struct WiredPairIdentity {
    std::string rootCertPem;    ///< 自签 root 证书（PEM 文本）
    std::string rootKeyPem;     ///< root 私钥（PKCS#8 PEM）——TLS 客户端身份用它
    std::string hostCertPem;    ///< host 证书（PEM 文本）
    std::string hostKeyPem;
    std::string deviceCertPem;  ///< 给手机签的 device 证书（PEM 文本）
    std::string hostId;         ///< 大写 UUID
    std::string systemBuid;     ///< 大写 UUID
};

/// 生成配对材料。devicePublicKey = lockdown GetValue("DevicePublicKey") 拿到的原始字节。
///
/// **别假设它是 DER**：文档与参照实现都说这里是 PKCS#1 RSAPublicKey DER，但真机上
/// 拿到的是 431 字节的 **PEM 文本**。本函数自己认格式（PEM / PKCS#1 / SPKI 都吃），
/// 所以调用方原样透传即可，不要提前转码。
///
/// hostId / systemBuid 由调用方给：**SetValue(UntrustedHostBUID) 必须和后面 Pair 里
/// 写的 SystemBUID 是同一个**，所以这对 UUID 得在配对流程开始前就定下来。传空串则现生成。
bool makeWiredPairIdentity(const std::vector<uint8_t>& devicePublicKey, const std::string& hostId,
                           const std::string& systemBuid, WiredPairIdentity* out,
                           std::string* err);

/// 大写 UUID（HostID / SystemBUID 都用它；手机要的是大写）
std::string makeUuidUpper();

// ----------------------------------------------------------------------------
// 落盘 / 复读：**免掉每次都要在手机上点"信任"**
// ----------------------------------------------------------------------------
// iOS 认"这台电脑"认的是 **HostID + 这一套证书**（它自己那边也存了一份 pair record）。
// 每跑一次就现造一套新的，手机每次都当陌生人 → 每次弹一次"要信任此电脑吗？"。
// 存下来复用，弹窗就该只出现第一次。
//
// key 用**手机序列号**（USB 描述符里就有），不用 DevicePublicKey —— 顺序上做不到：
// Pair 之前必须先报 SetValue(UntrustedHostBUID)，而那个 BUID 就在这份记录里，
// 那会儿还没读到 DevicePublicKey。
//
// 目录：<stateDir>/wired-pair/<key>/（stateDir 空 = defaultStateDir()）。

/// 只读回 HostID / SystemBUID（报 UntrustedHostBUID 要用）。没有记录返回 false，不算错。
bool loadWiredPairIds(const std::string& stateDir, const std::string& key, std::string* hostId,
                      std::string* systemBuid);

/// 读回整套材料。**devicePublicKey 对不上就当没有**（换了手机、手机重装、记录过期）：
/// 那种情况下 *found 回 false，调用方重新造一套并覆盖。
bool loadWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, WiredPairIdentity* out,
                           bool* found, std::string* err);

/// 落盘（私钥 0600）。失败返回 false —— 但**调用方不该因此判整个配对流失败**：
/// 存不下最多是下次再弹一次信任框。
bool saveWiredPairIdentity(const std::string& stateDir, const std::string& key,
                           const std::vector<uint8_t>& devicePublicKey, const WiredPairIdentity& id,
                           std::string* err);

/// 只读证书那几张（**不校验手机公钥**）。给"不再发 Pair"的快路径用：
/// 那条路上 device 证书根本用不上（它只出现在 PairRecord 里），而校验指纹需要
/// 先跟手机要 DevicePublicKey —— 快路径的意义就是不去要。
bool loadWiredPairCerts(const std::string& stateDir, const std::string& key, WiredPairIdentity* out,
                        bool* found, std::string* err);

// ---- "这对材料配对成功过"的印记 ------------------------------------------
// 为什么要单独记一笔：**配对成功 ≠ 有记录可复用**。上次卡在弹窗上时，材料也已经
// 落盘了（那时候就存了，不然 HostID 定不下来），但手机那边并没有认过它 ——
// 拿这种记录去跳过 Pair，等于端着一套手机没见过的证书去开加密会话，必然失败。
// 所以只有**真配成功过**才盖这个戳，下一轮才敢跳过 Pair。
bool markWiredPairTrusted(const std::string& stateDir, const std::string& key, std::string* err);
bool isWiredPairTrusted(const std::string& stateDir, const std::string& key);

}  // namespace core::usb
