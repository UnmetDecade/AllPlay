/**
 * @file   aa_wire_test.cpp
 * @brief  Android Auto wire 层自测：AAP 分帧/拼帧 + protobuf 子集
 * @author blt
 */

// ============================================================================
// tools/aa_wire_test.cpp —— 不需要手机的 AA 自测
// ============================================================================
// AA 的协议验证最终要真机，但帧格式、长度字段、拼帧和 protobuf 编码可以先在这里
// 钉死。跑法：./build/aa_wire_test
#include "projection/androidauto/aap.h"
#include "projection/androidauto/proto.h"

#include <cstdio>
#include <vector>

using namespace core::projection::androidauto;

namespace {
int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)
}  // namespace

int main()
{
    // ---- 1. 单帧往返：control channel / plain / CONTROL ----
    const uint8_t payload[] = {0x00, 0x05, 'h', 'i'};
    auto frame = encodeFrame(Channel::Control, /*encrypted=*/false,
                             /*control=*/true, payload, sizeof payload,
                             /*first=*/true, /*last=*/true, 0);
    CHECK(frame.size() == 2 + 2 + sizeof payload);
    CHECK(frame[0] == 0x00);
    CHECK(frame[1] == 0x07);                 // FIRST|LAST|CONTROL
    CHECK(frame[2] == 0x00 && frame[3] == 0x04);

    FrameParser parser;
    parser.feed(frame.data(), frame.size());
    Frame out;
    CHECK(parser.next(&out));
    CHECK(out.channel == Channel::Control);
    CHECK(out.control && !out.encrypted && out.first && out.last);
    CHECK(out.payload.size() == sizeof payload);
    CHECK(out.payload[1] == 0x05);
    CHECK(!parser.next(&out));               // 没有第二帧

    // ---- 2. 分帧： > 0x4000，FIRST 带 6 字节长度（含明文总长）----
    const std::vector<uint8_t> big(kMaxFramePayload + 100, 0xAB);
    auto first = encodeFrame(Channel::Video, true, false, big.data(),
                             kMaxFramePayload, /*first=*/true, /*last=*/false,
                             uint32_t(big.size()));
    auto last = encodeFrame(Channel::Video, true, false,
                            big.data() + kMaxFramePayload, 100,
                            /*first=*/false, /*last=*/true, 0);

    CHECK(first[1] == 0x09);                 // FIRST|ENCRYPTED
    CHECK(first[2] == 0x40 && first[3] == 0x00);
    CHECK(first[4] == 0x00 && first[5] == 0x00 &&
          first[6] == 0x40 && first[7] == 0x64);   // total = 0x4064
    CHECK(last[1] == 0x0A);                  // LAST|ENCRYPTED

    parser.reset();
    parser.feed(first.data(), first.size());
    parser.feed(last.data(), last.size());
    Frame a, b;
    CHECK(parser.next(&a));
    CHECK(parser.next(&b));
    CHECK(a.first && !a.last);
    CHECK(a.totalSize == big.size());
    CHECK(b.last && !b.first);
    CHECK(a.payload.size() == kMaxFramePayload);
    CHECK(b.payload.size() == 100);

    // ---- 3. 半帧必须等下一次 feed ----
    parser.reset();
    parser.feed(first.data(), 5);
    CHECK(!parser.next(&a));
    parser.feed(first.data() + 5, first.size() - 5);
    CHECK(parser.next(&a));

    // ---- 4. protobuf varint / length-delimited ----
    std::vector<uint8_t> msg;
    pb::putUInt(msg, 1, 2);
    pb::putString(msg, 2, "abc");
    CHECK(pb::Reader::getVarint(msg.data(), msg.size(), 1) == 2);

    pb::Reader r(msg.data(), msg.size());
    uint32_t field = 0;
    pb::WireType wt = pb::kVarint;
    uint64_t value = 0;
    const uint8_t* bytes = nullptr;
    size_t len = 0;
    CHECK(r.next(&field, &wt, &value, &bytes, &len));
    CHECK(field == 1 && wt == pb::kVarint && value == 2);
    CHECK(r.next(&field, &wt, &value, &bytes, &len));
    CHECK(field == 2 && wt == pb::kLengthDelimited && len == 3 &&
          bytes[0] == 'a');
    CHECK(!r.next(&field, &wt, &value, &bytes, &len));

    // varint 300 = 0xAC 0x02
    std::vector<uint8_t> var;
    pb::putVarint(var, 300);
    CHECK(var.size() == 2 && var[0] == 0xAC && var[1] == 0x02);

    if (failures == 0) std::printf("aa_wire_test: OK\n");
    return failures == 0 ? 0 : 1;
}
