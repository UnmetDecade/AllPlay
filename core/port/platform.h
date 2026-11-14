/**
 * @file   platform.h
 * @brief  Bundle of platform capabilities injected into core by the composition root
 * @author blt
 */

// ============================================================================
// core/port/platform.h — bundle of platform capabilities injected into core by the composition root
// ============================================================================
// core depends only on the interfaces here + OpenSSL + the C++ stdlib; no platform headers appear.
// Each platform's composition root (desktop app/main.cpp, Android jni_bridge, the various tools) assembles
// these implementations and injects them. Factories use std::function because listeners/broadcasters need "one per use".
#pragma once

#include "port/bluetooth.h"
#include "port/mdns.h"
#include "port/network.h"
#include "port/tcp.h"
#include "port/udp.h"

#include <functional>
#include <memory>

namespace core {

struct PlatformServices {
    INetworkInfo* network = nullptr;      ///< NIC IP/MAC (for AirPlay advertising and 0x4301)
    IBluetooth*   bluetooth = nullptr;    ///< Bluetooth trigger; may be null if no trigger

    /// Create a TCP listener (one each for RTSP/data/event ports)
    std::function<std::unique_ptr<ITcpListener>()> makeTcpListener;

    /// Create an mDNS broadcaster (_airplay._tcp)
    std::function<std::unique_ptr<IMdnsAdvertiser>()> makeMdnsAdvertiser;

    /// Create a UDP socket (audio RTP/RTCP)
    std::function<std::unique_ptr<IUdpSocket>()> makeUdpSocket;
};

}  // namespace core
