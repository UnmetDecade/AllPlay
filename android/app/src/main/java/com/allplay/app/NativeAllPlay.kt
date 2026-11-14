package com.allplay.app

import android.view.Surface

/**
 * JNI declarations for the native bridge (corresponding to the
 * Java_com_allplay_app_NativeAllPlay_* entry points in jni_bridge.cpp).
 */
class NativeAllPlay {
    companion object {
        init {
            System.loadLibrary("allplay_jni")
        }
    }

    /// Start core (Receiver): hand the Bluetooth/mDNS/video service objects to native for downcalls.
    /// @return RTSP port (>0 on success; -1 on failure)
    external fun nativeStart(
        bt: BluetoothService,
        mdns: MdnsService,
        video: MediaCodecSink,
        audio: AudioSink,
        stateDir: String,
        identityDir: String,
        name: String,
        btMac: String,
        ssid: String,
        passphrase: String,
        ip: String,
        width: Int,
        height: Int,
        skipBtTrigger: Boolean,
        fps: Int,
    ): Int

    external fun nativeStop()

    external fun nativeSetSurface(surface: Surface)

    /// Send touch: each group is 3 floats [x, y, down(0/1)], at most 2 groups; coordinates normalized 0..1
    external fun nativeSendTouch(points: FloatArray)

    /// Request a keyframe from the iPhone immediately (called after the Surface is rebuilt on returning to foreground, to restore the picture ASAP)
    external fun nativeRequestKeyFrame()

    /// Session state: 0=waiting for connection, 1=connecting, 2=connected
    external fun nativeSessionState(): Int

    /// Local deviceid (e.g. "02:F5:92:44:8E:EC"), used to probe /ctrl-int/1/connect
    external fun nativeDeviceId(): String
}
