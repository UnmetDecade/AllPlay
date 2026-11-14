package com.allplay.app

import android.annotation.SuppressLint
import android.content.Context
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.random.Random

/**
 * Android implementation of IHotspot. Prefer a **persistent SoftAP** (reflected WifiManager.setWifiApEnabled; not reclaimed in the background),
 * falling back to startLocalOnlyHotspot on failure (works in the foreground, reclaimed by the system in the background).
 */
class HotspotManager(context: Context) {
    @Volatile var ssid: String = ""; private set
    @Volatile var passphrase: String = ""; private set
    @Volatile var ip: String = ""; private set
    @Volatile var persistent: Boolean = false; private set

    private val wifiManager: WifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val filesDir = context.filesDir
    // LocalOnlyHotspotReservation is API 26; use Any? so 4.4 does not reference a missing type when loading the class
    private var reservation: Any? = null

    /// Start the hotspot (blocks until ready/failure). Prefer persistent SoftAP, fall back to LocalOnlyHotspot.
    @SuppressLint("MissingPermission")
    fun start(timeoutMs: Long = 15000): Boolean {
        val (s, p) = stableCredentials()
        if (startPersistentAp(s, p)) {
            ssid = s; passphrase = p; persistent = true
            ip = detectHotspotIp() ?: "192.168.43.1"
            Log.i("AllPlay", "Persistent SoftAP opened: ssid=$s ip=$ip")
            return true
        }
        return startLocalOnly(timeoutMs)
    }

    /// Stable SSID/passphrase: files/ap_ssid and files/ap_passphrase override, or are generated on first use and persisted (so the iPhone can reconnect without re-entering them)
    private fun stableCredentials(): Pair<String, String> {
        val s = readText("ap_ssid").ifBlank {
            "AllPlay-${Random.nextInt(1000, 10000)}".also { writeText("ap_ssid", it) }
        }
        val p = readText("ap_passphrase").ifBlank {
            val chars = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789"
            (1..10).map { chars[Random.nextInt(chars.length)] }.joinToString("")
                .also { writeText("ap_passphrase", it) }
        }
        return s to p
    }

    /// Create a real SoftAP via reflection (not reclaimed in the background). Android 13+ uses startSoftAp(SoftApConfiguration); older versions use setWifiApEnabled.
    @SuppressLint("MissingPermission")
    private fun startPersistentAp(ssid: String, pass: String): Boolean {
        // Android 13+: startSoftAp(SoftApConfiguration) (hidden API, reflected)
        if (Build.VERSION.SDK_INT >= 33) {
            try {
                val builderClass = Class.forName("android.net.wifi.SoftApConfiguration\$Builder")
                val builder = builderClass.getConstructor().newInstance()
                builderClass.getMethod("setSsid", String::class.java).invoke(builder, ssid)
                // setPassphrase(String, int): securityType 2 = WPA2_PSK
                builderClass.getMethod("setPassphrase", String::class.java, Int::class.javaPrimitiveType)
                    .invoke(builder, pass, 2)
                val config = builderClass.getMethod("build").invoke(builder)
                val configClass = Class.forName("android.net.wifi.SoftApConfiguration")
                val m = wifiManager.javaClass.getMethod("startSoftAp", configClass)
                return m.invoke(wifiManager, config) as Boolean
            } catch (e: Exception) {
                Log.w("AllPlay", "startSoftAp reflection failed: ${e.message}")
            }
        }
        // Android 10 and below: setWifiApEnabled(WifiConfiguration, boolean)
        return try {
            val cfg = WifiConfiguration().apply {
                SSID = "\"$ssid\""
                preSharedKey = "\"$pass\""
                allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK)
            }
            val m = wifiManager.javaClass.getMethod(
                "setWifiApEnabled", WifiConfiguration::class.java, Boolean::class.javaPrimitiveType,
            )
            m.invoke(wifiManager, cfg, true) as Boolean
        } catch (e: Exception) {
            val c = e.cause ?: e
            Log.w("AllPlay", "setWifiApEnabled reflection failed: ${c.javaClass.name}: ${c.message}", c)
            false
        }
    }

    /// Fallback: LocalOnlyHotspot (works in the foreground, reclaimed by the system in the background). API 26+ only; fails outright on 4.4.
    @SuppressLint("MissingPermission")
    private fun startLocalOnly(timeoutMs: Long): Boolean {
        if (Build.VERSION.SDK_INT < 26) return false
        val latch = CountDownLatch(1)
        var ok = false
        val handler = Handler(Looper.getMainLooper())
        wifiManager.startLocalOnlyHotspot(object : WifiManager.LocalOnlyHotspotCallback() {
            override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                reservation = res
                val cfg = res.wifiConfiguration
                ssid = cfg?.SSID?.removeSurrounding("\"") ?: ""
                passphrase = cfg?.preSharedKey ?: ""
                ip = detectHotspotIp() ?: "192.168.43.1"
                persistent = false
                ok = true
                Log.i("AllPlay", "LocalOnlyHotspot: ssid=$ssid password=$passphrase ip=$ip")
                latch.countDown()
            }
            override fun onStopped() { latch.countDown() }
            override fun onFailed(reason: Int) { latch.countDown() }
        }, handler)
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return ok
    }

    fun stop() {
        // reservation is the API 26 LocalOnlyHotspotReservation; call close() via reflection so 4.4 does not reference a missing class
        try {
            reservation?.let { r -> r.javaClass.getMethod("close").invoke(r) }
        } catch (_: Exception) {}
        reservation = null
        // Leave the persistent SoftAP running: keep the hotspot across backgrounding/service recreation (CarPlay stays resident)
    }

    /// Read the SSID of a system hotspot the user turned on manually (Android 11+ getSoftApConfiguration by reflection; older getWifiApConfiguration).
    /// Returns null when no manual hotspot is detected.
    @SuppressLint("PrivateApi", "MissingPermission")
    fun readManualApSsid(): String? {
        return try {
            if (Build.VERSION.SDK_INT >= 30) {
                val m = WifiManager::class.java.getMethod("getSoftApConfiguration")
                val cfg = m.invoke(wifiManager) ?: return null
                cfg.javaClass.getMethod("getSsid").invoke(cfg) as? String
            } else {
                val m = WifiManager::class.java.getMethod("getWifiApConfiguration")
                val cfg = m.invoke(wifiManager) as? WifiConfiguration ?: return null
                cfg.SSID?.removeSurrounding("\"")
            }
        } catch (e: Exception) {
            val c = e.cause ?: e
            Log.w("AllPlay", "readManualApSsid failed: ${c.javaClass.name}: ${c.message}")
            null
        }
    }

    /// Manual hotspot interface IP (ap*/softap*); null if none
    fun manualHotspotIp(): String? = detectHotspotIp()

    /// IPv4 on a hotspot interface (ap*/softap*/wlan1*); falls back to the Android SoftAP default gateway if none is found
    private fun detectHotspotIp(): String? {
        return try {
            val ifs = NetworkInterface.getNetworkInterfaces()
            while (ifs.hasMoreElements()) {
                val ni = ifs.nextElement()
                val n = ni.name.lowercase()
                if (n.startsWith("ap") || n.startsWith("softap") || n.startsWith("wlan1")) {
                    for (a in ni.inetAddresses) {
                        if (a is Inet4Address && !a.isLoopbackAddress) return a.hostAddress
                    }
                }
            }
            null
        } catch (e: Exception) { null }
    }

    private fun readText(name: String): String =
        try { java.io.File(filesDir, name).readText().trim() } catch (e: Exception) { "" }

    private fun writeText(name: String, value: String) {
        try { java.io.File(filesDir, name).writeText(value) } catch (e: Exception) {}
    }
}
