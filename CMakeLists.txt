# ============================================================================
# AllPlay —— 分层（ports & adapters）
# ============================================================================
#   core/           平台无关协议核心（唯一三方依赖 = OpenSSL）
#   platform/posix  Linux+macOS 适配（socket/avahi·dns_sd/bluez·IOBluetooth）
#   platform/windows Windows 适配（Winsock2，待做；当前只构建 core + 自测）
#   host/           桌面宿主（SDL2；缺 SDL2 时可缺省跳过）
# 本文件随里程碑逐步长出 target。
cmake_minimum_required(VERSION 3.16)
project(AllPlay LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# core（平台无关；不含任何平台源/平台头）—— 见 core/CMakeLists.txt
# ---------------------------------------------------------------------------
add_subdirectory(core)

# ---------------------------------------------------------------------------
# 自测（只依赖 allplay_core，全平台：Linux/macOS/Windows 都能编）
# ---------------------------------------------------------------------------
add_executable(iap2_test tools/iap2_test.cpp)
target_link_libraries(iap2_test PRIVATE allplay_core)
target_compile_options(iap2_test PRIVATE -Wall -Wextra)

# M3 自测：加密层（HKDF/ChaCha/SRP/X25519/Ed25519/TLV8）
add_executable(m3_test tools/m3_test.cpp)
target_link_libraries(m3_test PRIVATE allplay_core)
target_compile_options(m3_test PRIVATE -Wall -Wextra)

# 假 iPhone：对着真宿主跑完 RTSP + 加密屏幕流（端到端验收，不需要真机）。
# 有意**不依赖 SDL/ffmpeg** —— 它只推裸 H.264，自己解析 AnnexB。
# 因此它在任何能编 core 的机器上都能编，包括没有图形栈的目标机。
add_executable(fake_phone tools/fake_phone.cpp)
target_link_libraries(fake_phone PRIVATE allplay_core)
target_compile_options(fake_phone PRIVATE -Wall -Wextra)

# Android Auto wire 层自测：AAP 分帧/拼帧 + protobuf 子集（不需要手机）。
add_executable(aa_wire_test tools/aa_wire_test.cpp)
target_link_libraries(aa_wire_test PRIVATE allplay_core)
target_compile_options(aa_wire_test PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# 平台适配 + 桌面宿主：仅 Unix（Windows 的 Winsock2 适配层待做）
# ---------------------------------------------------------------------------
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)

    # platform/posix 适配（Linux + macOS）
    if(APPLE)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 17)
        set(POSIX_MDNS_SRC platform/posix/mdns_dns_sd.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_iobluetooth.mm)
        set_source_files_properties(platform/posix/bt_iobluetooth.mm
            PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
        set(POSIX_FRAMEWORKS "-framework IOBluetooth" "-framework CoreWLAN" "-framework Foundation")
        # 有线（USB）CarPlay：macOS 不碰 USB —— 控制面借系统 usbmuxd 的隧道，媒体面用
        # AppleUSBNCMData 自己建好的 en 网卡（见 platform/posix/usbmuxd_mac.cpp 文件头）。
        set(POSIX_USB_SRC platform/posix/usbmuxd_mac.cpp)
        # Android Auto 的 USB 传输层：macOS 必须自己碰 USB（libusb / IOKit），
        # 系统 usbmuxd 只管 iPhone。
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
        message(STATUS "蓝牙传输: IOBluetooth (macOS)")
        message(STATUS "有线传输: 系统 usbmuxd + AppleUSBNCMData (macOS，不需要 DriverKit)")
    else()
        set(POSIX_MDNS_SRC platform/posix/mdns_avahi.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_bluez.cpp)
        set(POSIX_FRAMEWORKS "")
        pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)
        # BlueZ 传输层：libbluetooth 出 SDP 查询 + 裸 RFCOMM socket + HCI 取本机地址；
        # libdbus-1 用来枚举已配对设备（org.bluez）与当前 SSID（NetworkManager）。
        pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
        pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)
        message(STATUS "蓝牙传输: BlueZ (Linux，libbluetooth ${BLUEZ_VERSION})")

        # 有线（USB）CarPlay 的传输层：libusb 只负责"把 iPhone 切进 CarPlay 配置"和
        # "把 USBMUX 口的 bulk 端点包成 IByteStream"两件事；媒体面是内核 cdc_ncm 的事。
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
        if(LIBUSB_FOUND)
            set(POSIX_USB_SRC platform/posix/usb_libusb.cpp)
            message(STATUS "有线传输: libusb (Linux，libusb ${LIBUSB_VERSION})")
        else()
            set(POSIX_USB_SRC "")
            message(STATUS "有线传输: 跳过（缺 libusb-1.0-dev）")
        endif()
    endif()

    set(POSIX_AA_USB_SRC "")
    if(LIBUSB_FOUND)
        set(POSIX_AA_USB_SRC platform/posix/aa_usb_libusb.cpp)
        message(STATUS "Android Auto USB: libusb ${LIBUSB_VERSION}")
    else()
        message(STATUS "Android Auto USB: 跳过（缺 libusb-1.0）")
    endif()

    add_library(allplay_posix STATIC
        platform/posix/net_posix.cpp
        ${POSIX_MDNS_SRC}
        ${POSIX_BT_SRC}
        ${POSIX_USB_SRC}
        ${POSIX_AA_USB_SRC}
    )
    target_include_directories(allplay_posix PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/platform)
    target_link_libraries(allplay_posix PUBLIC allplay_core ${POSIX_FRAMEWORKS})
    if(NOT APPLE)
        target_link_libraries(allplay_posix PUBLIC
            PkgConfig::AVAHI_CLIENT PkgConfig::BLUEZ PkgConfig::DBUS1)
        if(LIBUSB_FOUND)
            target_link_libraries(allplay_posix PUBLIC PkgConfig::LIBUSB)
            # 给上层（host）一个"这台机器编得进有线链路"的开关
            target_compile_definitions(allplay_posix PUBLIC ALLPLAY_HAVE_LIBUSB=1)
        endif()
    endif()
    # Android Auto 的 USB（Linux + macOS 都要）：libusb 由 pkg-config 提供。
    if(LIBUSB_FOUND)
        target_link_libraries(allplay_posix PUBLIC PkgConfig::LIBUSB)
        target_compile_definitions(allplay_posix PUBLIC ALLPLAY_HAVE_AA_USB=1)
    endif()
    target_compile_options(allplay_posix PRIVATE -Wall -Wextra)

    add_executable(core_selftest tools/core_selftest.cpp)
    target_link_libraries(core_selftest PRIVATE allplay_posix)
    target_compile_options(core_selftest PRIVATE -Wall -Wextra)

    # 有线控制面探针：在"切进 CarPlay 配置"之上，验 usbmux v2 + 迷你 TCP +
    # lockdown plist 通道这一段。要 allplay_posix（usb_libusb）与 allplay_core（usbmux）。
    # macOS 上同一个探针换一条传输层（系统 usbmuxd + AppleUSBNCMData），所以也编。
    if(APPLE)
        add_executable(wired_probe tools/wired_probe.cpp tools/wired_selftest.cpp)
        target_link_libraries(wired_probe PRIVATE allplay_posix)
        target_compile_options(wired_probe PRIVATE -Wall -Wextra)
        message(STATUS "有线探针 wired_probe: 启用（macOS，系统 usbmuxd）")
    elseif(LIBUSB_FOUND)   # 探测在平台分支里做过一次
        add_executable(usb_probe tools/usb_probe.cpp)
        target_link_libraries(usb_probe PRIVATE PkgConfig::LIBUSB)
        target_compile_options(usb_probe PRIVATE -Wall -Wextra)
        message(STATUS "USB 探针 usb_probe: 启用（libusb ${LIBUSB_VERSION}）")

        add_executable(wired_probe tools/wired_probe.cpp tools/wired_selftest.cpp)
        target_link_libraries(wired_probe PRIVATE allplay_posix)
        target_compile_options(wired_probe PRIVATE -Wall -Wextra)
        message(STATUS "有线探针 wired_probe: 启用")
    else()
        message(STATUS "USB 探针 usb_probe / 有线探针 wired_probe: 跳过（缺 libusb-1.0-dev）")
    endif()

    # 桌面开发宿主（SDL2，可选：缺 SDL2/SDL2_ttf 时跳过）
    pkg_check_modules(SDL2    IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2TTF IMPORTED_TARGET SDL2_ttf)
    # ffmpeg 软解：只要 avcodec/avutil/swscale。
    # **故意不要 swresample**：它整套 channel-layout 参数在 4.4 与 5.1+ 之间签名不同，
    # 而我们的音频转换只有"平面 float → 交织 s16"这一件事，自己写更省事（见 ffmpeg_compat.h）。
    pkg_check_modules(AV      IMPORTED_TARGET libavcodec libavutil libswscale)
    if(SDL2_FOUND AND SDL2TTF_FOUND)
        add_executable(allplay_host
            host/main.cpp
            host/bt_task.cpp
            host/wired_task.cpp
            host/statusbar.cpp
            host/launcher.cpp
        )
        target_include_directories(allplay_host PRIVATE host)
        target_link_libraries(allplay_host PRIVATE
            allplay_posix                     # PUBLIC 带出 allplay_core + OpenSSL + avahi/IOBluetooth
            PkgConfig::SDL2
            PkgConfig::SDL2TTF)
        if(AV_FOUND)
            target_sources(allplay_host PRIVATE
                host/media/ffmpeg_decoder.cpp
                host/media/sdl_video_sink.cpp
                host/media/sdl_audio_sink.cpp)
            target_link_libraries(allplay_host PRIVATE PkgConfig::AV)
            target_compile_definitions(allplay_host PRIVATE ALLPLAY_HAVE_FFMPEG=1)
            message(STATUS "桌面宿主 allplay_host: 启用（含 ffmpeg 软解）")
        else()
            message(STATUS "桌面宿主 allplay_host: 启用，但缺 ffmpeg —— 视频降级为桩")
        endif()
        target_compile_options(allplay_host PRIVATE -Wall -Wextra)
    else()
        message(STATUS "桌面宿主 allplay_host: 跳过（缺 SDL2/SDL2_ttf）")
    endif()

    # 视频冒烟：本地喂 AnnexB H.264，验证"解码 → letterbox 上屏"（不碰协议/网络/蓝牙）。
    # 排查黑屏时的二分点：有画面 = 协议侧的锅，没画面 = 解码侧的锅。
    if(SDL2_FOUND AND AV_FOUND)
        add_executable(video_probe
            tools/video_probe.cpp
            host/media/ffmpeg_decoder.cpp
            host/media/sdl_video_sink.cpp)
        target_include_directories(video_probe PRIVATE host)
        target_link_libraries(video_probe PRIVATE
            allplay_core                      # 只为 port/media.h 的 VideoCodec 等类型
            PkgConfig::SDL2
            PkgConfig::AV)
        target_compile_options(video_probe PRIVATE -Wall -Wextra)
        message(STATUS "视频冒烟 video_probe: 启用")
    endif()
else()
    # Windows：Winsock2 适配 + Win32 宿主（1280×720 窗口）
    add_library(allplay_windows STATIC
        platform/windows/net_windows.cpp
    )
    target_include_directories(allplay_windows PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/platform)
    target_link_libraries(allplay_windows PUBLIC allplay_core ws2_32 iphlpapi)
    target_compile_options(allplay_windows PRIVATE -Wall -Wextra)

    add_executable(allplay_windows_host
        host/windows_main.cpp
    )
    target_include_directories(allplay_windows_host PRIVATE host)
    target_link_libraries(allplay_windows_host PRIVATE allplay_windows)
    target_compile_options(allplay_windows_host PRIVATE -Wall -Wextra)
    message(STATUS "Windows 宿主 allplay_windows_host: 启用")
endif()
