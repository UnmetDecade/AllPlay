// ============================================================================
// tools/wired_selftest.cpp -- the part of the wired path that needs no phone, checked offline
// ============================================================================
// The wired path's failure mode is nasty: on a real device a hang becomes a timeout, and a timeout may be "the phone is ignoring me"
// or "my own code is wrong". So anything that can be **verified without plugging in a phone** gets verified here:
//
//   ① XML plist parsing -- the phone's reply carries an XML declaration and DOCTYPE, and the parser must swallow them
//   ② Certificate structure -- serial 0 / empty DN / extensions and critical bits / the device cert's public key
//   ③ TLS loopback handshake -- handshake against ourselves once to test the BIO pipe, read/write, and **whether the root cert
//                     can serve as the client identity** (hard to pin down if it fails on a real device)
//
// All of this is pure software and runs on the local machine/9.202: wired_probe --selftest
#include "usb/pair_record.h"
#include "usb/plist_xml.h"
#include "usb/tls_stream.h"

#include <openssl/bio.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_bad = 0;

void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "✓" : "✗", what.c_str());
    if (!ok) ++g_bad;
}

std::string opensslError()
{
    std::string out;
    unsigned long e = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256] = "";
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) out += " / ";
        out += buf;
    }
    return out.empty() ? "(no error code)" : out;
}

// ---------------------------------------------------------------- ① plist ----

void testPlist()
{
    std::printf("=== ① XML plist parsing ===\n");

    // This is what the phone's plist actually looked like in testing: XML declaration + DOCTYPE
    const std::string phoneReply =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>"
        "<key>Type</key><string>com.apple.mobile.lockdown</string>"
        "<key>Port</key><integer>49152</integer>"
        "<key>EnableSessionSSL</key><true/>"
        "<key>Off</key><false/>"
        "<key>Blob</key><data>AAECAwQ=</data>"
        "</dict></plist>";

    core::usb::Plist p;
    std::string     err;
    check(core::usb::Plist::parse(phoneReply, &p, &err), "parse a reply with a declaration (" + err + ")");
    if (!p.isDict()) {
        check(false, "top level is a dict");
        return;
    }
    check(p.stringValue("Type") == "com.apple.mobile.lockdown", "string field");
    check(p.intValue("Port") == 49152, "integer field");
    check(p.boolValue("EnableSessionSSL", false), "true field");
    check(!p.boolValue("Off", true), "false field");

    const core::usb::Plist* blob = p.find("Blob");
    const bool blobOk = blob != nullptr && blob->type() == core::usb::Plist::Type::Data &&
                        blob->asData() == std::vector<uint8_t>({0, 1, 2, 3, 4});
    check(blobOk, "data field (base64 decoded)");

    // Round-trip our own output: a request we generate must parse back (this pins framing, not content)
    core::usb::Plist req = core::usb::Plist::dict();
    req.set("Request", core::usb::Plist::string("Pair"));
    req.set("ProtocolVersion", core::usb::Plist::string("2"));
    core::usb::Plist back;
    check(core::usb::Plist::parse(req.toXml(), &back, &err), "our own XML parses back");
    check(back.stringValue("Request") == "Pair" && back.stringValue("ProtocolVersion") == "2",
          "round-trip values match");

    // The empty-DN cert goes into <data>; first confirm base64 encode/decode handles long data
    std::vector<uint8_t> big(1000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7 + 3);
    core::usb::Plist d = core::usb::Plist::dict();
    d.set("D", core::usb::Plist::data(big));
    core::usb::Plist d2;
    check(core::usb::Plist::parse(d.toXml(), &d2, &err), "1KB <data> round-trip");
    const core::usb::Plist* dv = d2.find("D");
    check(dv != nullptr && dv->asData() == big, "1KB <data> content matches");
}

// ------------------------------------------------------------- ② Certificates ----

X509* pemToX509(const std::string& pem)
{
    BIO*  b = BIO_new_mem_buf(pem.data(), int(pem.size()));
    X509* x = b != nullptr ? PEM_read_bio_X509(b, nullptr, nullptr, nullptr) : nullptr;
    if (b != nullptr) BIO_free(b);
    return x;
}

/// Fetch an extension's critical bit. Returns false = the extension is absent.
bool extCritical(X509* x, int nid, bool* critical)
{
    int crit = -1;
    void* v = X509_get_ext_d2i(x, nid, &crit, nullptr);
    if (v == nullptr) return false;
    OPENSSL_free(v);
    *critical = (crit == 1);
    return true;
}

/// Encode the public key as PKCS#1 RSAPublicKey DER (matching the format the phone gives)
std::vector<uint8_t> pkcs1PublicDer(EVP_PKEY* key)
{
    std::vector<uint8_t> der;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    OSSL_ENCODER_CTX* ectx = OSSL_ENCODER_CTX_new_for_pkey(key, EVP_PKEY_PUBLIC_KEY, "DER",
                                                           "type-specific", nullptr);
    if (ectx == nullptr) return der;
    unsigned char* out = nullptr;
    size_t         n = 0;
    if (OSSL_ENCODER_to_data(ectx, &out, &n) == 1 && out != nullptr) {
        der.assign(out, out + n);
        OPENSSL_free(out);
    }
    OSSL_ENCODER_CTX_free(ectx);
#endif
    return der;
}

void testCerts()
{
    std::printf("\n=== ② Pairing certificate structure ===\n");

    // Fake a "phone public key": just an ordinary RSA public key in PKCS#1 encoding
    EVP_PKEY* phoneKey = nullptr;
    {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(c);
        EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048);
        EVP_PKEY_keygen(c, &phoneKey);
        EVP_PKEY_CTX_free(c);
    }
    const std::vector<uint8_t> phonePkcs1 = pkcs1PublicDer(phoneKey);
    check(!phonePkcs1.empty(), "generate a 2048-bit test public key");

    // Observed on a real device: GetValue(DevicePublicKey) returns **PEM text**, not the documented PKCS#1 DER.
    // Both forms must be accepted -- we hit this regression once; don't wait for a real device to catch it again.
    std::string phonePem;
    {
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(b, phoneKey);
        char*      p = nullptr;
        const long n = BIO_get_mem_data(b, &p);
        if (p != nullptr && n > 0) phonePem.assign(p, size_t(n));
        BIO_free(b);
    }
    EVP_PKEY_free(phoneKey);
    check(!phonePem.empty(), "same public key also encodes as PEM text");

    core::usb::WiredPairIdentity id;
    std::string                 err;
    const int                   before = g_bad;
    check(core::usb::makeWiredPairIdentity(phonePkcs1, "", "", &id, &err),
          "generate root/host/device certificates (" + err + ")");
    if (g_bad != before) return;  // no certs were created, so further checks are pointless

    {
        core::usb::WiredPairIdentity id2;
        std::string                  err2;
        const std::vector<uint8_t>   pemBytes(phonePem.begin(), phonePem.end());
        check(core::usb::makeWiredPairIdentity(pemBytes, "", "", &id2, &err2),
              "phone public key in PEM form also works (" + err2 + ")");
        X509*     d2 = pemToX509(id2.deviceCertPem);
        EVP_PKEY* dp = d2 != nullptr ? X509_get0_pubkey(d2) : nullptr;
        check(dp != nullptr && pkcs1PublicDer(dp) == phonePkcs1,
              "PEM and PKCS#1 paths yield the same public key");
        if (d2 != nullptr) X509_free(d2);
    }

    check(id.rootCertPem.rfind("-----BEGIN CERTIFICATE-----", 0) == 0, "certificate is CERTIFICATE PEM");
    check(id.rootKeyPem.rfind("-----BEGIN PRIVATE KEY-----", 0) == 0, "private key is PKCS#8 PEM");

    std::string upper = id.hostId;
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    check(id.hostId.size() == 36 && id.hostId == upper && id.hostId[8] == '-' &&
              id.hostId[13] == '-' && id.hostId[18] == '-' && id.hostId[23] == '-',
          "HostID is a 36-character uppercase UUID");
    check(id.systemBuid != id.hostId && id.systemBuid.size() == 36, "SystemBUID is provided separately");

    X509* root = pemToX509(id.rootCertPem);
    X509* host = pemToX509(id.hostCertPem);
    X509* dev = pemToX509(id.deviceCertPem);
    check(root && host && dev, "all three certificates can be re-read");
    if (!root || !host || !dev) return;

    check(ASN1_INTEGER_get(X509_get_serialNumber(root)) == 0, "serial is 0");
    check(X509_NAME_entry_count(X509_get_subject_name(root)) == 0, "subject DN is empty");
    check(X509_NAME_entry_count(X509_get_issuer_name(root)) == 0, "issuer DN is empty");
    check(X509_get_version(root) == 2, "is a v3 certificate");

    // Validity span: exactly 10 years
    int days = 0, secs = 0;
    ASN1_TIME_diff(&days, &secs, X509_get0_notBefore(root), X509_get0_notAfter(root));
    check(days == 3650, "validity is exactly 3650 days (measured " + std::to_string(days) + ")");

    bool crit = false;
    check(extCritical(root, NID_basic_constraints, &crit) && crit, "root basicConstraints is critical");
    check(X509_check_ca(root) == 1, "root is a CA");

    check(extCritical(host, NID_basic_constraints, &crit) && crit,
          "host basicConstraints is critical");
    check(X509_check_ca(host) == 0, "host is not a CA");
    const uint32_t ku = X509_get_key_usage(host);
    check((ku & (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT)) ==
              (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT),
          "host keyUsage has digitalSignature + keyEncipherment");

    check(!extCritical(host, NID_subject_key_identifier, &crit), "host has **no** SKI");
    check(extCritical(dev, NID_subject_key_identifier, &crit) && !crit, "device has SKI and it is non-critical");
    check(X509_check_ca(dev) == 0, "device is not a CA");

    // the device cert's public key must be exactly the one the phone supplied
    EVP_PKEY* devPub = X509_get0_pubkey(dev);
    check(devPub != nullptr && pkcs1PublicDer(devPub) == phonePkcs1,
          "public key in the device certificate = the one the phone supplied");

    X509_free(root);
    X509_free(host);
    X509_free(dev);
}

// ------------------------------------------------------------- ③ TLS ----

/// Wrap an fd as an IByteStream so startTlsClient has something to layer over
class FdStream final : public core::IByteStream {
public:
    explicit FdStream(int fd) : fd_(fd) {}
    ~FdStream() override { close(); }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeoutMs);
        if (r == 0) return 0;   // timeout
        if (r < 0) return -1;
        const ssize_t k = ::recv(fd_, buf, n, 0);
        return k < 0 ? -1 : int(k);
    }
    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0) return false;
        size_t off = 0;
        while (off < n) {
            const ssize_t k = ::send(fd_, buf + off, n - off, MSG_NOSIGNAL);
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }
    void close() override
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_;
};

/// Fake phone: accept one TLS connection using **the same certs** and reply once per message received
void fakePhone(int fd, const std::string& certPem, const std::string& keyPem, bool* ok)
{
    *ok = false;
    // If the client never shows up (e.g. its certs failed to load), don't let this thread hang join forever
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    BIO* cb = BIO_new_mem_buf(certPem.data(), int(certPem.size()));
    X509* x = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
    BIO_free(cb);
    BIO* kb = BIO_new_mem_buf(keyPem.data(), int(keyPem.size()));
    EVP_PKEY* k = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
    BIO_free(kb);
    if (x == nullptr || k == nullptr || SSL_CTX_use_certificate(ctx, x) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, k) != 1) {
        std::printf("    (fake phone failed to install certificate: %s)\n", opensslError().c_str());
        goto done;
    }

    {
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1) {
            char buf[64] = "";
            const int n = SSL_read(ssl, buf, sizeof buf);
            if (n > 0) {
                if (SSL_write(ssl, "pong", 4) == 4) *ok = true;
                std::printf("    (fake phone received %d bytes: %.*s)\n", n, n, buf);
            }
        } else {
            std::printf("    (fake phone handshake failed: %s)\n", opensslError().c_str());
        }
        SSL_free(ssl);
    }

done:
    if (x != nullptr) X509_free(x);
    if (k != nullptr) EVP_PKEY_free(k);
    SSL_CTX_free(ctx);
}

void testTls()
{
    std::printf("\n=== ③ TLS loopback handshake ===\n");

    core::usb::WiredPairIdentity id;
    std::string                 err;
    EVP_PKEY* pk = nullptr;
    {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(c);
        EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048);
        EVP_PKEY_keygen(c, &pk);
        EVP_PKEY_CTX_free(c);
    }
    const std::vector<uint8_t> phonePkcs1 = pkcs1PublicDer(pk);
    EVP_PKEY_free(pk);
    if (!core::usb::makeWiredPairIdentity(phonePkcs1, "", "", &id, &err)) {
        check(false, "make certificate (" + err + ")");
        return;
    }

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        check(false, "socketpair failed");
        return;
    }

    bool serverOk = false;
    // Use **root** as the client identity -- matching the real device; getting this backwards only shows up on real hardware
    std::thread server(fakePhone, fds[1], id.rootCertPem, id.rootKeyPem, &serverOk);

    FdStream raw(fds[0]);
    auto     tls = core::usb::startTlsClient(raw, id.rootCertPem, id.rootKeyPem, &err);
    check(tls != nullptr, "client handshake completed (" + err + ")");

    if (tls != nullptr) {
        const char* ping = "ping";
        check(tls->writeAll(reinterpret_cast<const uint8_t*>(ping), 4), "write through TLS");

        char   buf[64] = "";
        size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (got < 4 && std::chrono::steady_clock::now() < deadline) {
            const int n = tls->read(reinterpret_cast<uint8_t*>(buf) + got, sizeof(buf) - got, 500);
            if (n < 0) break;
            if (n > 0) got += size_t(n);
        }
        check(got == 4 && std::memcmp(buf, "pong", 4) == 0, "read back through TLS");
    }

    server.join();  // the fake phone either finishes the handshake and exits, or exits on timeout after the client closes
    check(serverOk, "server side also considers the handshake successful");
}

}  // namespace

/// Entry point for wired_probe --selftest
int wiredSelfTest()
{
    std::printf("=== Wired link offline self-test (no phone needed) ===\n\n");
    testPlist();
    testCerts();
    testTls();
    std::printf("\n%s (%d checks failed)\n", g_bad == 0 ? "ALL PASSED" : "FAILURES", g_bad);
    return g_bad == 0 ? 0 : 1;
}
