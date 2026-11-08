/**
 * @file   lockdown.h
 * @brief  lockdown 通道：4 字节大端长度 + XML plist
 * @author blt
 */

// ============================================================================
// core/usb/lockdown.h —— lockdown 的请求/响应通道
// ============================================================================
// lockdown 就是手机上的那个 62078 服务。一条请求一条响应，两边都是 plist dict，
// 成帧是 **4 字节大端长度 + XML plist 文本**（长度不含它自己那 4 字节）。
//
// 它只认 IByteStream，**不关心底下加没加密** —— 因为这条通道会"原地升级"：
//
//   mux 迷你 TCP → lockdown（明文）→ StartSession(EnableSessionSSL)
//                → 同一条连接变成 TLS → 再建一个 LockdownClient 接着用
//
// 所以本类既能套在明文的迷你 TCP 上，也能套在 usb/tls_stream 上。
#pragma once

#include "port/stream.h"
#include "usb/plist_xml.h"

#include <cstdint>
#include <string>

namespace core::usb {

class LockdownClient {
public:
    explicit LockdownClient(IByteStream& ch) : ch_(ch) {}

    /// 发一条请求、收一条响应。req 通常至少带 Label 与 Request 两个键。
    /// 手机报错时（响应里有 Error 键）返回 false，err 里是手机的原话。
    bool request(const Plist& req, Plist* resp, int timeoutMs, std::string* err);

    /// 握手前探一下服务类型，顺便确认通道真的通了
    bool queryType(std::string* type, std::string* err);

    /// GetValue：found 报告手机认不认这个 key（不认时它回 Error，这里不算失败）
    bool getValue(const std::string& key, Plist* value, bool* found, std::string* err);

    /// SetValue / RemoveValue
    bool setValue(const std::string& key, const Plist& value, std::string* err);
    bool removeValue(const std::string& key, std::string* err);

    /// StartService：拿服务端口。enableSsl 报告**第二条连接**要不要再做 TLS。
    bool startService(const std::string& service, uint16_t* port, bool* enableSsl, std::string* err);

private:
    IByteStream& ch_;
};

}  // namespace core::usb
