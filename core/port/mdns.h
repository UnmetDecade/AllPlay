/**
 * @file   mdns.h
 * @brief  mDNS broadcast abstraction
 * @author blt
 */

// ============================================================================
// core/port/mdns.h — mDNS broadcast abstraction
// ============================================================================
// Platform backends: Linux=avahi-daemon (D-Bus), macOS=mDNSResponder (dns_sd.h),
// Android=NsdManager. Behavior is consistent: start() failure is not fatal (RTSP still listens, it is just invisible on the network).
// Peer discovery (receiving _carplay-ctrl._tcp) belongs to M3; a browse interface will be added at this layer then.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core {

class IMdnsAdvertiser {
public:
    virtual ~IMdnsAdvertiser() = default;

    /// @param name service instance name (= the name shown in the AirPlay list)
    /// @param port actual RTSP port
    /// @param txt  list of "key=value" (deviceid/features/flags/model/srcvers/protovers/pi/pk)
    virtual bool start(const std::string& name, uint16_t port,
                       const std::vector<std::string>& txt, std::string* err) = 0;
    virtual void stop() = 0;
};

}  // namespace core
