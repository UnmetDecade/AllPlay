// ============================================================================
// tools/iap2_test.cpp -- iAP2/MFi self-test (the most that can be verified without hardware)
// ============================================================================
// Covers:
//   ① Wire format: param encode/decode round trip, string NUL, CSM deframing (including leading junk / fragmentation)
//   ①b Steering-wheel controls: media key -> HID usage/report -> 0x4E02 message, key queue push/pop and overflow
//   ② Link layer: two LinkEngines handshake over an in-memory pipe (marker->SYN->ACK->NORMAL),
//      control payloads delivered reliably both ways, sequence/checksum paths correct
//   ③ MFi signing: load identity/ + sign a 32-byte digest + verify with the cert's public key
//   ④ Full flow: WirelessTrigger <-> "fake iPhone" (a peer built on the same engine),
//      running identification -> MFi -> subscribe -> 0x5703 -> 0x4301
//
// Usage: iap2_test [identity-dir]   (default <project root>/identity)
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
// Thread-safe in-memory pipe (both ends feed bytes to each other)
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
            return 0;   // timeout
        }
        if (buf_.empty()) return -1;   // closed
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
// ① Wire format
// ---------------------------------------------------------------------------
void testWire()
{
    std::printf("== ① Wire format ==\n");
    using namespace core::iap2;

    Params p;
    p.u8(1, 7).u16(2, 0x1234).u32(3, 0xDEADBEEF).str(4, "hello").u16List(5, {1, 2, 3}).voidv(6);
    Params g;
    g.u8(0, 1).str(1, "abc");
    p.group(7, g);
    auto enc = p.encode();
    std::vector<Param> back;
    CHECK(parseParams(enc.data(), enc.size(), &back), "parameter encode/decode round-trip");
    bool ok = false;
    CHECK(back.size() == 7, "parameter count is 7");
    CHECK(back[3].payload.size() == 6 && back[3].payload.back() == 0 &&
          paramString(back[3], &ok) == "hello" && ok, "string carries NUL");
    CHECK(back[2].payload.size() == 4 && paramU32(back[2]) == 0xDEADBEEF, "u32 big-endian");
    CHECK(back[4].payload.size() == 6 && back[4].payload[5] == 3 && back[4].payload[1] == 1, "u16 list");
    std::vector<Param> gsub;
    CHECK(parseParams(back[6].payload.data(), back[6].payload.size(), &gsub) &&
          gsub.size() == 2 && paramString(gsub[1]) == "abc", "nested group");

    auto frame = csmFrame(0x4301, enc);
    CsmFramer fr;
    std::vector<uint8_t> junk = {0x00, 0xff, 0x41};   // doesn't end with 0x40 (otherwise it joins the frame header into a fake 4040)
    fr.feed(junk.data(), junk.size());              // junk before the frame (for alignment)
    fr.feed(frame.data(), 3);                       // then feed in fragments
    uint16_t id = 0;
    std::vector<uint8_t> body;
    CHECK(!fr.next(&id, &body), "no frame emitted while fragmented");
    fr.feed(frame.data() + 3, frame.size() - 3);
    CHECK(fr.next(&id, &body) && id == 0x4301 && body == enc, "CSM deframing (tolerates leading junk / fragmentation)");
}

// ---------------------------------------------------------------------------
// ①b Steering-wheel encoding (media key -> HID report -> iAP2 message)
// ---------------------------------------------------------------------------
void testHid()
{
    std::printf("== ①b Steering-wheel encoding ==\n");
    using namespace core::iap2;

    const HidUsage next = mediaKeyUsage(core::MediaKey::NextTrack);
    CHECK(next.page == 0x000c && next.id == 0x00b5, "next track → Consumer Scan Next(0xB5)");
    const HidUsage hook = mediaKeyUsage(core::MediaKey::PhoneAccept);
    CHECK(hook.page == 0x000b && hook.id == 0x0020, "answer → Telephony HookSwitch");

    // report the usage on press, 0 on release (HID convention)
    const auto down = hidReport(core::MediaKey::NextTrack, true);
    CHECK(down.size() == 4 && down[0] == 0x0c && down[1] == 0x00 && down[2] == 0xb5 &&
          down[3] == 0x00, "press report = page 0x000C / usage 0x00B5 (little-endian)");
    const auto up = hidReport(core::MediaKey::NextTrack, false);
    CHECK(up.size() == 4 && up[2] == 0 && up[3] == 0, "release report usage = 0");
    const auto withId = hidReport(core::MediaKey::PlayPause, true, 3);
    CHECK(withId.size() == 5 && withId[0] == 3, "first byte is the report ID when an ID is present");

    // message: CSM id = 0x4E02, param 0 = report
    const auto frame = hidReportMessage(core::MediaKey::NextTrack, true);
    CsmFramer fr;
    fr.feed(frame.data(), frame.size());
    uint16_t id = 0;
    std::vector<uint8_t> body;
    CHECK(fr.next(&id, &body) && id == kAccessoryHidReport, "message CSM id = 0x4E02");
    std::vector<Param> ps;
    CHECK(parseParams(body.data(), body.size(), &ps) && ps.size() == 1 && ps[0].id == 0 &&
          ps[0].payload == down, "param 0 = report bytes");

    // Queue: FIFO; when over capacity, drop the oldest
    core::MediaKeyQueue q;
    q.push(core::MediaKey::Play, true);
    q.push(core::MediaKey::Play, false);
    core::MediaKeyEvent ev;
    CHECK(q.pop(&ev) && ev.key == core::MediaKey::Play && ev.down, "queue is FIFO");
    CHECK(q.pop(&ev) && !ev.down, "queue yields the release event");
    CHECK(!q.pop(&ev), "empty queue returns false");
    const size_t extra = 3;
    for (size_t i = 0; i < core::MediaKeyQueue::kCapacity + extra; ++i)
        q.push(core::MediaKey::NextTrack, i % 2 == 0);
    CHECK(q.size() == core::MediaKeyQueue::kCapacity, "does not grow past capacity");
    // the three oldest are dropped; the head is the 3rd (i=3 -> down=false)
    CHECK(q.pop(&ev) && ev.key == core::MediaKey::NextTrack && !ev.down,
          "overflow drops the oldest entry");
}

// ---------------------------------------------------------------------------
// ② Link-layer handshake + data
// ---------------------------------------------------------------------------
void testLink()
{
    std::printf("== ② Link-layer handshake ==\n");
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
    CHECK(a.ready() && b.ready(), "both sides handshake to NORMAL (marker→SYN→ACK)");

    // one control payload a -> b
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
    CHECK(got, "control payload a→b delivered reliably");
}

// ---------------------------------------------------------------------------
// ③ MFi signing (verify against the cert public key)
// ---------------------------------------------------------------------------
bool verifyRawSig(const std::vector<uint8_t>& certDer, const uint8_t digest[32],
                  const std::vector<uint8_t>& raw)
{
    if (raw.size() != 64) return false;
    const unsigned char* p = certDer.data();
    // a p7b usually contains just one cert; extract it with d2i_PKCS7
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
    std::printf("== ③ MFi signing ==\n");
    std::string err;
    auto id = core::mfi::CertificateMfiIdentity::load(dir, &err);
    if (!id) {
        std::printf("   ⏭  skipped (%s)\n", err.c_str());
        return;
    }
    CHECK(!id->certificate().empty(), "certificate loaded");
    uint8_t digest[32];
    for (int i = 0; i < 32; ++i) digest[i] = uint8_t(i * 7 + 1);
    bool ok = false;
    auto sig = id->signDigest(digest, 32, &ok);
    CHECK(ok && sig.size() == 64, "signature = 64-byte raw r||s");
    CHECK(verifyRawSig(id->certificate(), digest, sig), "verifies against the certificate public key");
    // a tampered digest should fail verification
    digest[0] ^= 0x01;
    CHECK(!verifyRawSig(id->certificate(), digest, sig), "wrong digest fails verification (negative control)");

    // Load the same identity from memory -- this is the path HarmonyOS takes (HAP rawfile reads bytes, no path available)
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
                  "load identity from memory (certificate matches directory load)");
            if (id2) {
                digest[0] ^= 0x01;   // restore the original digest
                bool ok2 = false;
                auto sig2 = id2->signDigest(digest, 32, &ok2);
                CHECK(ok2 && verifyRawSig(id2->certificate(), digest, sig2),
                      "memory-loaded identity also produces verifiable signatures");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ④ Full flow: sparring with a "fake iPhone"
// ---------------------------------------------------------------------------
void testFlow(const std::string& dir)
{
    std::printf("== ④ Full flow (fake iPhone) ==\n");
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
    // After 0x4301 is sent, run() doesn't end on its own -- that's the wireless CarPlay heartbeat, which must keep reading
    // (see kSessionKeepAliveMs in wireless_client.cpp). The test sets cancel once it has gathered the evidence,
    // otherwise t.join() would hang forever.
    std::atomic<bool> cancel{false};
    cfg.cancel = &cancel;

    WirelessTrigger trigger(&triggerStream, cfg);
    std::atomic<bool> triggerDone{false}, triggerOk{false};
    std::thread t([&] {
        triggerOk.store(trigger.run());
        triggerDone.store(true);
    });

    // Fake iPhone: same LinkEngine + hand-written responses
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
        // Once the link is ready, let the "phone" initiate identification (real flow: the iPhone sends 0x1D00 first)
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
                    // spot-check key fields of the identification message
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
                    CHECK(okName, "fake iPhone received identification message (name)");
                    CHECK(okEap, "identification message group10 EA protocol");
                    CHECK(findParam(params, 17) != nullptr, "identification message group17 (Bluetooth)");
                    CHECK(findParam(params, 24) != nullptr, "identification message group24 (SSID)");
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

    cancel.store(true);   // evidence gathered; let the resident trigger thread wind down (see the cfg.cancel comment above)
    t.join();
    CHECK(triggerDone.load(), "trigger thread finished");
    CHECK(triggerOk.load(), "trigger returned success (0x4301 sent)");
    CHECK(seenId && seenCert && seenSig, "flow: identification→certificate→signature");
    CHECK(subs == 5, "flow: 5 subscriptions");
    CHECK(seen5703, "flow: 0x5703 sent");
    if (!trigger.error().empty()) std::printf("   (error: %s)\n", trigger.error().c_str());

    // Fake iPhone signature check: use the cert from identity
    std::string err;
    auto id = core::mfi::CertificateMfiIdentity::load(dir, &err);
    if (id && sigBytes.size() == 64) {
        uint8_t digest[32];
        std::memset(digest, 0x5A, 32);
        CHECK(verifyRawSig(id->certificate(), digest, sigBytes), "0xAA03 signature verifies with the certificate");
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
    std::printf("\n%s (%d failures)\n", g_fail ? "==== FAILURES ====" : "==== ALL PASSED ====", g_fail);
    return g_fail ? 1 : 0;
}
