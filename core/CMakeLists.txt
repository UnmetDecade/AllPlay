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
    projection/registry.cpp
    projection/carplay/backend_carplay.cpp
    projection/androidauto/aap.cpp
    projection/androidauto/tls.cpp
    projection/androidauto/backend_androidauto.cpp
    projection/carlife/backend_carlife.cpp
    projection/hicar/backend_hicar.cpp
    projection/carplay/airplay/rtsp.cpp
    projection/carplay/airplay/info.cpp
    projection/carplay/airplay/server.cpp
    projection/carplay/airplay/session.cpp
    projection/carplay/airplay/control_cipher.cpp
    projection/carplay/airplay/video_stream.cpp
    projection/carplay/airplay/event_channel.cpp
    projection/carplay/airplay/audio_stream.cpp
    projection/carplay/iap2/wire.cpp
    projection/carplay/iap2/link.cpp
    projection/carplay/iap2/hid.cpp
    projection/carplay/iap2/wireless_client.cpp
    projection/carplay/iap2/wired_link.cpp
    port/control.cpp
    projection/carplay/usb/usbmux.cpp
    projection/carplay/usb/plist_xml.cpp
    projection/carplay/usb/tls_stream.cpp
    projection/carplay/usb/lockdown.cpp
    projection/carplay/usb/pair_record.cpp
    projection/carplay/usb/wired_control.cpp
    projection/carplay/mfi/mfi_identity.cpp
    projection/carplay/bt/bt_trigger.cpp
    crypto/hkdf.cpp
    crypto/chacha20poly1305.cpp
    crypto/srp6a.cpp
    crypto/x25519.cpp
    crypto/ed25519.cpp
    crypto/tlv8.cpp
    crypto/aes_ctr.cpp
)
target_include_directories(allplay_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    # CarPlay 后端内部仍写 "airplay/…"、"iap2/…"；目录搬进 projection/carplay 后
    # 靠这条 include 路径保持原样，搬迁就是纯 git mv。
    ${CMAKE_CURRENT_SOURCE_DIR}/projection/carplay
)
# OpenSSL::SSL 是有线链路要的（lockdown 的 StartSession 之后原地升成 TLS，见 usb/tls_stream）
target_link_libraries(allplay_core PUBLIC OpenSSL::Crypto OpenSSL::SSL Threads::Threads)
target_compile_options(allplay_core PRIVATE -Wall -Wextra)
