/**
 * @file   tls_stream.h
 * @brief  TLS client over any core::IByteStream (custom BIO, no socket fd needed)
 * @author blt
 */

// ============================================================================
// core/usb/tls_stream.h — TLS client wrapped around any byte stream
// ============================================================================
// Wired lockdown upgrades to TLS **in place** after StartSession(EnableSessionSSL):
// still the same mini TCP connection, only later bytes are TLS records. The layer below is not a socket,
// so SSL_set_fd cannot be used — instead we attach a custom BIO pair to OpenSSL, routing reads and writes to the supplied stream.
//
// **No peer certificate verification** (SSL_VERIFY_NONE). Not laziness: on this link the phone's certificate is signed
// by itself on the spot, we hold no trusted root, so verification is meaningless. What actually proves "the other side is that phone" is
// the usbmux physical channel (the USB cable) itself.
#pragma once

#include "port/stream.h"

#include <memory>
#include <string>

namespace core::usb {

/// Wrap raw into a TLS client stream. certPem/keyPem are the client identity **we present** (PEM format).
/// Empty certPem means no client authentication. Returns nullptr on failure; err is a human-readable reason.
///
/// ⚠️ raw is not owned by the returned stream — it must outlive it.
std::unique_ptr<IByteStream> startTlsClient(IByteStream& raw, const std::string& certPem,
                                            const std::string& keyPem, std::string* err);

}  // namespace core::usb
