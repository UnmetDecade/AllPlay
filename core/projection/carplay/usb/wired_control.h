/**
 * @file   wired_control.h
 * @brief  Wired control plane: pair → session → TLS → start the carkit service
 * @author blt
 */

// ============================================================================
// core/usb/wired_control.h — wired CarPlay control plane
// ============================================================================
// Chains usbmux / lockdown / tls_stream / pair_record together. On the phone side we open
// **three** mini TCP connections in total; missing any one means no final iAP2 stream:
//
//   1. :62078  plaintext lockdown — SetValue(UntrustedHostBUID) → GetValue(DevicePublicKey)
//              → GetValue(WiFiAddress) → Pair. Closed once pairing is done.
//   2. :62078  StartSession → **the same connection upgrades to TLS in place** → StartService(carkit)
//              This one **must stay open**; if it drops, the third is dead too.
//   3. The port StartService returns — a new connection, optionally TLS again. **This is the iAP2 stream**.
//
// The TLS client identity is the **root certificate**, not the host one — an easy mix-up (see pair_record.h).
#pragma once

#include "port/stream.h"
#include "usb/mux.h"
#include "usb/pair_record.h"

#include <memory>
#include <string>

namespace core::usb {

class UsbMux;

class WiredControl {
public:
    /// pipe = USB bulk pipe supplied by the platform layer (see platform/posix/usb_libusb.cpp).
    /// **Must outlive this object**. The mux is created internally (UsbMux); this is the Linux path.
    explicit WiredControl(IByteStream& pipe);

    /// Use a ready-made mux (macOS: system usbmuxd tunnel, see platform/posix/usbmuxd_mac.cpp).
    /// On this path there is no "our USB pipe"; the mux is owned by this object.
    explicit WiredControl(std::unique_ptr<IMux> mux);

    ~WiredControl();

    WiredControl(const WiredControl&) = delete;
    WiredControl& operator=(const WiredControl&) = delete;

    /// Let pairing material persist for reuse (**without this, old behavior: generate a new set each time, trust prompt each time**).
    /// @param key     Key identifying the phone — pass the USB serial number (pair_record.h explains why not
    ///                 DevicePublicKey). Empty string = no persistence.
    /// @param stateDir Empty = defaultStateDir() ($ALLPLAY_STATE_DIR or ~/.local/state/allplay).
    ///                 **Beware of sudo**: it changes HOME, so when running as root this value must be passed explicitly.
    void setPairStore(std::string key, std::string stateDir = {});

    /// Run the whole control plane. On success service() becomes usable.
    bool connect(std::string* err);

    /// The iAP2 stream. **The first byte is already an iAP2 link frame**; no extra handshake, no encapsulation.
    IByteStream* service() const { return service_.get(); }

    /// HostID / BUID used for this pairing (needed when persisting the pair record)
    const std::string& hostId() const { return identity_.hostId; }
    const std::string& systemBuid() const { return identity_.systemBuid; }

private:
    bool doPair(std::string* err);
    bool doSessionAndService(std::string* err);
    /// If disk has a "paired successfully" record, load it into identity_. **No packets are sent**.
    bool loadTrustedRecord();
    /// Tear down the session streams in **destruction order** (the two TLS streams reference their raws; order matters).
    void resetSessionStreams();

    IByteStream*                 pipe_ = nullptr;
    std::unique_ptr<IMux>        mux_;
    std::unique_ptr<IByteStream> pairCh_;      ///< connection 1, discarded after pairing
    std::unique_ptr<IByteStream> sessionRaw_;  ///< connection 2 (underlying before TLS; the TLS object references it)
    std::unique_ptr<IByteStream> sessionCh_;   ///< connection 2 as seen externally (may be TLS)
    std::unique_ptr<IByteStream> serviceRaw_;  ///< underlying of connection 3
    std::unique_ptr<IByteStream> service_;     ///< connection 3 as seen externally
    WiredPairIdentity            identity_;
    std::string                  pairKey_;     ///< pairing material key (phone serial); empty = no persistence
    std::string                  pairStateDir_;///< empty = defaultStateDir()
};

}  // namespace core::usb
