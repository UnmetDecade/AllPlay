/**
 * @file   backend_androidauto.h
 * @brief  Android Auto (wired) protocol backend
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/backend_androidauto.h - Android Auto wired backend
// ============================================================================
// Implementation path (following the public AASDK / OpenAuto implementations):
//   AOAP (51/52/53) -> re-enumeration -> AAP VERSION -> TLS 1.2 (records go over 0x0003)
//   -> AUTH_COMPLETE -> SERVICE_DISCOVERY -> channel open / AV SETUP / media streaming
//
// The platform handles only USB: platform/posix/aa_usb_libusb.cpp implements AaUsbPorts.
// This module pulls in neither protobuf nor Boost; see proto.h for wire encoding and tls.h for TLS.
#pragma once

#include "projection/backend.h"
#include "usb_transport.h"

#include <string>

namespace core::projection::androidauto {

struct AndroidAutoOptions {
    std::string name         = "AllPlay";     ///< Display name on the head unit
    std::string manufacturer = "AllPlay";
    std::string model        = "AllPlay";
    std::string serial       = "ALLPLAY-AA";  ///< AOAP SERIAL string / head unit serial

    int width  = 1280;
    int height = 720;
    int fps    = 30;

    int audio_rate     = 48000;
    int audio_channels = 2;
    int audio_bits     = 16;

    AaUsbPorts usb;
};

class AndroidAutoBackend final : public IProjectionBackend {
public:
    explicit AndroidAutoBackend(AndroidAutoOptions opt);
    ~AndroidAutoBackend() override = default;

    Protocol    protocol() const override { return Protocol::AndroidAuto; }
    const char* name() const override { return "Android Auto"; }

    std::vector<ProjectionTarget> probe(const core::PlatformServices& services,
                                        std::string* err) override;

    std::unique_ptr<IProjectionSession> connect(const ProjectionTarget& target,
                                                const core::PlatformServices& services,
                                                IProjectionSink& sink,
                                                std::string* err) override;

private:
    AndroidAutoOptions opt_;
};

}  // namespace core::projection::androidauto
