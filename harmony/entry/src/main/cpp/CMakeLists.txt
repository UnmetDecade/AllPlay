# ============================================================================
# AllPlay 鸿蒙原生层：core（协议）+ NAPI 桥
# ============================================================================
# 这一版**只编无线 iAP2 需要的那几个 core 文件**，不编整份 core：
#   · usb/*（有线 CarPlay）与 airplay/*（RTSP 接收）依赖 OpenSSL，等交叉编译了
#     OHOS 版 OpenSSL 再接进来；
#   · mfi_identity.cpp 在没有 OHOS 版 OpenSSL 时换成 mfi_identity_stub.cpp
#     （ALLPLAY_MFI_STUB）—— 那时只验"链路握手 + 识别"，MFi 会明确回报失败。
# 蓝牙 / mDNS / 视频解码不走 C++ 库，由 ArkTS 侧调系统 API，经 NAPI 桥喂字节。
cmake_minimum_required(VERSION 3.5.0)
project(allplay_harmony LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 项目根（harmony/entry/src/main/cpp 上溯 5 层：main → src → entry → harmony → 根）
get_filename_component(ALLPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# ---- MFi 签名（先定，因为 wireless_client.h 会 include mfi_identity.h）----
# 有 OHOS 版 OpenSSL 就编真实现；没有就编空实现。OpenSSL 由
# scripts/build-ohos-openssl.sh 产出。
# allplay_proto 也依赖这个头，所以这个 target 必须排在最前面：
# 它的 PUBLIC include 目录 / 宏会顺着 target_link_libraries 传下去。
set(OHOS_OPENSSL_ROOT "${ALLPLAY_ROOT}/tmp/ohos-ssl/install-aarch64" CACHE PATH
    "OHOS 版 OpenSSL 安装前缀（scripts/build-ohos-openssl.sh 的产物）")
if(EXISTS "${OHOS_OPENSSL_ROOT}/lib/libcrypto.a")
    message(STATUS "MFi：使用 ${OHOS_OPENSSL_ROOT}/lib/libcrypto.a")
    add_library(allplay_mfi STATIC
        ${ALLPLAY_ROOT}/core/projection/carplay/mfi/mfi_identity.cpp
        # AirPlay 配对层的本机身份（Ed25519 + pairing id）——
        # 0x4301 里的 publicKey/deviceIdentifier 就是它，没有它 iOS 不认这台"接收端"。
        ${ALLPLAY_ROOT}/core/platform/device_identity.cpp)
    target_include_directories(allplay_mfi PUBLIC
        ${ALLPLAY_ROOT}/core
        # CarPlay 协议代码内部仍写 "airplay/…"、"iap2/…"、"mfi/…"
        ${ALLPLAY_ROOT}/core/projection/carplay
        ${OHOS_OPENSSL_ROOT}/include)
    target_link_libraries(allplay_mfi PUBLIC ${OHOS_OPENSSL_ROOT}/lib/libcrypto.a)
else()
    message(STATUS "MFi：没有 ${OHOS_OPENSSL_ROOT}/lib/libcrypto.a，用空实现（签名不可用）")
    add_library(allplay_mfi STATIC ${ALLPLAY_ROOT}/core/projection/carplay/mfi/mfi_identity_stub.cpp)
    target_include_directories(allplay_mfi PUBLIC
        ${ALLPLAY_ROOT}/core
        ${ALLPLAY_ROOT}/core/projection/carplay)
    target_compile_definitions(allplay_mfi PUBLIC ALLPLAY_MFI_STUB=1)
endif()

# ---- core 子集（平台无关）----
add_library(allplay_proto STATIC
    ${ALLPLAY_ROOT}/core/common/log.cpp
    ${ALLPLAY_ROOT}/core/projection/carplay/iap2/wire.cpp
    ${ALLPLAY_ROOT}/core/projection/carplay/iap2/link.cpp
    ${ALLPLAY_ROOT}/core/projection/carplay/iap2/hid.cpp
    ${ALLPLAY_ROOT}/core/projection/carplay/iap2/wireless_client.cpp
    ${ALLPLAY_ROOT}/core/port/control.cpp
)
target_include_directories(allplay_proto PUBLIC
    ${ALLPLAY_ROOT}/core
    ${ALLPLAY_ROOT}/core/projection/carplay)
# wireless_client.h 里带着 IMfiIdentity 指针 —— 头文件要么拉 openssl/evp.h，
# 要么要 ALLPLAY_MFI_STUB，两者都由 allplay_mfi 的 PUBLIC 属性给出。
target_link_libraries(allplay_proto PUBLIC allplay_mfi)
target_compile_options(allplay_proto PRIVATE -Wall -Wextra)

# ---- AirPlay 接收端（RTSP 服务器 + 会话状态机）----
# 只在交叉编译好的 libcrypto.a 存在时才编 —— SRP-6a / Ed25519 / X25519 / AES-CTR
# 全靠它，没有 OpenSSL 这部分就是空的。
if(EXISTS "${OHOS_OPENSSL_ROOT}/lib/libcrypto.a")
    add_library(allplay_airplay STATIC
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/server.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/rtsp.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/session.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/video_stream.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/audio_stream.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/event_channel.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/info.cpp
        ${ALLPLAY_ROOT}/core/projection/carplay/airplay/control_cipher.cpp
        ${ALLPLAY_ROOT}/core/bplist/bplist.cpp
        ${ALLPLAY_ROOT}/core/crypto/aes_ctr.cpp
        ${ALLPLAY_ROOT}/core/crypto/chacha20poly1305.cpp
        ${ALLPLAY_ROOT}/core/crypto/ed25519.cpp
        ${ALLPLAY_ROOT}/core/crypto/hkdf.cpp
        ${ALLPLAY_ROOT}/core/crypto/srp6a.cpp
        ${ALLPLAY_ROOT}/core/crypto/tlv8.cpp
        ${ALLPLAY_ROOT}/core/crypto/x25519.cpp
        ${ALLPLAY_ROOT}/core/platform/pairing_store.cpp
        # OHOS 是 POSIX 系：TCP 监听 / UDP / 网卡信息直接复用桌面那份实现
        ${ALLPLAY_ROOT}/platform/posix/net_posix.cpp
        # 空 mDNS 广播器 + 组合根（mDNS 由 ArkTS 的 @ohos.net.mdns 负责）
        harmony_platform.cpp
    )
    target_include_directories(allplay_airplay PUBLIC
        ${ALLPLAY_ROOT}/core
        ${ALLPLAY_ROOT}/core/projection/carplay
        ${ALLPLAY_ROOT}/platform
        ${OHOS_OPENSSL_ROOT}/include)
    target_link_libraries(allplay_airplay PUBLIC
        allplay_proto
        allplay_mfi
        ${OHOS_OPENSSL_ROOT}/lib/libcrypto.a)
    target_compile_options(allplay_airplay PRIVATE -Wall -Wextra)
endif()

# ---- NAPI 模块：liballplay_core.so ----
add_library(allplay_core SHARED
    napi_bridge.cpp
)
target_include_directories(allplay_core PRIVATE ${ALLPLAY_ROOT}/core)
target_link_libraries(allplay_core PUBLIC
    allplay_proto
    allplay_mfi
    libace_napi.z.so
    libhilog_ndk.z.so
)
if(TARGET allplay_airplay)
    target_compile_definitions(allplay_core PRIVATE ALLPLAY_HAVE_AIRPLAY=1)
    # 硬解：AVCodec NDK（vdec / codecbase / core）+ 用 surfaceId 换 NativeWindow
    # 音频：acodec 解码 + ohaudio 渲染（PCM 直接交给系统播放器）
    target_sources(allplay_core PRIVATE harmony_decoder.cpp harmony_audio.cpp)
    target_link_libraries(allplay_core PUBLIC
        allplay_airplay
        native_media_vdec
        native_media_acodec
        native_media_codecbase
        native_media_core
        native_window
        ohaudio)
endif()
