/**
 * @file   backend_androidauto.h
 * @brief  Android Auto（有线）协议后端
 * @author blt
 */

// ============================================================================
// core/projection/androidauto/backend_androidauto.h —— Android Auto 有线后端
// ============================================================================
// 实现链路（参照 AASDK / OpenAuto 的公开实现）：
//   AOAP（51/52/53）→ 重枚举 → AAP VERSION → TLS 1.2（记录走 0x0003）
//   → AUTH_COMPLETE → SERVICE_DISCOVERY → 通道打开 / AV SETUP / 媒体流
//
// 平台只负责 USB：platform/posix/aa_usb_libusb.cpp 实现 AaUsbPorts。
// 本模块不引 protobuf / Boost，wire 编码见 proto.h，TLS 见 tls.h。
#pragma once

#include "projection/backend.h"
#include "usb_transport.h"

#include <string>

namespace core::projection::androidauto {

struct AndroidAutoOptions {
    std::string name         = "AllPlay";     ///< 车机显示名
    std::string manufacturer = "AllPlay";
    std::string model        = "AllPlay";
    std::string serial       = "ALLPLAY-AA";  ///< AOAP SERIAL 字符串 / 车机序列号

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
