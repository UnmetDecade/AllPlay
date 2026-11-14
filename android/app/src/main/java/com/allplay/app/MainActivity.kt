package com.allplay.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.EditText
import android.widget.CheckBox
import android.widget.Button
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.ArrayAdapter

/**
 * Entry point: request permissions → start the foreground service → three-state UI (waiting for connection / connecting / projection).
 * Pass the Surface to the service (→ native → MediaCodec).
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var serviceIntent: Intent
    private var surfaceReady = false

    private lateinit var surfaceView: SurfaceView
    private lateinit var launcherView: FrameLayout
    private lateinit var statusText: TextView
    private lateinit var btBadge: FrameLayout
    private lateinit var fpsText: TextView

    private val uiHandler = Handler(Looper.getMainLooper())
    private var lastRenderedFrames = 0
    private var lastState = -1
    private var dotStep = 0

    // Three-dot animation after the "waiting for connection" text
    private val dotsRunnable = object : Runnable {
        override fun run() {
            if (lastState == 0 && launcherView.visibility == View.VISIBLE) {
                val base = "Waiting for connection"
                val dots = when (dotStep % 4) { 0 -> ""; 1 -> "."; 2 -> ".."; else -> "..." }
                statusText.text = base + dots
                dotStep++
            }
            uiHandler.postDelayed(this, 700)
        }
    }

    // Live frame rate (top-right badge, refreshed every second)
    private val fpsRunnable = object : Runnable {
        override fun run() {
            val cur = (application as? AllPlayApp)?.service?.videoSink?.renderedFrames ?: 0
            val fps = cur - lastRenderedFrames
            lastRenderedFrames = cur
            fpsText.text = "$fps fps"
            uiHandler.postDelayed(this, 1000)
        }
    }

    // Session state polling: 0 waiting / 1 connecting / 2 connected
    private val stateRunnable = object : Runnable {
        override fun run() {
            val s = (application as? AllPlayApp)?.service?.sessionState() ?: 0
            if (s != lastState) {
                lastState = s
                updateUiState(s)
            }
            uiHandler.postDelayed(this, 500)
        }
    }

    private fun updateUiState(state: Int) {
        when (state) {
            2 -> {   // connected → show the projection, hide the launch screen
                launcherView.visibility = View.GONE
            }
            1 -> {   // connecting
                launcherView.visibility = View.VISIBLE
                statusText.text = "Connecting…"
            }
            else -> {  // waiting for connection
                launcherView.visibility = View.VISIBLE
                statusText.text = "Waiting for connection"
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Full-screen immersive: fill the notch area too
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener { v, e -> onTouch(v, e) }

        // Launch/connecting screen: white background + branding + Bluetooth ripple + status text
        launcherView = FrameLayout(this)
        launcherView.setBackgroundColor(Color.WHITE)

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
        }
        val title = TextView(this).apply {
            // Two-tone logo: "All" in dark + "Play" in blue, to avoid monotony
            val sp = android.text.SpannableString("AllPlay")
            sp.setSpan(android.text.style.ForegroundColorSpan(Color.parseColor("#1A1A1A")), 0, 3,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sp.setSpan(android.text.style.ForegroundColorSpan(Color.parseColor("#2F7CF6")), 3, 7,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sp
            textSize = 38f
            typeface = Typeface.create("sans-serif", Typeface.BOLD)
            if (Build.VERSION.SDK_INT >= 21) letterSpacing = 0.04f   // setLetterSpacing is API 21
            gravity = Gravity.CENTER
        }
        // Bluetooth ripple badge: ripple rings + blue circle base + Bluetooth icon
        btBadge = FrameLayout(this)
        val ripple = RippleView(this)
        btBadge.addView(ripple, FrameLayout.LayoutParams(dp(168), dp(168), Gravity.CENTER))
        val badgeBg = View(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(Color.parseColor("#2F7CF6"))
            }
        }
        btBadge.addView(badgeBg, FrameLayout.LayoutParams(dp(84), dp(84), Gravity.CENTER))
        val btIcon = ImageView(this).apply {
            setImageResource(R.drawable.bluetooth_icon)
        }
        btBadge.addView(btIcon, FrameLayout.LayoutParams(dp(52), dp(52), Gravity.CENTER))

        statusText = TextView(this).apply {
            text = "Waiting for connection"
            setTextColor(Color.parseColor("#666666"))
            textSize = 19f
            gravity = Gravity.CENTER
        }

        content.addView(title, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        content.addView(btBadge, LinearLayout.LayoutParams(
            dp(168), dp(168)).apply {
            topMargin = dp(44)
        })
        content.addView(statusText, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(32)
        })
        launcherView.addView(content, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.CENTER))

        // Settings icon in the top-left
        val settingsBtn = ImageView(this).apply {
            setImageResource(R.drawable.settings_icon)
            setPadding(dp(2), dp(2), dp(2), dp(2))
            setOnClickListener { showSettingsDialog() }
        }
        launcherView.addView(settingsBtn, FrameLayout.LayoutParams(
            dp(60), dp(60), Gravity.TOP or Gravity.START))

        // FPS badge: live frame rate in the top-right (display only)
        fpsText = TextView(this).apply {
            setTextColor(Color.GREEN)
            textSize = 20f
            gravity = Gravity.CENTER
            setPadding(dp(16), dp(8), dp(16), dp(8))
            setBackgroundColor(Color.argb(150, 0, 0, 0))
            text = "-- fps"
        }

        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(launcherView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(fpsText, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.END))
        setContentView(root)
        hideSystemBars()
        // Show the FPS badge according to the setting
        fpsText.visibility = if (java.io.File(filesDir, "fps_show").exists()) View.VISIBLE else View.GONE

        serviceIntent = Intent(this, AllPlaySessionService::class.java)
        maybeRequestPermissions()

        uiHandler.post(fpsRunnable)
        uiHandler.post(stateRunnable)
        uiHandler.post(dotsRunnable)
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /// Touch → normalize to 0..1 → at most 2 contacts → [x,y,down] × 2 → send to the event channel (per the protocol's sendTouch)
    private fun onTouch(v: View, e: MotionEvent): Boolean {
        val service = (application as? AllPlayApp)?.service ?: return false
        if (v.width == 0 || v.height == 0) return true
        val pts = FloatArray(6)   // 2 contacts × 3
        val count = minOf(e.pointerCount, 2)
        for (i in 0 until count) {
            pts[i * 3] = e.getX(i) / v.width.toFloat()
            pts[i * 3 + 1] = e.getY(i) / v.height.toFloat()
            val up = e.actionMasked == MotionEvent.ACTION_UP ||
                (e.actionMasked == MotionEvent.ACTION_POINTER_UP && e.actionIndex == i)
            pts[i * 3 + 2] = if (up) 0f else 1f
        }
        service.sendTouch(pts)
        return true
    }

    private fun maybeRequestPermissions() {
        // Android 4.4 (<23): dangerous permissions are granted at install with no runtime model, so start the service directly
        if (Build.VERSION.SDK_INT < 23) {
            startService()
            return
        }
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            perms += listOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        }
        if (Build.VERSION.SDK_INT >= 33) perms += Manifest.permission.NEARBY_WIFI_DEVICES
        if (Build.VERSION.SDK_INT <= 32) perms += Manifest.permission.ACCESS_FINE_LOCATION
        val missing = perms.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 1)
        } else {
            startService()
            requestBatteryExemption()
        }
    }

    override fun onRequestPermissionsResult(code: Int, p: Array<out String>, r: IntArray) {
        startService()
        requestBatteryExemption()
    }

    /// Request a battery optimization exemption (so Doze does not kill the process or drop the hotspot in the background; the user must tap Allow in the system dialog)
    private fun requestBatteryExemption() {
        if (Build.VERSION.SDK_INT < 23) return   // Android 4.4 has no battery optimization concept
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (e: Exception) {}
    }

    private fun startService() {
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(serviceIntent)
        else startService(serviceIntent)   // Android 4.4 has no startForegroundService
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        attachToService(holder.surface)
    }

    private fun attachToService(surface: android.view.Surface) {
        val app = application as? AllPlayApp ?: return
        app.surface = surface
        app.service?.attachSurface(surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, f: Int, w: Int, h: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        // Surface destroyed on backgrounding: tell the service to drop the decoder's reference to the dead Surface, so a crash does not drop CarPlay
        (application as? AllPlayApp)?.service?.releaseSurface()
    }

    /// Rotation (configChanges declares orientation|screenSize): when the orientation changes, have the service restart the session at the new resolution
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        (application as? AllPlayApp)?.service?.restartForOrientation()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Immersive mode is exited on losing focus; hide again on regaining focus so it stays fullscreen when returning to foreground
        if (hasFocus) hideSystemBars()
    }

    /// Manual smooth-mode toggle dialog (triggered by tapping the top-right FPS badge)
    private fun showSmoothModeDialog() {
        val svc = (application as? AllPlayApp)?.service ?: return
        val active = svc.smoothModeActive
        val builder = AlertDialog.Builder(this).setCancelable(false)
        if (active) {
            builder.setTitle("Exit smooth mode")
                .setMessage("Smooth mode is active (1280x720 + 30fps).\nRestore normal resolution?")
                .setPositiveButton("Restore") { _, _ -> svc.disableSmoothMode() }
                .setNegativeButton("Keep smooth mode", null)
        } else {
            builder.setTitle("Enter smooth mode")
                .setMessage("Lock to 1280x720 + 30fps max to reduce decoding load and dropped frames.")
                .setPositiveButton("Enter smooth mode") { _, _ -> svc.enableSmoothMode() }
                .setNegativeButton("Cancel", null)
        }
        builder.show()
    }

    /// Settings dialog (triggered by the top-left gear): pick a mode + enter SSID/passphrase + smooth-mode toggle; takes effect after saving and restarting
    private fun showSettingsDialog() {
        val files = filesDir
        val lanFile = java.io.File(files, "lan")
        val ssidFile = java.io.File(files, "ssid")
        val passFile = java.io.File(files, "passphrase")
        val smoothFile = java.io.File(files, "smooth")
        val portraitFile = java.io.File(files, "portrait")
        val fpsFile = java.io.File(files, "fps_show")

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(20), dp(24), dp(8))
        }
        val modeLabel = TextView(this).apply {
            text = "Wireless connection mode"
            setTextColor(Color.parseColor("#333333"))
            textSize = 15f
        }
        val modeSpinner = Spinner(this)
        val modes = arrayOf("Hotspot mode (phone connects to this device)", "LAN mode (connect to router WiFi)")
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, modes)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        modeSpinner.adapter = adapter
        modeSpinner.setSelection(if (lanFile.exists()) 1 else 0)

        val orientLabel = TextView(this).apply {
            text = "Screen orientation"
            setTextColor(Color.parseColor("#333333"))
            textSize = 15f
        }
        val orientSpinner = Spinner(this)
        val orientModes = arrayOf("Landscape (default)", "Portrait")
        val orientAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, orientModes)
        orientAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        orientSpinner.adapter = orientAdapter
        orientSpinner.setSelection(if (portraitFile.exists()) 1 else 0)

        val ssidEdit = EditText(this).apply {
            hint = "SSID / hotspot name"
            setText(if (ssidFile.exists()) ssidFile.readText().trim() else "")
        }
        val passEdit = EditText(this).apply {
            hint = "Password"
            setText(if (passFile.exists()) passFile.readText().trim() else "")
        }
        val smoothCheck = CheckBox(this).apply {
            text = "Smooth mode"
            setTextColor(Color.parseColor("#333333"))
            isChecked = smoothFile.exists()
        }
        val fpsCheck = CheckBox(this).apply {
            text = "Show FPS"
            setTextColor(Color.parseColor("#333333"))
            isChecked = fpsFile.exists()
        }

        layout.addView(modeLabel)
        layout.addView(modeSpinner)
        layout.addView(ssidEdit, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(passEdit, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(10)
        })
        layout.addView(orientLabel, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(orientSpinner)
        layout.addView(smoothCheck, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(fpsCheck, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(8)
        })

        val scroll = ScrollView(this).apply { addView(layout) }

        AlertDialog.Builder(this)
            .setTitle("Settings")
            .setView(scroll)
            .setPositiveButton("Save") { _, _ ->
                try {
                    // 0 = hotspot mode (delete lan), 1 = LAN mode (create lan)
                    if (modeSpinner.selectedItemPosition == 1) lanFile.createNewFile() else lanFile.delete()
                    // 0 = landscape, 1 = portrait
                    if (orientSpinner.selectedItemPosition == 1) portraitFile.createNewFile() else portraitFile.delete()
                    ssidFile.writeText(ssidEdit.text.toString().trim())
                    passFile.writeText(passEdit.text.toString())
                    if (smoothCheck.isChecked) smoothFile.createNewFile() else smoothFile.delete()
                    if (fpsCheck.isChecked) fpsFile.createNewFile() else fpsFile.delete()
                } catch (e: Exception) {}
                fpsText.visibility = if (fpsCheck.isChecked) View.VISIBLE else View.GONE
                (application as? AllPlayApp)?.service?.restartNative()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            // On some devices (e.g. vivo), insetsController may be null before setContentView and throw an NPE; catch it as a fallback
            try {
                val c = window.insetsController
                c?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                c?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            } catch (e: Exception) {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
        }
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(fpsRunnable)
        uiHandler.removeCallbacks(stateRunnable)
        uiHandler.removeCallbacks(dotsRunnable)
        super.onDestroy()
    }
}
