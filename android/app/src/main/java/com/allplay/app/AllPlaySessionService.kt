package com.allplay.app

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.provider.Settings
import android.view.Surface

/**
 * Foreground service: hosts the native core (Receiver) permanently. Bluetooth trigger → pairing → projection all happen here.
 * MainActivity only handles the Surface and starting/stopping this service.
 */
class AllPlaySessionService : Service() {
    private lateinit var native: NativeAllPlay
    private lateinit var bt: BluetoothService
    private lateinit var mdns: MdnsService
    private lateinit var video: MediaCodecSink
    private lateinit var audio: AudioSink
    private lateinit var hotspot: HotspotManager
    private var started = false
    private var wakeLock: PowerManager.WakeLock? = null
    private var nativeWidth = 0
    private var nativeHeight = 0
    private var hotspotReady = false
    private var cachedSsid = ""
    private var cachedPass = ""
    private var cachedIp = ""
    private var lastRestartMs = 0L

    override fun onCreate() {
        super.onCreate()
        ensureIdentity()   // On first launch, unpack the MFi identity bundled in the APK (identity.pk8 + certificate.p7b) into files/identity
        val app = application as AllPlayApp
        app.service = this
        native = NativeAllPlay()
        bt = BluetoothService()
        mdns = MdnsService(this)
        video = MediaCodecSink()
        audio = AudioSink()
        hotspot = HotspotManager(this)
        // Background keep-alive: hold a partial wake lock so CPU/network sleep after backgrounding does not drop CarPlay
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AllPlay:CarPlay").apply { acquire() }
        // The Surface may become ready before this service (MainActivity surfaceCreated runs before service creation); pass it in once
        app.surface?.let { s -> video.setSurface(s) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(1, buildNotification())
        if (!started) {
            started = true
            startNative()
        }
        return START_STICKY
    }

    /// On first launch, unpack the MFi identity bundled in the APK into files/identity (identity.pk8 + certificate.p7b).
    /// If already present, leave it alone, so files later overwritten via adb push take precedence over the APK copy.
    private fun ensureIdentity() {
        val dir = java.io.File(filesDir, "identity")
        val key = java.io.File(dir, "identity.pk8")
        val cert = java.io.File(dir, "certificate.p7b")
        if (key.exists() && cert.exists()) return
        dir.mkdirs()
        try {
            if (!key.exists()) copyAsset("identity/identity.pk8", key)
            if (!cert.exists()) copyAsset("identity/certificate.p7b", cert)
            android.util.Log.i("AllPlay", "MFi identity copied from APK to files/identity")
        } catch (e: Exception) {
            android.util.Log.e("AllPlay", "Failed to copy MFi identity; CarPlay authentication will fail", e)
        }
    }

    private fun copyAsset(name: String, dest: java.io.File) {
        assets.open(name).use { input ->
            java.io.FileOutputStream(dest).use { output ->
                input.copyTo(output)
            }
        }
    }

    /// Screen orientation: landscape by default (width > height); portrait if files/portrait exists (width < height).
    /// Smooth mode: halve the resolution (pixels drop to 1/4, aspect ratio unchanged).
    private fun resolutionForOrientation(): Pair<Int, Int> {
        val portraitMode = java.io.File(filesDir, "portrait").exists()
        // Performance mode: fixed 1280x720 @ 30fps (no more halved resolution)
        if (smoothMode) return if (portraitMode) 720 to 1280 else 1280 to 720
        val wm = getSystemService(WINDOW_SERVICE) as android.view.WindowManager
        val dm = android.util.DisplayMetrics()
        wm.defaultDisplay.getRealMetrics(dm)
        val big = maxOf(dm.widthPixels, dm.heightPixels)
        val small = minOf(dm.widthPixels, dm.heightPixels)
        return if (portraitMode) small to big else big to small
    }

    private fun startNative(restart: Boolean = false) {
        val (w, h) = resolutionForOrientation()
        nativeWidth = w; nativeHeight = h
        val stateDir = filesDir.absolutePath
        val identityDir = "$stateDir/identity"   // identity.pk8 + certificate.p7b are placed here at install/first launch
        // Hotspot mode: open the hotspot on a background thread → start with the hotspot SSID/passphrase/IP (to avoid blocking the main thread)
        // LAN mode: if files/lan exists, skip the hotspot and use the current WiFi directly (iPhone and tablet on the same network)
        val lanMode = java.io.File(filesDir, "lan").exists()
        Thread {
            android.util.Log.i("AllPlay", "startNative thread started")
            var ssid = ""; var pass = ""; var ip = ""
            // First detect a system hotspot the user turned on manually (on vivo the ROM blocks app-created hotspots, but a manual one works)
            val manualSsid = if (!lanMode) hotspot.readManualApSsid() else null
            val manualIp = if (!lanMode) hotspot.manualHotspotIp() else null
            android.util.Log.i("AllPlay", "Manual hotspot detection: ssid=$manualSsid ip=$manualIp")
            if (lanMode) {
                ssid = currentSsid()
                pass = wifiPassphrase().ifEmpty { "12345678" }   // placeholder passphrase → securityType=WPA2; the iPhone is already on WiFi so no real passphrase is needed
                android.util.Log.i("AllPlay", "LAN mode: skipping hotspot, using current network ssid=$ssid")
            } else if (manualIp != null) {
                // Manual hotspot active: prefer the system SSID (getSoftApConfiguration, needs a signature permission), fall back to files/ssid
                ssid = manualSsid ?: currentSsid()
                pass = wifiPassphrase().ifEmpty { "12345678" }
                ip = manualIp
                cachedSsid = ssid; cachedPass = pass; cachedIp = ip
                hotspotReady = true
                android.util.Log.i("AllPlay", "Manual hotspot detected: ssid=$ssid ip=$ip passLen=${pass.length}")
            } else if (hotspotReady) {
                // Rotation restart: reuse the hotspot already open instead of reopening it (saves 15s)
                ssid = cachedSsid; pass = cachedPass; ip = cachedIp
                android.util.Log.i("AllPlay", "Reusing open hotspot ssid=$ssid")
            } else if (hotspot.start()) {
                ssid = hotspot.ssid; pass = hotspot.passphrase; ip = hotspot.ip
                cachedSsid = ssid; cachedPass = pass; cachedIp = ip
                hotspotReady = true
                android.util.Log.i("AllPlay", "Hotspot opened: ssid=$ssid ip=$ip")
            } else {
                android.util.Log.w("AllPlay", "Hotspot start failed, falling back to same-network mode ssid=$ssid")
                ssid = currentSsid(); pass = wifiPassphrase()
            }
            mdns.advertiseIp = ip   // hotspot interface IP: makes JmDNS bind to the AP interface for advertising
            val fps = maxFps()
            val port = native.nativeStart(
                bt, mdns, video, audio,
                stateDir, identityDir, "AllPlay",
                bluetoothMac(), ssid, pass, ip,
                w, h,
                false,   // Rotation reconnects must also go through the Bluetooth trigger: carplay-probe alone is not enough in practice, and the iPhone will not reconnect on its own
                fps,
            )
            android.util.Log.i("AllPlay", "nativeStart -> port=$port resolution=${w}x${h} restart=$restart fps=$fps")
            startCarPlayProbe()
        }.start()
    }

    /// Orientation changed after rotation: stop the old session and restart with the new resolution (CarPlay must renegotiate the resolution; the iPhone reconnects briefly)
    fun restartForOrientation() {
        if (!started) return
        // Debounce: do not restart within 3s, absorbing the flurry when returning to foreground/rotating
        val now = System.currentTimeMillis()
        if (now - lastRestartMs < 3000) return
        val (w, h) = resolutionForOrientation()
        if (w == nativeWidth && h == nativeHeight) return
        lastRestartMs = now
        android.util.Log.i("AllPlay", "Orientation changed, restarting session ${nativeWidth}x${nativeHeight} → ${w}x${h}")
        native.nativeStop()
        startNative(restart = true)
    }

    fun attachSurface(surface: Surface) {
        video.setSurface(surface)
        // After the Surface is rebuilt on returning to foreground, request a keyframe immediately to restore the picture ASAP (do not wait for the 1s rate limit or the phone's natural IDR)
        native.nativeRequestKeyFrame()
    }

    /// Surface destroyed on backgrounding: make the video decoder drop its reference to the dead Surface to avoid configure/render crashes
    fun releaseSurface() {
        video.releaseSurface()
    }

    fun sendTouch(points: FloatArray) {
        native.nativeSendTouch(points)
    }

    /// Expose the video decoder to MainActivity for live frame-rate calculation
    val videoSink: MediaCodecSink get() = video

    /// Session state: 0=waiting for connection, 1=connecting, 2=connected
    fun sessionState(): Int = if (started) native.nativeSessionState() else 0

    /// Restart the session after the network config is saved (re-reads the files config)
    fun restartNative() {
        if (!started) return
        android.util.Log.i("AllPlay", "Network config changed, restarting session")
        native.nativeStop()
        startNative(restart = true)
    }

    @SuppressLint("MissingPermission", "HardwareIds")
    private fun bluetoothMac(): String {
        // Huawei masks the WifiManager/BluetoothAdapter values, so allow an adb-pushed file to override them (see files/btmac)
        val manual = try { java.io.File(filesDir, "btmac").readText().trim() } catch (e: Exception) { "" }
        if (manual.isNotEmpty()) return manual
        val a = BluetoothAdapter.getDefaultAdapter()?.address ?: ""
        if (a.isNotEmpty() && !a.startsWith("02:00:00:00:00:00")) return a
        return try { Settings.Secure.getString(contentResolver, "bluetooth_address") ?: a } catch (e: Exception) { a }
    }

    @SuppressLint("MissingPermission")
    private fun currentSsid(): String {
        // Huawei masks it, so allow an adb-pushed file to override it (see files/ssid)
        val manual = try { java.io.File(filesDir, "ssid").readText().trim() } catch (e: Exception) { "" }
        if (manual.isNotEmpty()) return manual
        return try {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wm.connectionInfo?.ssid?.removeSurrounding("\"") ?: ""
        } catch (e: Exception) { "" }
    }

    /// Wi-Fi passphrase: read from files/passphrase (same-network mode; WPA2 needs a passphrase)
    private fun wifiPassphrase(): String =
        try { java.io.File(filesDir, "passphrase").readText().trim() } catch (e: Exception) { "" }

    /// CarPlay frame-rate cap: overridden by files/fps, default 60; forced to 30 in smooth mode
    private fun maxFps(): Int {
        if (smoothMode) return 30
        val s = try { java.io.File(filesDir, "fps").readText().trim() } catch (e: Exception) { "" }
        return s.toIntOrNull()?.coerceIn(15, 60) ?: 60
    }

    /// Smooth-mode marker file: files/smooth exists = halved resolution + 30fps
    private val smoothFile get() = java.io.File(filesDir, "smooth")
    private val smoothMode get() = smoothFile.exists()

    /// Lets MainActivity check whether smooth mode is currently active (for dialog text/buttons)
    val smoothModeActive: Boolean get() = smoothMode

    /// Enter smooth mode: write the marker → restart the session with halved resolution + 30fps
    fun enableSmoothMode() {
        try { smoothFile.createNewFile() } catch (e: Exception) {}
        android.util.Log.i("AllPlay", "Entering smooth mode (halved resolution + 30fps), restarting session")
        native.nativeStop()
        startNative(restart = true)
    }

    /// Exit smooth mode: delete the marker → restore normal resolution + 60fps
    fun disableSmoothMode() {
        try { smoothFile.delete() } catch (e: Exception) {}
        android.util.Log.i("AllPlay", "Exiting smooth mode, restoring normal resolution + 60fps")
        native.nativeStop()
        startNative(restart = true)
    }

    /// Discover the iPhone's _carplay-ctrl._tcp and probe /ctrl-int/1/connect to make the iPhone connect over RTSP
    private fun startCarPlayProbe() {
        Thread {
            try { Thread.sleep(2000) } catch (e: Exception) {}   // let the Bluetooth trigger run first
            val deviceId = native.nativeDeviceId()
            val resolved = java.util.concurrent.atomic.AtomicReference<Pair<String, Int>?>(null)
            mdns.discoverCarPlayCtrl { host, port -> resolved.set(host to port) }
            for (i in 0 until 20) {
                val r = resolved.getAndSet(null)
                if (r != null) {
                    val ok = mdns.probeCarPlay(r.first, r.second, deviceId, "370.10")
                    android.util.Log.i("AllPlay", "carplay probe ${r.first}:${r.second} -> $ok")
                }
                try { Thread.sleep(1000) } catch (e: Exception) {}
            }
        }.start()
    }

    override fun onDestroy() {
        if (started) native.nativeStop()
        mdns.unregister()
        try { wakeLock?.release() } catch (_: Exception) {}
        wakeLock = null
        if ((application as AllPlayApp).service === this) (application as AllPlayApp).service = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun buildNotification(): Notification {
        if (Build.VERSION.SDK_INT >= 26) {
            val ch = NotificationChannel("allplay", "AllPlay Casting", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
            return Notification.Builder(this, "allplay")
                .setContentTitle("AllPlay")
                .setContentText("Waiting for iPhone connection…")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .build()
        }
        // Android 4.4: no NotificationChannel, use the old constructor
        @Suppress("DEPRECATION")
        return Notification.Builder(this)
            .setContentTitle("AllPlay")
            .setContentText("Waiting for iPhone connection…")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .build()
    }
}
