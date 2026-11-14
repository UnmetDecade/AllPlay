/**
 * @file   lockdown.cpp
 * @brief  Implementation of the lockdown request/response channel
 * @author blt
 */

#include "usb/lockdown.h"

#include "common/log.h"

#include <chrono>
#include <cstring>

namespace core::usb {
namespace {

constexpr const char* kTag = "lockdown";
constexpr size_t      kLengthBytes = 4;
constexpr size_t      kMaxMessageBytes = 4 * 1024 * 1024;  ///< the phone never sends anything larger
constexpr int         kDefaultTimeoutMs = 5000;

/// Read exactly n bytes using "how many ms remain". Returns false = timeout or stream closed.
bool readFully(IByteStream& ch, uint8_t* buf, size_t n,
               const std::chrono::steady_clock::time_point& deadline)
{
    size_t got = 0;
    while (got < n) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) return false;
        const int r = ch.read(buf + got, n - got, int(left));
        if (r < 0) return false;
        if (r == 0) continue;
        got += size_t(r);
    }
    return true;
}

}  // namespace

bool LockdownClient::request(const Plist& req, Plist* resp, int timeoutMs, std::string* err)
{
    const std::string xml = req.toXml();
    if (xml.size() > kMaxMessageBytes) {
        if (err != nullptr) *err = "Request is too large";
        return false;
    }

    std::vector<uint8_t> frame(kLengthBytes + xml.size());
    const uint32_t n = uint32_t(xml.size());
    frame[0] = uint8_t(n >> 24);
    frame[1] = uint8_t(n >> 16);
    frame[2] = uint8_t(n >> 8);
    frame[3] = uint8_t(n);
    std::memcpy(frame.data() + kLengthBytes, xml.data(), xml.size());

    LOG_DEBUG("[%s] → %s", kTag, req.stringValue("Request", "(no Request)").c_str());
    if (!ch_.writeAll(frame.data(), frame.size())) {
        if (err != nullptr) *err = "Failed to write lockdown request";
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs
                                                                                   : kDefaultTimeoutMs);
    uint8_t hdr[kLengthBytes];
    if (!readFully(ch_, hdr, kLengthBytes, deadline)) {
        if (err != nullptr) *err = "Timed out waiting for lockdown response (did not even receive the length)";
        return false;
    }
    const uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) |
                         (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    if (len == 0 || len > kMaxMessageBytes) {
        if (err != nullptr) *err = "Unreasonable lockdown response length: " + std::to_string(len);
        return false;
    }

    std::string body(len, '\0');
    if (!readFully(ch_, reinterpret_cast<uint8_t*>(&body[0]), len, deadline)) {
        if (err != nullptr) *err = "Connection dropped in the middle of reading the lockdown response";
        return false;
    }

    Plist parsed;
    if (!Plist::parse(body, &parsed, err)) return false;
    if (!parsed.isDict()) {
        if (err != nullptr) *err = "Lockdown response is not a dict";
        return false;
    }

    // On error the phone replies with Error (string or integer) + possibly ErrorString.
    // **This is the most valuable debugging info**: it distinguishes pairing failure, missing service, locked device.
    if (const Plist* e = parsed.find("Error")) {
        std::string why = parsed.stringValue("ErrorString");
        if (why.empty()) {
            if (e->type() == Plist::Type::Integer) why = std::to_string(e->asInteger());
            else why = e->asString();
        }
        if (err != nullptr) {
            *err = "lockdown error (" + req.stringValue("Request", "?") + "): " + why;
        }
        LOG_WARN("[%s] Phone reported an error: %s", kTag, why.c_str());
        return false;
    }

    if (resp != nullptr) *resp = std::move(parsed);
    return true;
}

bool LockdownClient::queryType(std::string* type, std::string* err)
{
    Plist req = Plist::dict();
    req.set("Label", Plist::string("allplay.wired"));
    req.set("Request", Plist::string("QueryType"));

    Plist resp;
    if (!request(req, &resp, kDefaultTimeoutMs, err)) return false;
    if (type != nullptr) *type = resp.stringValue("Type");
    return true;
}

bool LockdownClient::getValue(const std::string& key, Plist* value, bool* found, std::string* err)
{
    if (found != nullptr) *found = false;

    Plist req = Plist::dict();
    req.set("Label", Plist::string("allplay.wired"));
    req.set("Request", Plist::string("GetValue"));
    req.set("Key", Plist::string(key));

    Plist resp;
    // The phone replies with Error when it does not recognize the key — that is normal, not a failure to propagate
    std::string localErr;
    if (!request(req, &resp, kDefaultTimeoutMs, &localErr)) {
        if (localErr.find("lockdown error (") == 0) {
            LOG_DEBUG("[%s] Phone does not have %s (%s)", kTag, key.c_str(), localErr.c_str());
            return true;
        }
        if (err != nullptr) *err = localErr;
        return false;
    }
    const Plist* v = resp.find("Value");
    if (v == nullptr) {
        if (err != nullptr) *err = "Response to GetValue(" + key + ") has no Value";
        return false;
    }
    if (value != nullptr) *value = *v;
    if (found != nullptr) *found = true;
    return true;
}

bool LockdownClient::setValue(const std::string& key, const Plist& value, std::string* err)
{
    Plist req = Plist::dict();
    req.set("Label", Plist::string("allplay.wired"));
    req.set("Request", Plist::string("SetValue"));
    req.set("Key", Plist::string(key));
    req.set("Value", value);
    return request(req, nullptr, kDefaultTimeoutMs, err);
}

bool LockdownClient::removeValue(const std::string& key, std::string* err)
{
    Plist req = Plist::dict();
    req.set("Label", Plist::string("allplay.wired"));
    req.set("Request", Plist::string("RemoveValue"));
    req.set("Key", Plist::string(key));
    return request(req, nullptr, kDefaultTimeoutMs, err);
}

bool LockdownClient::startService(const std::string& service, uint16_t* port, bool* enableSsl,
                                  std::string* err)
{
    // NOTE: the StartService request has **no Label** (that is how DiPlay sends it)
    Plist req = Plist::dict();
    req.set("Request", Plist::string("StartService"));
    req.set("Service", Plist::string(service));

    Plist resp;
    if (!request(req, &resp, 10000, err)) return false;

    const Plist* p = resp.find("Port");
    if (p == nullptr || p->type() != Plist::Type::Integer) {
        if (err != nullptr) *err = "StartService response has no integer Port";
        return false;
    }
    const int64_t v = p->asInteger();
    if (v < 1 || v > 0xffff) {
        if (err != nullptr) *err = "Invalid Port from StartService: " + std::to_string(v);
        return false;
    }
    if (port != nullptr) *port = uint16_t(v);
    if (enableSsl != nullptr) *enableSsl = resp.boolValue("EnableServiceSSL", false);
    LOG_INFO("[%s] Service %s is on port %u (service-side TLS: %s)", kTag, service.c_str(), unsigned(v),
             (enableSsl != nullptr && *enableSsl) ? "yes" : "no");
    return true;
}

}  // namespace core::usb
