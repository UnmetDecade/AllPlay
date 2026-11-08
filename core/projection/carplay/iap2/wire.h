/**
 * @file   wire.h
 * @brief  iAP2 线格式：CSM 帧 + 参数编解码（大端）
 * @author blt
 */

// ============================================================================
// core/iap2/wire.h —— iAP2 线格式：CSM 帧 + 参数编解码（大端）
// ============================================================================
// 协议事实（对照 iAP2 帧/线格式整理）：
//   CSM 帧：`0x4040 | u16 帧长 | u16 msgId | 体`（帧长含 6 字节头；无校验和）
//   参数  ：`u16 参数长 | u16 参数id | 载荷`（参数长含 4 字节头）
//   字符串：UTF-8 + 尾部 NUL（长度含 NUL）；字符串列表 = 若干 NUL 结尾串拼接
//   u16 列表：逐个大端 u16；void：空载荷；group：嵌套的参数表（就是子参数按序编码）
// 这里只做"字节怎么摆"，不含业务语义（业务在 wireless_client.cpp）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

namespace core::iap2 {

// ---------------------------------------------------------------------------
// 参数构造（有序、允许重复 id —— 与协议一致）
// ---------------------------------------------------------------------------
class Params {
public:
    Params& u8(uint16_t id, uint8_t v);
    Params& u16(uint16_t id, uint16_t v);
    Params& u32(uint16_t id, uint32_t v);
    Params& voidv(uint16_t id);                          // 空载荷参数
    Params& bytes(uint16_t id, const std::vector<uint8_t>& v);
    Params& str(uint16_t id, const std::string& v);      // UTF-8 + NUL
    Params& strList(uint16_t id, const std::vector<std::string>& v);
    Params& u16List(uint16_t id, const std::vector<uint16_t>& v);
    Params& group(uint16_t id, const Params& child);     // 嵌套参数表

    std::vector<uint8_t> encode() const;

private:
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> items_;
};

// ---------------------------------------------------------------------------
// 参数解析（收对端报文）
// ---------------------------------------------------------------------------
struct Param {
    uint16_t             id = 0;
    std::vector<uint8_t> payload;
};

/// 解析参数表；遇到畸形（长度越界）返回 false
bool parseParams(const uint8_t* p, size_t n, std::vector<Param>* out);

/// 在参数表里找第一个 id 匹配的参数；没有返回 nullptr
const Param* findParam(const std::vector<Param>& params, uint16_t id);

/// 读字符串参数（去尾部 NUL；无 NUL 返回空并置 ok=false）
std::string paramString(const Param& p, bool* ok = nullptr);
/// 读 u8 / u16 / u32（长度不符返回 0，ok=false）
uint8_t  paramU8(const Param& p, bool* ok = nullptr);
uint16_t paramU16(const Param& p, bool* ok = nullptr);
uint32_t paramU32(const Param& p, bool* ok = nullptr);

// ---------------------------------------------------------------------------
// CSM 帧
// ---------------------------------------------------------------------------
std::vector<uint8_t> csmFrame(uint16_t msgId, const std::vector<uint8_t>& body);

/// 流式拆帧：喂任意分片，产出完整帧（容忍 0x4040 前的垃圾与拼接）
class CsmFramer {
public:
    void feed(const uint8_t* p, size_t n);
    /// 取下一个完整帧；没有返回 false
    bool next(uint16_t* msgId, std::vector<uint8_t>* body);
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

}  // namespace core::iap2
