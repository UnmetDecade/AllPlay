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
 * 前台服务：常驻承载 native core（Receiver）。蓝牙触发 → 配对 → 投屏都在这里。
 * MainActivity 只负责 Surface 与启动/停止本服务。
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
        ensureIdentity()   // 首启把打包进 APK 的 MFi 身份（identity.pk8 + certificate.p7b）落到 files/identity
        val app = application as AllPlayApp
        app.service = this
        native = NativeAllPlay()
        bt = BluetoothService()
        mdns = MdnsService(this)
        video = MediaCodecSink()
        audio = AudioSink()
        hotspot = HotspotManager(this)
        // 后台保活：拿部分唤醒锁，防止退后台后 CPU/网络休眠导致 CarPlay 断连
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AllPlay:CarPlay").apply { acquire() }
        // Surface 可能先于本服务就绪（MainActivity surfaceCreated 早于服务创建），补传一次
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

    /// 首启把打包进 APK 的 MFi 身份落到 files/identity（identity.pk8 + certificate.p7b）。
    /// 已存在就不动，所以之后 adb push 覆盖的文件优先于 APK 内置的这份。
    private fun ensureIdentity() {
        val dir = java.io.File(filesDir, "identity")
        val key = java.io.File(dir, "identity.pk8")
        val cert = java.io.File(dir, "certificate.p7b")
        if (key.exists() && cert.exists()) return
        dir.mkdirs()
        try {
            if (!key.exists()) copyAsset("identity/identity.pk8", key)
            if (!cert.exists()) copyAsset("identity/certificate.p7b", cert)
            android.util.Log.i("AllPlay", "MFi 身份已从 APK 落到 files/identity")
        } catch (e: Exception) {
            android.util.Log.e("AllPlay", "MFi 身份落地失败，CarPlay 认证会过不了", e)
        }
    }

    private fun copyAsset(name: String, dest: java.io.File) {
        assets.open(name).use { input ->
            java.io.FileOutputStream(dest).use { output ->
                input.copyTo(output)
            }
        }
    }

    /// 屏幕方向：默认横屏（宽 > 高）；files/portrait 存在则竖屏（宽 < 高）。
    /// 流畅模式：分辨率减半（像素降到 1/4，比例不变）。
    private fun resolutionForOrientation(): Pair<Int, Int> {
        val portraitMode = java.io.File(filesDir, "portrait").exists()
        // 性能模式：固定 1280x720 @ 30fps（不再分辨率减半）
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
        val identityDir = "$stateDir/identity"   // 由安装/首启时放入 identity.pk8 + certificate.p7b
        // 热点模式：后台线程开热点 → 用热点 SSID/口令/IP 启动（避免阻塞主线程）
        // 局域网模式：files/lan 存在则跳过热点，直接用当前 WiFi（iPhone 与平板同网）
        val lanMode = java.io.File(filesDir, "lan").exists()
        Thread {
            android.util.Log.i("AllPlay", "startNative 线程开始")
            var ssid = ""; var pass = ""; var ip = ""
            // 先检测用户手动开的系统热点（vivo 上 app 自建热点被 ROM 封，手动热点反而能用）
            val manualSsid = if (!lanMode) hotspot.readManualApSsid() else null
            val manualIp = if (!lanMode) hotspot.manualHotspotIp() else null
            android.util.Log.i("AllPlay", "手动热点检测：ssid=$manualSsid ip=$manualIp")
            if (lanMode) {
                ssid = currentSsid()
                pass = wifiPassphrase().ifEmpty { "12345678" }   // 占位密码→securityType=WPA2，iPhone 已连 WiFi 不需真密码
                android.util.Log.i("AllPlay", "LAN 模式：跳过热点，用当前网络 ssid=$ssid")
            } else if (manualIp != null) {
                // 手动热点激活：SSID 优先读系统(getSoftApConfiguration，需签名权限)，读不到退回 files/ssid
                ssid = manualSsid ?: currentSsid()
                pass = wifiPassphrase().ifEmpty { "12345678" }
                ip = manualIp
                cachedSsid = ssid; cachedPass = pass; cachedIp = ip
                hotspotReady = true
                android.util.Log.i("AllPlay", "检测到手动热点：ssid=$ssid ip=$ip passLen=${pass.length}")
            } else if (hotspotReady) {
                // 旋转重启：复用已开的热点，别重复开（省 15s）
                ssid = cachedSsid; pass = cachedPass; ip = cachedIp
                android.util.Log.i("AllPlay", "复用已开热点 ssid=$ssid")
            } else if (hotspot.start()) {
                ssid = hotspot.ssid; pass = hotspot.passphrase; ip = hotspot.ip
                cachedSsid = ssid; cachedPass = pass; cachedIp = ip
                hotspotReady = true
                android.util.Log.i("AllPlay", "热点已开：ssid=$ssid ip=$ip")
            } else {
                android.util.Log.w("AllPlay", "热点启动失败，退回同网模式 ssid=$ssid")
                ssid = currentSsid(); pass = wifiPassphrase()
            }
            mdns.advertiseIp = ip   // 热点接口 IP：让 JmDNS 绑定到 AP 接口广播
            val fps = maxFps()
            val port = native.nativeStart(
                bt, mdns, video, audio,
                stateDir, identityDir, "AllPlay",
                bluetoothMac(), ssid, pass, ip,
                w, h,
                false,   // 旋转重连也必须走蓝牙触发：实测 carplay-probe 单独不够，iPhone 不会自动重连
                fps,
            )
            android.util.Log.i("AllPlay", "nativeStart -> port=$port 分辨率=${w}x${h} restart=$restart fps=$fps")
            startCarPlayProbe()
        }.start()
    }

    /// 旋转后方向变了：停旧会话，用新分辨率重启（CarPlay 分辨率需重新协商，iPhone 会短暂重连）
    fun restartForOrientation() {
        if (!started) return
        // 防抖：3s 内不重复重启，吞掉回前台/旋转瞬间的抖动
        val now = System.currentTimeMillis()
        if (now - lastRestartMs < 3000) return
        val (w, h) = resolutionForOrientation()
        if (w == nativeWidth && h == nativeHeight) return
        lastRestartMs = now
        android.util.Log.i("AllPlay", "方向变化，重启会话 ${nativeWidth}x${nativeHeight} → ${w}x${h}")
        native.nativeStop()
        startNative(restart = true)
    }

    fun attachSurface(surface: Surface) {
        video.setSurface(surface)
        // 回前台 Surface 重建后立即请一次关键帧，尽快恢复画面（别等 1s 限频或手机自然 IDR）
        native.nativeRequestKeyFrame()
    }

    /// 退后台 Surface 销毁：让视频解码器释放对死 Surface 的引用，避免 configure/渲染崩溃
    fun releaseSurface() {
        video.releaseSurface()
    }

    fun sendTouch(points: FloatArray) {
        native.nativeSendTouch(points)
    }

    /// 暴露视频解码器给 MainActivity 算实时帧率
    val videoSink: MediaCodecSink get() = video

    /// 会话状态：0=等待连接，1=连接中，2=已连接
    fun sessionState(): Int = if (started) native.nativeSessionState() else 0

    /// 保存网络配置后重启会话（重新读 files 配置）
    fun restartNative() {
        if (!started) return
        android.util.Log.i("AllPlay", "网络配置已改，重启会话")
        native.nativeStop()
        startNative(restart = true)
    }

    @SuppressLint("MissingPermission", "HardwareIds")
    private fun bluetoothMac(): String {
        // 华为打码 WifiManager/BluetoothAdapter 的值，允许 adb push 文件覆盖（见 files/btmac）
        val manual = try { java.io.File(filesDir, "btmac").readText().trim() } catch (e: Exception) { "" }
        if (manual.isNotEmpty()) return manual
        val a = BluetoothAdapter.getDefaultAdapter()?.address ?: ""
        if (a.isNotEmpty() && !a.startsWith("02:00:00:00:00:00")) return a
        return try { Settings.Secure.getString(contentResolver, "bluetooth_address") ?: a } catch (e: Exception) { a }
    }

    @SuppressLint("MissingPermission")
    private fun currentSsid(): String {
        // 华为打码，允许 adb push 文件覆盖（见 files/ssid）
        val manual = try { java.io.File(filesDir, "ssid").readText().trim() } catch (e: Exception) { "" }
        if (manual.isNotEmpty()) return manual
        return try {
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wm.connectionInfo?.ssid?.removeSurrounding("\"") ?: ""
        } catch (e: Exception) { "" }
    }

    /// Wi-Fi 口令：从 files/passphrase 读（同网模式，WPA2 需口令）
    private fun wifiPassphrase(): String =
        try { java.io.File(filesDir, "passphrase").readText().trim() } catch (e: Exception) { "" }

    /// CarPlay 帧率上限：files/fps 覆盖，默认 60；流畅模式下强制 30
    private fun maxFps(): Int {
        if (smoothMode) return 30
        val s = try { java.io.File(filesDir, "fps").readText().trim() } catch (e: Exception) { "" }
        return s.toIntOrNull()?.coerceIn(15, 60) ?: 60
    }

    /// 流畅模式标记文件：files/smooth 存在 = 分辨率减半 + 30fps
    private val smoothFile get() = java.io.File(filesDir, "smooth")
    private val smoothMode get() = smoothFile.exists()

    /// 供 MainActivity 判断当前是否已在流畅模式（弹窗文案/按钮用）
    val smoothModeActive: Boolean get() = smoothMode

    /// 进入流畅模式：写标记 → 用减半分辨率 + 30fps 重启会话
    fun enableSmoothMode() {
        try { smoothFile.createNewFile() } catch (e: Exception) {}
        android.util.Log.i("AllPlay", "进入流畅模式（分辨率减半 + 30fps），重启会话")
        native.nativeStop()
        startNative(restart = true)
    }

    /// 退出流畅模式：删标记 → 恢复正常分辨率 + 60fps
    fun disableSmoothMode() {
        try { smoothFile.delete() } catch (e: Exception) {}
        android.util.Log.i("AllPlay", "退出流畅模式，恢复正常分辨率 + 60fps")
        native.nativeStop()
        startNative(restart = true)
    }

    /// 发现 iPhone 的 _carplay-ctrl._tcp 并探测 /ctrl-int/1/connect，促使 iPhone 连 RTSP
    private fun startCarPlayProbe() {
        Thread {
            try { Thread.sleep(2000) } catch (e: Exception) {}   // 等蓝牙触发先跑
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
            val ch = NotificationChannel("allplay", "AllPlay 投屏", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
            return Notification.Builder(this, "allplay")
                .setContentTitle("AllPlay")
                .setContentText("等待 iPhone 连接…")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .build()
        }
        // Android 4.4：无 NotificationChannel，用旧构造器
        @Suppress("DEPRECATION")
        return Notification.Builder(this)
            .setContentTitle("AllPlay")
            .setContentText("等待 iPhone 连接…")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .build()
    }
}
