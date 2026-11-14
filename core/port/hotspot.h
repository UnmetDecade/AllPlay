/**
 * @file   hotspot.h
 * @brief  Wireless hotspot abstraction (hotspot mode)
 * @author blt
 */

// ============================================================================
// core/port/hotspot.h — wireless hotspot abstraction (hotspot mode)
// ============================================================================
// Hotspot mode: the accessory starts a local hotspot, the iPhone joins it, and the two connect directly without a router.
// Platform implementations: Android = WifiManager.startLocalOnlyHotspot (LocalOnlyHotspotManager);
//           desktop = stub (Linux create_ap / NetworkManager to follow).
// core does not own the hotspot; it only fills its ssid/passphrase/ip into WirelessTriggerConfig.
#pragma once

#include <string>

namespace core {

class IHotspot {
public:
    virtual ~IHotspot() = default;

    /// Start the hotspot (blocks until ready or failed). Returns false and sets err on failure.
    virtual bool start(std::string* err) = 0;
    virtual void stop() = 0;

    virtual const std::string& ssid() const = 0;        ///< Hotspot name (0x5703 to the iPhone)
    virtual const std::string& passphrase() const = 0;  ///< Hotspot passphrase (0x5703 to the iPhone)
    virtual const std::string& ip() const = 0;          ///< IPv4 of the hotspot interface (ipAddresses in 0x4301)
};

}  // namespace core
