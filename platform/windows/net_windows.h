/**
 * @file   net_windows.h
 * @brief  Windows (Winsock2) network adapters: TCP/UDP/network interfaces + composition-root factories
 * @author blt
 */

// ============================================================================
// platform/windows/net_windows.h -- Winsock2 adapter declarations
// ============================================================================
// Implements core::ITcpListener / ITcpStream / IUdpSocket / INetworkInfo.
// Read timeouts use select(); accept polls with select(), and stop() wakes it by closing the socket.
#pragma once

#include "port/mdns.h"
#include "port/network.h"
#include "port/platform.h"
#include "port/tcp.h"
#include "port/udp.h"

#include <memory>
#include <string>

namespace allplay::windows {

/// Initialize Winsock2 in-process (idempotent). Writes err and returns false on failure.
bool initWinsock(std::string* err);

core::INetworkInfo& networkInfo();
std::unique_ptr<core::ITcpListener> makeTcpListener();
std::unique_ptr<core::IUdpSocket> makeUdpSocket();
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser();
core::IBluetooth* makeBluetooth();   // Bluetooth iAP2 trigger (bt_windows.cpp, client side)
core::PlatformServices makePlatformServices();

}  // namespace allplay::windows
