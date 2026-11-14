# ============================================================================
# AllPlay — layered (ports & adapters)
# ============================================================================
#   core/           platform-independent protocol core (only third-party dependency = OpenSSL)
#   platform/posix  Linux+macOS adaptation (socket/avahi·dns_sd/bluez·IOBluetooth)
#   platform/windows Windows adaptation (Winsock2, TODO; currently only builds core + self-tests)
#   host/           desktop host (SDL2; skipped by default when SDL2 is missing)
# This file grows targets milestone by milestone.
cmake_minimum_required(VERSION 3.16)
project(AllPlay LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# core (platform-independent; contains no platform sources/headers) — see core/CMakeLists.txt
# ---------------------------------------------------------------------------
add_subdirectory(core)

# ---------------------------------------------------------------------------
# Self-tests (depend only on allplay_core; build on all platforms: Linux/macOS/Windows)
# ---------------------------------------------------------------------------
add_executable(iap2_test tools/iap2_test.cpp)
target_link_libraries(iap2_test PRIVATE allplay_core)
target_compile_options(iap2_test PRIVATE -Wall -Wextra)

# M3 self-tests: crypto layer (HKDF/ChaCha/SRP/X25519/Ed25519/TLV8)
add_executable(m3_test tools/m3_test.cpp)
target_link_libraries(m3_test PRIVATE allplay_core)
target_compile_options(m3_test PRIVATE -Wall -Wextra)

# Fake iPhone: runs RTSP + encrypted screen streaming against the real host (end-to-end acceptance, no real device needed).
# Deliberately **does not depend on SDL/ffmpeg** — it pushes raw H.264 and parses AnnexB itself.
# So it builds on any machine that can build core, including targets with no graphics stack.
add_executable(fake_phone tools/fake_phone.cpp)
target_link_libraries(fake_phone PRIVATE allplay_core)
target_compile_options(fake_phone PRIVATE -Wall -Wextra)

# Android Auto wire-layer self-tests: AAP framing/deframing + protobuf subset (no phone needed).
add_executable(aa_wire_test tools/aa_wire_test.cpp)
target_link_libraries(aa_wire_test PRIVATE allplay_core)
target_compile_options(aa_wire_test PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# Platform adaptation + desktop host: Unix only (the Windows Winsock2 adaptation layer is TODO)
# ---------------------------------------------------------------------------
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)

    # platform/posix adaptation (Linux + macOS)
    if(APPLE)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 17)
        set(POSIX_MDNS_SRC platform/posix/mdns_dns_sd.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_iobluetooth.mm)
        set_source_files_properties(platform/posix/bt_iobluetooth.mm
            PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
        set(POSIX_FRAMEWORKS "-framework IOBluetooth" "-framework CoreWLAN" "-framework Foundation")
        # Wired (USB) CarPlay: macOS does not touch USB — the control plane borrows the system usbmuxd tunnel, and the media plane uses
        # the en NIC created by AppleUSBNCMData (see the file header of platform/posix/usbmuxd_mac.cpp).
        set(POSIX_USB_SRC platform/posix/usbmuxd_mac.cpp)
        # Android Auto's USB transport layer: macOS must handle USB itself (libusb / IOKit),
        # whereas the system usbmuxd only handles iPhone.
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
        message(STATUS "Bluetooth transport: IOBluetooth (macOS)")
        message(STATUS "Wired transport: system usbmuxd + AppleUSBNCMData (macOS, no DriverKit needed)")
    else()
        set(POSIX_MDNS_SRC platform/posix/mdns_avahi.cpp)
        set(POSIX_BT_SRC   platform/posix/bt_bluez.cpp)
        set(POSIX_FRAMEWORKS "")
        pkg_check_modules(AVAHI_CLIENT REQUIRED IMPORTED_TARGET avahi-client)
        # BlueZ transport layer: libbluetooth for SDP queries + raw RFCOMM sockets + HCI to get the local address;
        # libdbus-1 enumerates paired devices (org.bluez) and the current SSID (NetworkManager).
        pkg_check_modules(BLUEZ REQUIRED IMPORTED_TARGET bluez)
        pkg_check_modules(DBUS1 REQUIRED IMPORTED_TARGET dbus-1)
        message(STATUS "Bluetooth transport: BlueZ (Linux, libbluetooth ${BLUEZ_VERSION})")

        # Wired (USB) CarPlay transport layer: libusb only handles "switch the iPhone into CarPlay mode" and
        # "wrap the USBMUX interface's bulk endpoints into an IByteStream"; the media plane is the kernel cdc_ncm's job.
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
        if(LIBUSB_FOUND)
            set(POSIX_USB_SRC platform/posix/usb_libusb.cpp)
            message(STATUS "Wired transport: libusb (Linux, libusb ${LIBUSB_VERSION})")
        else()
            set(POSIX_USB_SRC "")
            message(STATUS "Wired transport: skipped (libusb-1.0-dev missing)")
        endif()
    endif()

    set(POSIX_AA_USB_SRC "")
    if(LIBUSB_FOUND)
        set(POSIX_AA_USB_SRC platform/posix/aa_usb_libusb.cpp)
        message(STATUS "Android Auto USB: libusb ${LIBUSB_VERSION}")
    else()
        message(STATUS "Android Auto USB: skipped (libusb-1.0 missing)")
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
            # Give the upper layer (host) a switch for "this machine can compile in the wired link"
            target_compile_definitions(allplay_posix PUBLIC ALLPLAY_HAVE_LIBUSB=1)
        endif()
    endif()
    # Android Auto USB (needed on both Linux + macOS): libusb comes from pkg-config.
    if(LIBUSB_FOUND)
        target_link_libraries(allplay_posix PUBLIC PkgConfig::LIBUSB)
        target_compile_definitions(allplay_posix PUBLIC ALLPLAY_HAVE_AA_USB=1)
    endif()
    target_compile_options(allplay_posix PRIVATE -Wall -Wextra)

    add_executable(core_selftest tools/core_selftest.cpp)
    target_link_libraries(core_selftest PRIVATE allplay_posix)
    target_compile_options(core_selftest PRIVATE -Wall -Wextra)

    # Wired control-plane probe: on top of "switch into CarPlay mode", verifies usbmux v2 + mini TCP +
    # the lockdown plist channel. Needs allplay_posix (usb_libusb) and allplay_core (usbmux).
    # On macOS the same probe uses a different transport (system usbmuxd + AppleUSBNCMData), so it is built too.
    if(APPLE)
        add_executable(wired_probe tools/wired_probe.cpp tools/wired_selftest.cpp)
        target_link_libraries(wired_probe PRIVATE allplay_posix)
        target_compile_options(wired_probe PRIVATE -Wall -Wextra)
        message(STATUS "Wired probe wired_probe: enabled (macOS, system usbmuxd)")
    elseif(LIBUSB_FOUND)   # probe was done once in the platform branch
        add_executable(usb_probe tools/usb_probe.cpp)
        target_link_libraries(usb_probe PRIVATE PkgConfig::LIBUSB)
        target_compile_options(usb_probe PRIVATE -Wall -Wextra)
        message(STATUS "USB probe usb_probe: enabled (libusb ${LIBUSB_VERSION})")

        add_executable(wired_probe tools/wired_probe.cpp tools/wired_selftest.cpp)
        target_link_libraries(wired_probe PRIVATE allplay_posix)
        target_compile_options(wired_probe PRIVATE -Wall -Wextra)
        message(STATUS "Wired probe wired_probe: enabled")
    else()
        message(STATUS "USB probe usb_probe / wired probe wired_probe: skipped (libusb-1.0-dev missing)")
    endif()

    # Desktop development host (SDL2, optional: skipped when SDL2/SDL2_ttf are missing)
    pkg_check_modules(SDL2    IMPORTED_TARGET sdl2)
    pkg_check_modules(SDL2TTF IMPORTED_TARGET SDL2_ttf)
    # ffmpeg software decode: only avcodec/avutil/swscale.
    # **Deliberately no swresample**: its whole channel-layout parameter set has different signatures between 4.4 and 5.1+,
    # and our audio conversion only does "planar float → interleaved s16", so writing it ourselves is simpler (see ffmpeg_compat.h).
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
            allplay_posix                     # PUBLIC re-exports allplay_core + OpenSSL + avahi/IOBluetooth
            PkgConfig::SDL2
            PkgConfig::SDL2TTF)
        if(AV_FOUND)
            target_sources(allplay_host PRIVATE
                host/media/ffmpeg_decoder.cpp
                host/media/sdl_video_sink.cpp
                host/media/sdl_audio_sink.cpp)
            target_link_libraries(allplay_host PRIVATE PkgConfig::AV)
            target_compile_definitions(allplay_host PRIVATE ALLPLAY_HAVE_FFMPEG=1)
            message(STATUS "Desktop host allplay_host: enabled (with ffmpeg software decoding)")
        else()
            message(STATUS "Desktop host allplay_host: enabled, but ffmpeg missing — video degraded to a stub")
        endif()
        target_compile_options(allplay_host PRIVATE -Wall -Wextra)
    else()
        message(STATUS "Desktop host allplay_host: skipped (SDL2/SDL2_ttf missing)")
    endif()

    # Video smoke test: feed local AnnexB H.264 to verify "decode → letterbox display" (no protocol/network/Bluetooth involved).
    # Bisection point when debugging a black screen: picture = protocol side's fault, no picture = decode side's fault.
    if(SDL2_FOUND AND AV_FOUND)
        add_executable(video_probe
            tools/video_probe.cpp
            host/media/ffmpeg_decoder.cpp
            host/media/sdl_video_sink.cpp)
        target_include_directories(video_probe PRIVATE host)
        target_link_libraries(video_probe PRIVATE
            allplay_core                      # only for types like VideoCodec from port/media.h
            PkgConfig::SDL2
            PkgConfig::AV)
        target_compile_options(video_probe PRIVATE -Wall -Wextra)
        message(STATUS "Video smoke test video_probe: enabled")
    endif()
else()
    # Windows: Winsock2 adaptation + Win32 host (1280×720 window)
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
    message(STATUS "Windows host allplay_windows_host: enabled")
endif()
