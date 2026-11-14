/**
 * @file   wired_control.cpp
 * @brief  Implementation of the wired control plane
 * @author blt
 */

#include "usb/wired_control.h"

#include "common/log.h"
#include "usb/lockdown.h"
#include "usb/tls_stream.h"
#include "usb/usbmux.h"

#include <chrono>
#include <thread>

namespace core::usb {
namespace {

constexpr const char* kTag = "wired";
constexpr uint16_t    kLockdownPort = 62078;
constexpr int         kConnectTimeoutMs = 10000;
constexpr int         kPairTimeoutMs = 15000;  ///< wait per Pair attempt; real waiting is done by resend polling
/// How long to wait for the "Trust this computer" prompt (a human has to unlock the phone, find the prompt, tap it)
constexpr int         kTrustWaitSec = 180;
constexpr int         kSessionTimeoutMs = 15000;
constexpr const char* kCarkitService = "com.apple.carkit.service";

/// Label used when sending requests. The phone merely echoes it; any value works, but keep it identical to DiPlay to ease log comparison.
constexpr const char* kLabel = "allplay.wired";

/// Build the nested PairRecord in the Pair request.
/// **NOTE: all three certificates are <data> containing PEM text bytes, not DER** — counter-intuitive,
/// and once naively written as DER. Also **there is no RootPrivateKey entry**, the private key is never sent to the phone.
Plist makePairRecord(const WiredPairIdentity& id)
{
    auto pem = [](const std::string& s) {
        return Plist::data(std::vector<uint8_t>(s.begin(), s.end()));
    };

    Plist rec = Plist::dict();
    rec.set("DeviceCertificate", pem(id.deviceCertPem));
    rec.set("HostCertificate", pem(id.hostCertPem));
    rec.set("RootCertificate", pem(id.rootCertPem));
    rec.set("HostID", Plist::string(id.hostId));
    rec.set("SystemBUID", Plist::string(id.systemBuid));
    return rec;
}

}  // namespace

WiredControl::WiredControl(IByteStream& pipe) : pipe_(&pipe) {}
WiredControl::WiredControl(std::unique_ptr<IMux> mux) : mux_(std::move(mux)) {}

WiredControl::~WiredControl() = default;  // members destruct in reverse order: service streams first, mux last

bool WiredControl::connect(std::string* err)
{
    if (!mux_) {
        if (pipe_ == nullptr) {
            if (err != nullptr) *err = "No usable mux (neither a USB pipe nor an existing mux was provided)";
            return false;
        }
        mux_ = std::make_unique<UsbMux>(*pipe_);
    }
    if (!mux_->begin(err)) return false;

    // ① This material **paired successfully** before → open the session directly, **send no Pair at all**.
    //
    // Why this is the key to "no trust tap": reusing the same HostID/certificates is only a necessary condition. The phone's
    // "I trust this computer" is expressed by **connections after a successful pairing skipping Pair and going straight to TLS with the host certificate**.
    // Sending Pair again every time is treated as a new pairing and the prompt reappears — measured: with the same
    // material, Pair at 12:53 succeeded, another Pair at 12:54 prompted again.
    if (loadTrustedRecord()) {
        std::string serr;
        if (doSessionAndService(&serr)) {
            LOG_INFO("[%s] Opened session directly with the stored pairing record (skipped Pair)", kTag);
            return true;
        }
        LOG_WARN("[%s] Opening the session directly without Pair failed (%s) - falling back to full pairing", kTag, serr.c_str());
        resetSessionStreams();
        identity_ = WiredPairIdentity{};   // on fallback, let doPair prepare the material from scratch
    }

    // ② Full pairing (when the phone has not accepted this machine, this step shows "Trust this computer?")
    if (!doPair(err)) return false;
    return doSessionAndService(err);
}

bool WiredControl::loadTrustedRecord()
{
    if (pairKey_.empty()) return false;
    if (!isWiredPairTrusted(pairStateDir_, pairKey_)) return false;

    WiredPairIdentity id;
    bool               found = false;
    std::string        lerr;
    if (!loadWiredPairCerts(pairStateDir_, pairKey_, &id, &found, &lerr) || !found) return false;
    if (!loadWiredPairIds(pairStateDir_, pairKey_, &id.hostId, &id.systemBuid)) return false;

    identity_ = std::move(id);
    return true;
}

void WiredControl::resetSessionStreams()
{
    // Order matters: service_ / sessionCh_ are TLS streams referencing their raws.
    service_.reset();
    serviceRaw_.reset();
    sessionCh_.reset();
    sessionRaw_.reset();
}

void WiredControl::setPairStore(std::string key, std::string stateDir)
{
    pairKey_      = std::move(key);
    pairStateDir_ = std::move(stateDir);
}

bool WiredControl::doPair(std::string* err)
{
    // These two UUIDs must be fixed before the flow starts: the BUID reported by SetValue and the one written in Pair
    // must be the same, otherwise the phone treats us as another "host" and pairing is as good as none.
    //
    // **Prefer reusing the previous ones**: changing HostID invalidates the pair record stored on the phone,
    // so "Trust this computer?" appears again. DevicePublicKey is not available yet (SetValue
    // must come before GetValue), so the key is the phone serial number, not DevicePublicKey.
    if (!pairKey_.empty()) {
        if (loadWiredPairIds(pairStateDir_, pairKey_, &identity_.hostId, &identity_.systemBuid)) {
            LOG_INFO("[%s] Reusing previous pairing identity HostID=%s (the phone recognizes it; no trust prompt expected)", kTag,
                     identity_.hostId.c_str());
        }
    }
    if (identity_.hostId.empty()) identity_.hostId = makeUuidUpper();
    if (identity_.systemBuid.empty()) identity_.systemBuid = makeUuidUpper();
    // From this point these two values are "the pair already reported to the phone"; nothing later may change them.
    const std::string hostId     = identity_.hostId;
    const std::string systemBuid = identity_.systemBuid;

    pairCh_ = mux_->connectTcp(kLockdownPort, kConnectTimeoutMs, err);
    if (pairCh_ == nullptr) return false;
    LockdownClient lk(*pairCh_);

    // 1) Introduce ourselves first. Without this the phone likely won't hand out DevicePublicKey.
    if (!lk.setValue("UntrustedHostBUID", Plist::string(identity_.systemBuid), err)) return false;

    // 2) The phone's public key — used to sign a certificate "for it"
    Plist devKey;
    bool  found = false;
    if (!lk.getValue("DevicePublicKey", &devKey, &found, err)) return false;
    if (!found) {
        if (err != nullptr) *err = "Phone did not provide DevicePublicKey (not unlocked? this computer not trusted?)";
        return false;
    }
    if (devKey.type() != Plist::Type::Data) {
        if (err != nullptr) *err = "DevicePublicKey is not <data>";
        return false;
    }
    // Leave the encoding to pair_record — this phone gives **PEM text**, not the documented
    // PKCS#1 DER; don't conclude otherwise in this log line
    const std::vector<uint8_t>& devKeyBytes = devKey.asData();
    LOG_INFO("[%s] Got phone public key: %zu bytes", kTag, devKeyBytes.size());

    // 3) WiFiAddress — unused on wired. Order copied from DiPlay; failing to read is not an error.
    Plist wifi;
    bool  hasWifi = false;
    if (!lk.getValue("WiFiAddress", &wifi, &hasWifi, err)) return false;
    if (hasWifi) LOG_DEBUG("[%s] Phone WiFiAddress: %s", kTag, wifi.asString().c_str());

    // 4) Three certificates: **use stored ones if available** (the phone accepts this exact set; changing it is like a new computer, prompt again),
    //    if absent / for another phone, generate a new set and save it. The device cert's public key is the one obtained above.
    if (!pairKey_.empty()) {
        bool found = false;
        std::string lerr;
        if (!loadWiredPairIdentity(pairStateDir_, pairKey_, devKeyBytes, &identity_, &found,
                                   &lerr)) {
            if (err != nullptr) *err = lerr;   // only reached when "readable but corrupt"
            return false;
        }
        if (found) {
            // The certificates came from storage; **the two UUIDs follow the ones already reported to the phone** —
            // SetValue(UntrustedHostBUID) above sent identity_.systemBuid, so writing a different one
            // in Pair means "another host" and pairing is as good as none.
            // (Normally both come from the same source; this just removes any chance of divergence.)
            identity_.hostId     = hostId;
            identity_.systemBuid = systemBuid;
            LOG_INFO("[%s] Reusing the stored certificates (one each for root/host/device)", kTag);
        }
    }
    if (identity_.rootCertPem.empty()) {
        if (!makeWiredPairIdentity(devKeyBytes, identity_.hostId, identity_.systemBuid, &identity_,
                                   err)) {
            return false;
        }
        if (!pairKey_.empty()) {
            std::string serr;
            // Failing to save is not a failure: at worst one more trust prompt; don't abort the whole pairing for it.
            if (!saveWiredPairIdentity(pairStateDir_, pairKey_, devKeyBytes, identity_, &serr))
                LOG_WARN("[%s] Failed to save pairing material (%s) - this phone will need one more trust tap next time", kTag,
                         serr.c_str());
        }
    }

    // 5) Pair
    Plist req = Plist::dict();
    req.set("Label", Plist::string(kLabel));
    req.set("Request", Plist::string("Pair"));
    // ProtocolVersion goes at the **outer level** (as DiPlay does, not inside PairRecord).
    // If the phone objects, its error will mention it; that is the first line to change then.
    req.set("ProtocolVersion", Plist::string("2"));
    Plist opts = Plist::dict();
    opts.set("ExtendedPairingErrors", Plist::boolean(true));  // gives a readable reason on failure
    req.set("PairingOptions", std::move(opts));
    req.set("PairRecord", makePairRecord(identity_));

    // The first time the phone sees this "computer" it shows "Trust this computer?"; wait for the tap.
    // During this time it replies Error=PairingDialogResponsePending each time — **not a failure, but "keep waiting"**;
    // just resend the same Pair request (UUIDs and certificates must not change, else it is a new computer and the prompt restarts).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTrustWaitSec);
    int  waited = 0;
    for (;;) {
        std::string perr;
        if (lk.request(req, nullptr, kPairTimeoutMs, &perr)) break;

        if (perr.find("PairingDialogResponsePending") != std::string::npos &&
            std::chrono::steady_clock::now() < deadline) {
            if (waited == 0) {
                LOG_WARN("[%s] The phone is showing \"Trust This Computer?\" - unlock it and tap \"Trust\". "
                         "Waiting up to %d seconds here (the request is resent every second; the phone still sees the same dialog)",
                         kTag, kTrustWaitSec);
            } else if (waited % 15 == 0) {
                LOG_INFO("[%s] Still waiting for confirmation... (%d seconds elapsed)", kTag, waited);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++waited;
            continue;
        }
        if (waited > 0) {
            LOG_WARN("[%s] Gave up after waiting %d seconds without confirmation", kTag, waited);
        }
        if (err != nullptr) *err = perr;
        return false;
    }
    LOG_INFO("[%s] Pairing succeeded (HostID=%s)", kTag, identity_.hostId.c_str());

    // Stamp: only records from **a genuinely successful pairing** may skip Pair next round (see pair_record.h).
    if (!pairKey_.empty()) {
        std::string merr;
        if (markWiredPairTrusted(pairStateDir_, pairKey_, &merr))
            LOG_INFO("[%s] Recorded that this phone accepts this material - next time the session opens directly with no prompt", kTag);
        else
            LOG_WARN("[%s] Failed to record \"paired\" (%s) - this phone will prompt once more next time", kTag,
                     merr.c_str());
    }

    pairCh_.reset();  // connection 1 is done; close it instead of keeping it open
    return true;
}

bool WiredControl::doSessionAndService(std::string* err)
{
    sessionRaw_ = mux_->connectTcp(kLockdownPort, kConnectTimeoutMs, err);
    if (sessionRaw_ == nullptr) return false;

    // The StartSession request has **no EnableSessionSSL** — the phone decides encryption in its response
    bool enableSsl = false;
    {
        LockdownClient lk(*sessionRaw_);
        Plist         req = Plist::dict();
        req.set("Label", Plist::string(kLabel));
        req.set("Request", Plist::string("StartSession"));
        req.set("HostID", Plist::string(identity_.hostId));
        req.set("SystemBUID", Plist::string(identity_.systemBuid));

        Plist resp;
        if (!lk.request(req, &resp, kSessionTimeoutMs, err)) return false;
        enableSsl = resp.boolValue("EnableSessionSSL", false);
    }

    // Upgrade in place: start the TLS handshake on the same connection. Client identity is the **root** cert + key.
    if (enableSsl) {
        LOG_INFO("[%s] Session requires TLS; upgrading in place", kTag);
        sessionCh_ = startTlsClient(*sessionRaw_, identity_.rootCertPem, identity_.rootKeyPem, err);
        if (sessionCh_ == nullptr) return false;
    } else {
        // The phone did not ask for encryption. Follow its lead — forcing TLS would only kill this connection.
        LOG_WARN("[%s] Phone did not request SessionSSL; continuing in plaintext (if it fails later, suspect this first)", kTag);
        sessionCh_ = std::move(sessionRaw_);
    }

    // Start the carkit service on this (possibly encrypted) session
    uint16_t port = 0;
    bool     svcSsl = false;
    {
        LockdownClient lk(*sessionCh_);
        if (!lk.startService(kCarkitService, &port, &svcSsl, err)) return false;
    }

    // Connection 3: open a new connection to that port. It carries iAP2.
    serviceRaw_ = mux_->connectTcp(port, kConnectTimeoutMs, err);
    if (serviceRaw_ == nullptr) return false;

    if (svcSsl) {
        service_ = startTlsClient(*serviceRaw_, identity_.rootCertPem, identity_.rootKeyPem, err);
        if (service_ == nullptr) return false;
    } else {
        service_ = std::move(serviceRaw_);
    }

    LOG_INFO("[%s] Got the carkit service stream (port %u, service-side TLS: %s); iAP2 can start", kTag,
             unsigned(port), svcSsl ? "yes" : "no");
    return true;
}

}  // namespace core::usb
