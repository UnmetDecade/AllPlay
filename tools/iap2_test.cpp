// ============================================================================
// tools/iap2_test.cpp —— iAP2/MFi 的自测（不插硬件能验的最高程度）
// ============================================================================
// 覆盖：
//   ① 线格式：参数编解码往返、字符串 NUL、CSM 拆帧（含垃圾前置/分片）
//   ①b 方控：媒体键 → HID usage/报表 → 0x4E02 报文、按键队列的进出与溢出
//   ② 链路层：两个 LinkEngine 经内存管道握手（marker→SYN→ACK→NORMAL），
//      控制载荷双向可靠送达、序号/校验和路径正确
//   ③ MFi 签名：identity/ 加载 + 32 字节摘要签名 + 用证书公钥验签
//   ④ 全流程：WirelessTrigger ←→ "假 iPhone"（用同一套引擎实现的对端），
//      跑完 识别→MFi→订阅→0x5703→0x4301
//
// 用法： iap2_test [identity目录]   （默认 <项目根>/identity）
#include "iap2/hid.h"
#include "iap2/link.h"
#include "iap2/wire.h"
#include "iap2/wireless_client.h"
#include "mfi/mfi_identity.h"
#include "port/control.h"

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("   ✅ %s\n", msg); }                       \
        else      { std::printf("   ❌ %s\n", msg); ++g_fail; }             \
    } while (0)

uint64_t nowMs()
{
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// 线程安全的内存管道（两端互喂字节）
// ---------------------------------------------------------------------------
class Pipe {
public:
    void push(const uint8_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.insert(buf_.end(), p, p + n);
        cv_.notify_all();
    }
    int read(uint8_t* out, size_t n, int timeoutMs)
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [&] { return !buf_.empty() || closed_; })) {
            return 0;   // 超时
        }
        if (buf_.empty()) return -1;   // 关闭
        const size_t take = std::min(n, buf_.size());
        std::copy(buf_.begin(), buf_.begin() + take, out);
        buf_.erase(buf_.begin(), buf_.begin() + take);
        return int(take);
    }
    void close()
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<uint8_t>     buf_;
    bool                    closed_ = false;
};

class PipeStream : public core::IByteStream {
public:
    PipeStream(Pipe* in, Pipe* out) : in_(in), out_(out) {}
    int  read(uint8_t* buf, size_t n, int timeoutMs) override { return in_->read(buf, n, timeoutMs); }
    bool writeAll(const uint8_t* buf, size_t n) override { out_->push(buf, n); return true; }

private:
    Pipe* in_;
    Pipe* out_;
};

// ---------------------------------------------------------------------------
// ① 线格式
// ---------------------------------------------------------------------------
void testWire()
{
    std::printf("== ① 线格式 ==\n");
    using namespace core::iap2;

    Params p;
    p.u8(1, 7).u16(2, 0x1234).u32(3, 0xDEADBEEF).str(4, "hello").u16List(5, {1, 2, 3}).voidv(6);
    Params g;
    g.u8(0, 1).str(1, "abc");
    p.group(7, g);
    auto enc = p.encode();
    std::vector<Param> back;
    CHECK(parseParams(enc.data(), enc.size(), &back), "参数编解码往返");
    bool ok = false;
    CHECK(back.size() == 7, "参数个数 7");
    CHECK(back[3].payload.size() == 6 && back[3].payload.back() == 0 &&
          paramString(back[3], &ok) == "hello" && ok, "字符串带 NUL");
    CHECK(back[2].payload.size() == 4 && paramU32(back[2]) == 0xDEADBEEF, "u32 大端");
    CHECK(back[4].payload.size() == 6 && back[4].payload[5] == 3 && back[4].payload[1] == 1, "u16 列表");
    std::vector<Param> gsub;
    CHECK(parseParams(back[6].payload.data(), back[6].payload.size(), &gsub) &&
          gsub.size() == 2 && paramString(gsub[1]) == "abc", "group 嵌套");

    auto frame = csmFrame(0x4301, enc);
    CsmFramer fr;
    std::vector<uint8_t> junk = {0x00, 0xff, 0x41};   // 不以 0x40 结尾（否则与帧头拼成假 4040）
    fr.feed(junk.data(), junk.size());              // 帧前的垃圾（对齐用）
    fr.feed(frame.data(), 3);                       // 再分片喂
    uint16_t id = 0;
    std::vector<uint8_t> body;
    CHECK(!fr.next(&id, &body), "分片未齐不发帧");
    fr.feed(frame.data() + 3, frame.size() - 3);
    CHECK(fr.next(&id, &body) && id == 0x4301 && body == enc, "CSM 拆帧（容忍前置垃圾/分片）");
}

// ---------------------------------------------------------------------------
// ①b 方控（媒体键 → HID 报表 → iAP2 报文）
// ---------------------------------------------------------------------------
void testHid()
{
    std::printf("== ①b 方控编码 ==\n");
    using namespace core::iap2;

    const HidUsage next = mediaKeyUsage(core::MediaKey::NextTrack);
    CHECK(next.page == 0x000c && next.id == 0x00b5, "下一首 → Consumer Scan Next(0xB5)");
    const HidUsage hook = mediaKeyUsage(core::MediaKey::PhoneAccept);
    CHECK(hook.page == 0x000b && hook.id == 0x0020, "接听 → Telephony HookSwitch");

    // 按下报 usage，抬起报 0（HID 惯例）
    const auto down = hidReport(core::MediaKey::NextTrack, true);
    CHECK(down.size() == 4 && down[0] == 0x0c && down[1] == 0x00 && down[2] == 0xb5 &&
          down[3] == 0x00, "按下报表 = 页 0x000C / usage 0x00B5（小端）");
    const auto up = hidReport(core::MediaKey::NextTrack, false);
    CHECK(up.size() == 4 && up[2] == 0 && up[3] == 0, "抬起报表 usage = 0");
    const auto withId = hidReport(core::MediaKey::PlayPause, true, 3);
    CHECK(withId.size() == 5 && withId[0] == 3, "带报表 ID 时首字节是 ID");

    // 报文：CSM id = 0x4E02，参数 0 = 报表
    const auto frame = hidReportMessage(core::MediaKey::NextTrack, true);
    CsmFramer fr;
    fr.feed(frame.data(), frame.size());
    uint16_t id = 0;
    std::vector<uint8_t> body;
    CHECK(fr.next(&id, &body) && id == kAccessoryHidReport, "报文的 CSM id = 0x4E02");
    std::vector<Param> ps;
    CHECK(parseParams(body.data(), body.size(), &ps) && ps.size() == 1 && ps[0].id == 0 &&
          ps[0].payload == down, "参数 0 = 报表字节");

    // 队列：先进先出；超过容量丢最老的
    core::MediaKeyQueue q;
    q.push(core::MediaKey::Play, true);
    q.push(core::MediaKey::Play, false);
    core::MediaKeyEvent ev;
    CHECK(q.pop(&ev) && ev.key == core::MediaKey::Play && ev.down, "队列先进先出");
    CHECK(q.pop(&ev) && !ev.down, "队列出到抬起事件");
    CHECK(!q.pop(&ev), "空队列返回 false");
    const size_t extra = 3;
    for (size_t i = 0; i < core::MediaKeyQueue::kCapacity + extra; ++i)
        q.push(core::MediaKey::NextTrack, i % 2 == 0);
    CHECK(q.size() == core::MediaKeyQueue::kCapacity, "满了不再长");
    // 最老的三条被丢掉，队首是第 3 条（i=3 → down=false）
    CHECK(q.pop(&ev) && ev.key == core::MediaKey::NextTrack && !ev.down,
          "溢出丢最老的那条");
}

// ---------------------------------------------------------------------------
// ② 链路层握手 + 数据
// ---------------------------------------------------------------------------
void testLink()
{
    std::printf("== ② 链路层握手 ==\n");
    using namespace core::iap2;

    Pipe ab, ba;
    LinkEngine a, b;
    auto pump = [&](LinkEngine& e, Pipe& in, Pipe& out) {
        e.tick(nowMs());
        std::vector<uint8_t> bytes;
        if (e.takeOutput(&bytes)) out.push(bytes.data(), bytes.size());
        uint8_t buf[2048];
        int n = in.read(buf, sizeof buf, 20);
        if (n > 0) e.feed(buf, size_t(n), nowMs());
    };

    a.start(nowMs());
    b.start(nowMs());
    const uint64_t deadline = nowMs() + 3000;
    while (nowMs() < deadline && !(a.ready() && b.ready())) {
        pump(a, ba, ab);
        pump(b, ab, ba);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(a.ready() && b.ready(), "双端握手到 NORMAL（marker→SYN→ACK）");

    // a → b 一条控制载荷
    const std::vector<uint8_t> msg = {0x40, 0x40, 0x00, 0x0a, 0x12, 0x34, 1, 2, 3, 4};
    a.sendControl(msg, nowMs());
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        pump(a, ba, ab);
        pump(b, ab, ba);
        std::vector<uint8_t> ctrl;
        while (b.nextControl(&ctrl)) {
            if (ctrl == msg) got = true;
            ctrl.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got, "控制载荷 a→b 可靠送达");
}

// ---------------------------------------------------------------------------
// ③ MFi 签名（对证书公钥验签）
// ---------------------------------------------------------------------------
bool verifyRawSig(const std::vector<uint8_t>& certDer, const uint8_t digest[32],
                  const std::vector<uint8_t>& raw)
{
    if (raw.size() != 64) return false;
    const unsigned char* p = certDer.data();
    // p7b 里通常只有一张证书；用 d2i_PKCS7 提取
    PKCS7* pkcs7 = d2i_PKCS7(nullptr, &p, long(certDer.size()));
    if (!pkcs7) return false;
    STACK_OF(X509)* certs = pkcs7->d.sign ? pkcs7->d.sign->cert : nullptr;
    X509* cert = (certs && sk_X509_num(certs) > 0) ? sk_X509_value(certs, 0) : nullptr;
    bool ok = false;
    if (cert) {
        EVP_PKEY* pub = X509_get_pubkey(cert);
        if (pub) {
            ECDSA_SIG* sig = ECDSA_SIG_new();
            BIGNUM* r = BN_bin2bn(raw.data(), 32, nullptr);
            BIGNUM* s = BN_bin2bn(raw.data() + 32, 32, nullptr);
            ECDSA_SIG_set0(sig, r, s);
            unsigned char der[80];
            unsigned char* q = der;
            const int derLen = i2d_ECDSA_SIG(sig, &q);
            EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pub, nullptr);
            if (ctx && EVP_PKEY_verify_init(ctx) > 0)
                ok = EVP_PKEY_verify(ctx, der, size_t(derLen), digest, 32) == 1;
            if (ctx) EVP_PKEY_CTX_free(ctx);
            ECDSA_SIG_free(sig);
            EVP_PKEY_free(pub);
        }
    }
    PKCS7_free(pkcs7);
    return ok;
}

void testMfi(const std::string& dir)
{
    std::printf("== ③ MFi 签名 ==\n");
    std::string err;
    auto id = core::mfi::CertificateMfiIdentity::load(dir, &err);
    if (!id) {
        std::printf("   ⏭  跳过（%s）\n", err.c_str());
        return;
    }
    CHECK(!id->certificate().empty(), "证书已载入");
    uint8_t digest[32];
    for (int i = 0; i < 32; ++i) digest[i] = uint8_t(i * 7 + 1);
    bool ok = false;
    auto sig = id->signDigest(digest, 32, &ok);
    CHECK(ok && sig.size() == 64, "签名 = 64 字节 raw r||s");
    CHECK(verifyRawSig(id->certificate(), digest, sig), "对证书公钥验签通过");
    // 篡改摘要应验不过
    digest[0] ^= 0x01;
    CHECK(!verifyRawSig(id->certificate(), digest, sig), "错摘要验签失败（阴性对照）");

    // 同一份身份从内存加载 —— 鸿蒙走的就是这条路（HAP rawfile 读字节，读不到路径）
    {
        auto slurp = [](const std::string& p, std::vector<uint8_t>* out) {
            std::ifstream in(p, std::ios::binary);
            out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !out->empty();
        };
        std::vector<uint8_t> key, cert;
        if (slurp(dir + "/identity.pk8", &key) && slurp(dir + "/certificate.p7b", &cert)) {
            std::string err;
            auto id2 = core::mfi::CertificateMfiIdentity::loadFromMemory(
                key.data(), key.size(), cert.data(), cert.size(), &err);
            CHECK(id2 && id2->certificate() == id->certificate(),
                  "从内存加载身份（证书与目录加载一致）");
            if (id2) {
                digest[0] ^= 0x01;   // 还原成原摘要
                bool ok2 = false;
                auto sig2 = id2->signDigest(digest, 32, &ok2);
                CHECK(ok2 && verifyRawSig(id2->certificate(), digest, sig2),
                      "内存加载的身份也能签出可验签的名字");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ④ 全流程："假 iPhone" 对打
// ---------------------------------------------------------------------------
void testFlow(const std::string& dir)
{
    std::printf("== ④ 全流程（假 iPhone）==\n");
    using namespace core::iap2;

    Pipe toTrigger, toPhone;
    PipeStream triggerStream(&toTrigger, &toPhone);
    PipeStream phoneStream(&toPhone, &toTrigger);

    WirelessTriggerConfig cfg;
    cfg.btMac = "AA:BB:CC:DD:EE:01";
    cfg.ssid = "TestLAN";
    cfg.passphrase = "12345678";
    cfg.ipAddresses = {"192.168.10.11"};
    cfg.airPlayPort = 7100;
    cfg.deviceIdentifier = "02:AA:BB:CC:DD:EE";
    cfg.publicKeyHex = std::string(64, 'a');
    cfg.mfiDir = dir;
    cfg.timeoutMs = 15000;
    // 0x4301 发出后 run() 不会自己结束 —— 那是无线 CarPlay 的心跳线，得一直读
    // （见 wireless_client.cpp 里的 kSessionKeepAliveMs）。测试在拿齐证据后置位取消，
    // 否则 t.join() 会一直挂着。
    std::atomic<bool> cancel{false};
    cfg.cancel = &cancel;

    WirelessTrigger trigger(&triggerStream, cfg);
    std::atomic<bool> triggerDone{false}, triggerOk{false};
    std::thread t([&] {
        triggerOk.store(trigger.run());
        triggerDone.store(true);
    });

    // 假 iPhone：同款 LinkEngine + 手写应答
    LinkEngine phone;
    CsmFramer framer;
    phone.start(nowMs());
    bool seenId = false, seenCert = false, seenSig = false, seen5703 = false;
    bool sent1d00 = false;
    int  subs = 0;
    std::vector<uint8_t> sigBytes;
    bool sent4300 = false;

    auto phoneSend = [&](uint16_t msgId, const Params& body) {
        auto frame = csmFrame(msgId, body.encode());
        phone.sendControl(frame, nowMs());
    };

    const uint64_t deadline = nowMs() + 15000;
    while (nowMs() < deadline && !triggerDone.load()) {
        phone.tick(nowMs());
        // 链路就绪后，由"手机"发起识别（真实流程：iPhone 先发 0x1D00）
        if (phone.ready() && !sent1d00) {
            phoneSend(0x1d00, Params{});
            sent1d00 = true;
        }
        std::vector<uint8_t> out;
        if (phone.takeOutput(&out)) phoneStream.writeAll(out.data(), out.size());
        uint8_t buf[4096];
        int n = phoneStream.read(buf, sizeof buf, 20);
        if (n > 0) phone.feed(buf, size_t(n), nowMs());

        std::vector<uint8_t> ctrl;
        while (phone.nextControl(&ctrl)) {
            framer.feed(ctrl.data(), ctrl.size());
            uint16_t msgId = 0;
            std::vector<uint8_t> body;
            while (framer.next(&msgId, &body)) {
                std::vector<Param> params;
                parseParams(body.data(), body.size(), &params);
                switch (msgId) {
                case 0x1d01: {
                    seenId = true;
                    // 抽查识别报文关键字段
                    bool okName = false, okEap = false;
                    const Param* p0 = findParam(params, 0);
                    const Param* p10 = findParam(params, 10);
                    if (p0) okName = paramString(*p0, &okName) == cfg.name && okName;
                    if (p10) {
                        std::vector<Param> sub;
                        parseParams(p10->payload.data(), p10->payload.size(), &sub);
                        const Param* s1 = findParam(sub, 1);
                        bool ok2 = false;
                        if (s1) okEap = paramString(*s1, &ok2) == cfg.eaProtocol && ok2;
                    }
                    CHECK(okName, "假 iPhone 收到识别报文（name）");
                    CHECK(okEap, "识别报文 group10 EA 协议");
                    CHECK(findParam(params, 17) != nullptr, "识别报文 group17（蓝牙）");
                    CHECK(findParam(params, 24) != nullptr, "识别报文 group24（SSID）");
                    phoneSend(0x1d02, Params{});   // IdentificationAccepted
                    phoneSend(0xaa00, Params{});   // RequestCertificate
                    break;
                }
                case 0xaa01: {
                    seenCert = true;
                    phoneSend(0xaa02, [] { Params p; std::vector<uint8_t> ch(32, 0x5A); p.bytes(0, ch); return p; }());
                    break;
                }
                case 0xaa03: {
                    seenSig = true;
                    const Param* s = findParam(params, 0);
                    if (s) sigBytes = s->payload;
                    phoneSend(0xaa05, Params{});   // AuthenticationSucceeded
                    break;
                }
                case 0x5000: case 0x5200: case 0xae00: case 0x4157: case 0x4154: {
                    ++subs;
                    if (subs == 5) phoneSend(0x5702, Params{});   // RequestAccessoryWiFiConfiguration
                    break;
                }
                case 0x5703: {
                    seen5703 = true;
                    phoneSend(0x4e0e, Params{});                   // DeviceTransportIdentifier
                    if (!sent4300) { phoneSend(0x4300, Params{}); sent4300 = true; }
                    break;
                }
                default: break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cancel.store(true);   // 证据已收齐，让常驻的触发线程收工（见上面 cfg.cancel 注释）
    t.join();
    CHECK(triggerDone.load(), "触发线程结束");
    CHECK(triggerOk.load(), "触发返回成功（0x4301 已发出）");
    CHECK(seenId && seenCert && seenSig, "过程：识别→证书→签名");
    CHECK(subs == 5, "过程：5 条订阅");
    CHECK(seen5703, "过程：0x5703 已发");
    if (!trigger.error().empty()) std::printf("   （错误：%s）\n", trigger.error().c_str());

    // 假 iPhone 验签：用 identity 里的证书
    std::string err;
    auto id = core::mfi::CertificateMfiIdentity::load(dir, &err);
    if (id && sigBytes.size() == 64) {
        uint8_t digest[32];
        std::memset(digest, 0x5A, 32);
        CHECK(verifyRawSig(id->certificate(), digest, sigBytes), "0xAA03 签名可用证书验签");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "identity";
    testWire();
    testHid();
    testLink();
    testMfi(dir);
    testFlow(dir);
    std::printf("\n%s（失败 %d）\n", g_fail ? "==== 有失败 ====" : "==== 全部通过 ====", g_fail);
    return g_fail ? 1 : 0;
}
