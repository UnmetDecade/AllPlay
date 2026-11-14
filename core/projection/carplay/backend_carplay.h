/**
 * @file   backend_carplay.h
 * @brief  CarPlay protocol backend: wraps wireless (Receiver + bt_trigger) and wired (usbmux/iAP2)
 *         into core::projection::IProjectionBackend
 * @author blt
 */

// ============================================================================
// core/projection/carplay/backend_carplay.h - CarPlay backend
// ============================================================================
// This is the first implementation of core/projection/backend.h:
//   · probe()   -- wired: attached iPhones (platform-injected probePhones); wireless: already-paired phones;
//   · connect() -- choose a wired / wireless session based on target.transport;
//   · sessions reuse the existing protocol stack with unchanged behavior (handshake, 0x4300/0x4301, iAP2, MFi).
//
// Everything platform-specific is injected via constructor arguments (WiredPorts + controllerAddress/currentSSID);
// no platform headers appear in core.
#pragma once

#include "projection/backend.h"
#include "wired_port.h"

#include <functional>
#include <string>

namespace core::mfi { class IMfiIdentity; }

namespace core::projection::carplay {

struct CarPlayBackendOptions {
    // -- Media / identity (same meaning as airplay::ReceiverConfig) --
    std::string name      = "AllPlay";
    std::string ifname;                       ///< NIC to bind for wireless; empty = automatic
    uint16_t    port      = 7000;
    int         width     = 1280;
    int         height    = 720;
    int         fps       = 60;
    std::string stateDir;                     ///< Empty = default state directory
    std::string mfiDir    = "identity";
    const core::mfi::IMfiIdentity* mfiIdentity = nullptr;   ///< Takes precedence over mfiDir (not owned)

    // -- Wireless trigger (same meaning as bt::BtTriggerParams) --
    std::string deviceName    = "iPhone";     ///< Match by name substring
    std::string deviceAddress;                ///< If non-empty, connect directly by address
    std::string ssid;
    std::string passphrase;
    int         retry         = 10;
    int         retryDelayMs  = 3000;
    bool        enableAvahi   = true;

    // -- Wired --
    unsigned    productId     = 0;            ///< 0 = any
    int         switchWaitMs  = 10000;
    std::string ncmLinkLocal;
    WiredPorts  wired;

    // -- Small platform helpers (nullable) --
    std::function<std::string()> controllerAddress;   ///< Local Bluetooth MAC (0x5703 / info)
    std::function<std::string()> currentSSID;         ///< SSID of the same network
};

class CarPlayBackend final : public IProjectionBackend {
public:
    explicit CarPlayBackend(CarPlayBackendOptions opt);
    ~CarPlayBackend() override = default;

    Protocol    protocol() const override { return Protocol::CarPlay; }
    const char* name() const override { return "CarPlay"; }

    std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                        std::string* err) override;

    std::unique_ptr<IProjectionSession> connect(const ProjectionTarget& target,
                                                const core::PlatformServices& services,
                                                IProjectionSink& sink,
                                                std::string* err) override;

private:
    CarPlayBackendOptions opt_;
};

}  // namespace core::projection::carplay
