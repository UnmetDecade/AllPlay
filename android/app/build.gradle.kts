plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.allplay.app"
    compileSdk = 34
    // NDK r25c: the last version supporting API 19 (Android 4.4) (from r26 the minimum is 21)
    ndkVersion = "25.2.9519653"

    defaultConfig {
        applicationId = "com.allplay.app"
        // Supports Android 4.4 (API 19). Note: this target box is 32-bit armv7l,
        // abiFilters still lists arm64-v8a; the armeabi-v7a OpenSSL cross-compile is handled separately.
        minSdk = 19
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // The target box is 32-bit armv7l and needs armeabi-v7a; arm64-v8a is kept for other devices.
            // Both need their own OpenSSL cross-compile (see OPENSSL_BASE_DIR in env.sh).
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                // core's only third-party dependency is OpenSSL; use per-ABI dirs (openssl-arm64 / openssl-arm),
                // and cpp/CMakeLists.txt derives the concrete OPENSSL_ROOT_DIR from ANDROID_ABI.
                arguments += "-DANDROID_STL=c++_static"
                System.getenv("OPENSSL_BASE_DIR")?.let { arguments += "-DOPENSSL_BASE_DIR=$it" }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
}

dependencies {
    // androidx (appcompat/core-ktx/lifecycle) is not referenced in the source, and appcompat 1.7 requires minSdk 21,
    // so it was removed; only the JmDNS actually used (mDNS advertise/discover) remains.
    implementation("org.jmdns:jmdns:3.6.3")
}
