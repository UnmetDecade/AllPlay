// AllPlay — Android host (Kotlin + NDK C++ core + JNI)
// core is platform-independent C++ (see ../core), wired in through the JNI bridge in app/src/main/cpp.
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
}
