# ============================================================================
# core —— 平台无关协议核心（唯一第三方依赖 = OpenSSL）
# ============================================================================
# 桌面（顶层 CMakeLists）与 Android（android/app/src/main/cpp/CMakeLists.txt）
# 都通过 add_subdirectory(core) 复用这份定义。core 不含任何平台源/平台头。
find_package(Threads REQUIRED)

# OpenSSL：桌面走标准 find；Android 交叉编译时 FindOpenSSL 因 NDK sysroot 找不到自定义前缀，
# 用 OPENSSL_ROOT_DIR 显式给出交叉编译出的库/头路径。
if(ANDROID AND DEFINED OPENSSL_ROOT_DIR)
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libcrypto.a" CACHE FILEPATH "")
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/lib/libssl.a" CACHE FILEPATH "")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include" CACHE PATH "")
endif()
find_package(OpenSSL REQUIRED)

add_library(allplay_core STATIC
    common/log.cpp
    bplist/bplist.cpp
    platform/device_identity.cpp
    platform/pairing_store.cpp
    airplay/rtsp.cpp
    airplay/info.cpp
    airplay/server.cpp
    airplay/session.cpp
    airplay/control_cipher.cpp
    airplay/video_stream.cpp
    airplay/event_channel.cpp
    airplay/audio_stream.cpp
    iap2/wire.cpp
    iap2/link.cpp
    iap2/hid.cpp
    iap2/wireless_client.cpp
    iap2/wired_link.cpp
    port/control.cpp
    usb/usbmux.cpp
    usb/plist_xml.cpp
    usb/tls_stream.cpp
    usb/lockdown.cpp
    usb/pair_record.cpp
    usb/wired_control.cpp
    mfi/mfi_identity.cpp
    bt/bt_trigger.cpp
    crypto/hkdf.cpp
    crypto/chacha20poly1305.cpp
    crypto/srp6a.cpp
    crypto/x25519.cpp
    crypto/ed25519.cpp
    crypto/tlv8.cpp
    crypto/aes_ctr.cpp
)
target_include_directories(allplay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# OpenSSL::SSL 是有线链路要的（lockdown 的 StartSession 之后原地升成 TLS，见 usb/tls_stream）
target_link_libraries(allplay_core PUBLIC OpenSSL::Crypto OpenSSL::SSL Threads::Threads)
target_compile_options(allplay_core PRIVATE -Wall -Wextra)
