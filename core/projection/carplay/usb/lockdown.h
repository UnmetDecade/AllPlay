/**
 * @file   lockdown.h
 * @brief  lockdown channel: 4-byte big-endian length + XML plist
 * @author blt
 */

// ============================================================================
// core/usb/lockdown.h — lockdown request/response channel
// ============================================================================
// lockdown is the 62078 service on the phone. One request, one response; both sides are plist dicts,
// framed as **4-byte big-endian length + XML plist text** (length excludes its own 4 bytes).
//
// It only knows IByteStream and **does not care whether the layer below is encrypted** — because this channel upgrades "in place":
//
//   mux mini TCP → lockdown (plaintext) → StartSession(EnableSessionSSL)
//                → the same connection becomes TLS → build another LockdownClient and continue
//
// So this class can wrap either a plaintext mini TCP or usb/tls_stream.
#pragma once

#include "port/stream.h"
#include "usb/plist_xml.h"

#include <cstdint>
#include <string>

namespace core::usb {

class LockdownClient {
public:
    explicit LockdownClient(IByteStream& ch) : ch_(ch) {}

    /// Send one request, receive one response. req usually carries at least Label and Request.
    /// Returns false when the phone reports an error (the response has an Error key); err holds the phone's own words.
    bool request(const Plist& req, Plist* resp, int timeoutMs, std::string* err);

    /// Probe the service type before handshake, confirming the channel actually works
    bool queryType(std::string* type, std::string* err);

    /// GetValue: found reports whether the phone recognizes this key (if not it returns Error; not treated as failure here)
    bool getValue(const std::string& key, Plist* value, bool* found, std::string* err);

    /// SetValue / RemoveValue
    bool setValue(const std::string& key, const Plist& value, std::string* err);
    bool removeValue(const std::string& key, std::string* err);

    /// StartService: get the service port. enableSsl reports whether the **second connection** also needs TLS.
    bool startService(const std::string& service, uint16_t* port, bool* enableSsl, std::string* err);

private:
    IByteStream& ch_;
};

}  // namespace core::usb
