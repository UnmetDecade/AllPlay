/**
 * @file   tls_stream.h
 * @brief  在任意 core::IByteStream 上做 TLS 客户端（自定义 BIO，不需要 socket fd）
 * @author blt
 */

// ============================================================================
// core/usb/tls_stream.h —— 套在任意字节流上的 TLS 客户端
// ============================================================================
// 有线的 lockdown 在 StartSession(EnableSessionSSL) 之后**原地**升级成 TLS：
// 还是那条迷你 TCP 连接，只不过之后的字节都是 TLS 记录。而底下不是 socket，
// 所以不能用 SSL_set_fd —— 这里给 OpenSSL 挂一对自定义 BIO，读写都转到传入的流上。
//
// **不校验对端证书**（SSL_VERIFY_NONE）。这不是偷懒：这条链路里手机的证书是它自己
// 现场签的，我们手上没有任何可信根，校验没有意义。真正证明"对面是那台手机"的是
// 前面 usbmux 那条物理通道（USB 线）本身。
#pragma once

#include "port/stream.h"

#include <memory>
#include <string>

namespace core::usb {

/// 把 raw 包成 TLS 客户端流。certPem/keyPem 是**我们要出示的**客户端身份（PEM 格式）。
/// certPem 为空表示不做客户端认证。失败返回 nullptr，err 是给人看的原因。
///
/// ⚠️ raw 不归返回的流所有 —— 它必须比返回的流活得久。
std::unique_ptr<IByteStream> startTlsClient(IByteStream& raw, const std::string& certPem,
                                            const std::string& keyPem, std::string* err);

}  // namespace core::usb
