# ============================================================================
# AllPlay Android native layer: core (platform-independent) + POSIX NIC/TCP + JNI bridge
# ============================================================================
# Dependencies (the only third-party lib is OpenSSL): core needs OpenSSL. Android has no system OpenSSL,
# so cross-compile one for arm64-v8a / armeabi-v7a / x86_64 (e.g. an openssl-3.x NDK build)
# and pass it in gradle's externalNativeBuild.cmake.arguments:
#   -DOPENSSL_ROOT_DIR=<openssl-install-prefix>
# Bluetooth / mDNS / video decode do not use C++ libs; they call system APIs via JNI (BluetoothSocket / NsdManager / MediaCodec).
cmake_minimum_required(VERSION 3.22)
project(allplay_jni LANGUAGES CXX)

# core uses C++17 features such as std::filesystem / nested namespaces; the desktop top level sets this, so the Android top level must too
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Project root (5 levels up from android/app/src/main/cpp)
get_filename_component(ALLPLAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# Cross-compile OpenSSL into per-ABI dirs: <OPENSSL_BASE_DIR>/openssl-arm64 and openssl-arm.
# Set OPENSSL_ROOT_DIR before add_subdirectory(core) (which runs find_package(OpenSSL) internally).
if(ANDROID AND DEFINED OPENSSL_BASE_DIR)
    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-arm")
    elseif(ANDROID_ABI STREQUAL "arm64-v8a")
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-arm64")
    else()
        set(OPENSSL_ROOT_DIR "${OPENSSL_BASE_DIR}/openssl-${ANDROID_ABI}")
    endif()
endif()

# core (platform-independent; includes find_package(OpenSSL/Threads))
add_subdirectory("${ALLPLAY_ROOT}/core" core)

# POSIX TCP/NIC (the NDK provides POSIX sockets + getifaddrs)
add_library(allplay_posix_net STATIC
    "${ALLPLAY_ROOT}/platform/posix/net_posix.cpp")
target_include_directories(allplay_posix_net PUBLIC
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")

# JNI bridge: implements the core::port interfaces as downcalls to Kotlin service objects
add_library(allplay_jni SHARED jni_bridge.cpp stdio_compat.cpp)
target_include_directories(allplay_jni PRIVATE
    "${ALLPLAY_ROOT}/core"
    "${ALLPLAY_ROOT}/platform")
target_link_libraries(allplay_jni
    allplay_core
    allplay_posix_net
    log)
