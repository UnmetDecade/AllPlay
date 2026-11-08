/**
 * @file   lockdown.cpp
 * @brief  lockdown 请求/响应通道的实现
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
constexpr size_t      kMaxMessageBytes = 4 * 1024 * 1024;  ///< 手机不会发比这更大的
constexpr int         kDefaultTimeoutMs = 5000;

/// 按"还剩多少毫秒"读满 n 字节。返回 false = 超时或断流。
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
        if (err != nullptr) *err = "请求太大了";
        return false;
    }

    std::vector<uint8_t> frame(kLengthBytes + xml.size());
    const uint32_t n = uint32_t(xml.size());
    frame[0] = uint8_t(n >> 24);
    frame[1] = uint8_t(n >> 16);
    frame[2] = uint8_t(n >> 8);
    frame[3] = uint8_t(n);
    std::memcpy(frame.data() + kLengthBytes, xml.data(), xml.size());

    LOG_DEBUG("[%s] → %s", kTag, req.stringValue("Request", "(无 Request)").c_str());
    if (!ch_.writeAll(frame.data(), frame.size())) {
        if (err != nullptr) *err = "lockdown 请求写不出去";
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs
                                                                                   : kDefaultTimeoutMs);
    uint8_t hdr[kLengthBytes];
    if (!readFully(ch_, hdr, kLengthBytes, deadline)) {
        if (err != nullptr) *err = "等 lockdown 响应超时（连长度都没等到）";
        return false;
    }
    const uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) |
                         (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    if (len == 0 || len > kMaxMessageBytes) {
        if (err != nullptr) *err = "lockdown 响应长度不合理：" + std::to_string(len);
        return false;
    }

    std::string body(len, '\0');
    if (!readFully(ch_, reinterpret_cast<uint8_t*>(&body[0]), len, deadline)) {
        if (err != nullptr) *err = "lockdown 响应读了一半就断了";
        return false;
    }

    Plist parsed;
    if (!Plist::parse(body, &parsed, err)) return false;
    if (!parsed.isDict()) {
        if (err != nullptr) *err = "lockdown 响应不是 dict";
        return false;
    }

    // 手机报错时回的是 Error（字符串或整数）+ 可能带 ErrorString。
    // **这是最有价值的调试信息**：配对失败、服务不存在、设备没解锁，全靠它区分。
    if (const Plist* e = parsed.find("Error")) {
        std::string why = parsed.stringValue("ErrorString");
        if (why.empty()) {
            if (e->type() == Plist::Type::Integer) why = std::to_string(e->asInteger());
            else why = e->asString();
        }
        if (err != nullptr) {
            *err = "lockdown 报错（" + req.stringValue("Request", "?") + "）：" + why;
        }
        LOG_WARN("[%s] 手机报错：%s", kTag, why.c_str());
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
    // 手机不认这个 key 时回的是 Error —— 那是正常情况，不该当失败往上抛
    std::string localErr;
    if (!request(req, &resp, kDefaultTimeoutMs, &localErr)) {
        if (localErr.find("lockdown 报错") == 0) {
            LOG_DEBUG("[%s] 手机没有 %s（%s）", kTag, key.c_str(), localErr.c_str());
            return true;
        }
        if (err != nullptr) *err = localErr;
        return false;
    }
    const Plist* v = resp.find("Value");
    if (v == nullptr) {
        if (err != nullptr) *err = "GetValue(" + key + ") 的响应里没有 Value";
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
    // 注意：StartService 的请求**没有 Label**（DiPlay 就是这么发的）
    Plist req = Plist::dict();
    req.set("Request", Plist::string("StartService"));
    req.set("Service", Plist::string(service));

    Plist resp;
    if (!request(req, &resp, 10000, err)) return false;

    const Plist* p = resp.find("Port");
    if (p == nullptr || p->type() != Plist::Type::Integer) {
        if (err != nullptr) *err = "StartService 的响应里没有整数 Port";
        return false;
    }
    const int64_t v = p->asInteger();
    if (v < 1 || v > 0xffff) {
        if (err != nullptr) *err = "StartService 给的 Port 不合法：" + std::to_string(v);
        return false;
    }
    if (port != nullptr) *port = uint16_t(v);
    if (enableSsl != nullptr) *enableSsl = resp.boolValue("EnableServiceSSL", false);
    LOG_INFO("[%s] 服务 %s 在端口 %u（服务侧 TLS：%s）", kTag, service.c_str(), unsigned(v),
             (enableSsl != nullptr && *enableSsl) ? "要" : "不要");
    return true;
}

}  // namespace core::usb
