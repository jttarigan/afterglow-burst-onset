package org.afterglow.app

import android.app.Activity
import android.app.GameManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.provider.Settings
import android.util.Log
import android.view.Choreographer
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.system.exitProcess

/**
 * Hosts one lab run per launch, from the "args" intent extra:
 *   adb shell am start -n org.afterglow.app/.MainActivity --es args "--mode=stepbench ..."
 * A render thread paced by its own Choreographer drives NativeBridge.frame; host-only facts go to
 * C++ once a second. Kernels and levels ship as APK assets, unpacked to filesDir/payload
 * (--root-in); results go to the external files dir (--root-out), where adb can pull them.
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    companion object {
        // Fixed internal resolution (D7): 720 x 1560 on every device, scaled by the compositor.
        const val DRAWABLE_W = 720
        const val DRAWABLE_H = 1560
        const val TAG = "aglab"
    }

    private lateinit var status: TextView
    private var userArgs: List<String> = emptyList()
    private var render: HandlerThread? = null
    private var renderHandler: Handler? = null
    private var stateThread: Thread? = null
    @Volatile private var started = false
    @Volatile private var finished = false
    private var periodNs = 16_666_667L
    private var rootOut = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val root = FrameLayout(this)
        val surfaceView = SurfaceView(this)
        status = TextView(this).apply {
            setBackgroundColor(Color.TRANSPARENT)
            setTextColor(Color.argb(200, 150, 230, 255))
            typeface = android.graphics.Typeface.MONOSPACE
            gravity = Gravity.CENTER_HORIZONTAL or Gravity.BOTTOM
            textSize = 11f
            setPadding(16, 16, 16, 48)
        }
        root.addView(surfaceView)
        root.addView(status)
        setContentView(root)
        userArgs = intent.getStringExtra("args").orEmpty().trim().split(Regex("\\s+")).filter { it.startsWith("--") }
        if (userArgs.none { it.startsWith("--mode=") }) {
            status.text = "Afterglow\nlaunch with --es args \"--mode=…\" (tools/device/run_android.sh)"
            return
        }
        pickRefreshMode()
        arg("brightness")?.toFloatOrNull()?.let { pct ->
            window.attributes = window.attributes.apply { screenBrightness = (pct / 100f).coerceIn(0.01f, 1f) }
        }
        surfaceView.holder.addCallback(this)
    }

    private fun arg(key: String): String? = userArgs.firstOrNull { it.startsWith("--$key=") }?.substringAfter('=')

    /** The requested refresh (--req-display_refresh_hz; 60 Hz is the primary condition, docs/04 §1,
     *  120 Hz the D53 subset): same resolution, refresh rate closest to the request. */
    private fun requestedHz(): Float = arg("req-display_refresh_hz")?.toFloatOrNull()?.takeIf { it > 0f } ?: 60f

    private fun pickRefreshMode() {
        val disp = display ?: return
        val cur = disp.mode
        val hz = requestedHz()
        val best = disp.supportedModes
            .filter { it.physicalWidth == cur.physicalWidth && it.physicalHeight == cur.physicalHeight }
            .minByOrNull { abs(it.refreshRate - hz) } ?: return
        window.attributes = window.attributes.apply { preferredDisplayModeId = best.modeId }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (started || finished) return
        started = true
        val rootIn = unpackAssets()
        rootOut = (getExternalFilesDir(null) ?: filesDir).absolutePath
        val args = userArgs.toMutableList()
        if (arg("kernels") == null) args += "--kernels=kernels"
        if (arg("levels") == null) args += "--levels=levels"
        if (arg("brightness") != null && arg("req-brightness") == null) args += "--req-brightness=${arg("brightness")}"
        status.text = "Afterglow running\n" + userArgs.joinToString(" ")
        val thread = HandlerThread("ag-render", android.os.Process.THREAD_PRIORITY_DISPLAY).apply { start() }
        render = thread
        val handler = Handler(thread.looper)
        renderHandler = handler
        handler.post {
            NativeBridge.platformState(platformState())   // before create: state_start needs the host facts
            val rc = NativeBridge.create(args.joinToString(" "), rootIn.absolutePath, rootOut, holder.surface,
                                         DRAWABLE_W, DRAWABLE_H)
            Log.i(TAG, NativeBridge.message())
            if (rc != 0) {
                end(2)
                return@post
            }
            scheduleFrame()
        }
        stateThread = Thread {
            try {
                while (!finished) {
                    Thread.sleep(1000)
                    val json = platformState()
                    renderHandler?.post { if (!finished) NativeBridge.platformState(json) }
                }
            } catch (_: InterruptedException) {
            }
        }.apply { isDaemon = true; start() }
    }

    /** On the render thread: one Choreographer callback per vsync. */
    private fun scheduleFrame() {
        val ch = Choreographer.getInstance()
        if (Build.VERSION.SDK_INT >= 33) {
            ch.postVsyncCallback { data -> runFrame(data.preferredFrameTimeline.expectedPresentationTimeNanos) }
        } else {
            ch.postFrameCallback { runFrame(0L) }
        }
    }

    private fun runFrame(targetNs: Long) {
        if (finished) return
        display?.refreshRate?.let { if (it > 1f) periodNs = (1e9 / it).toLong() }
        val r = NativeBridge.frame(targetNs, periodNs)
        if (r != 0) end(if (r == 1) 0 else 1) else scheduleFrame()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) = interrupted("background")

    override fun onPause() {
        super.onPause()
        interrupted("background")
    }

    /** A backgrounded or interrupted run is invalid (docs/04 §2 rule 2): end it and say so. */
    private fun interrupted(what: String) {
        if (!started || finished) return
        renderHandler?.post {
            if (!finished) {
                NativeBridge.event(what)
                end(3)
            }
        }
    }

    /** On the render thread. */
    private fun end(code: Int) {
        if (finished) return
        finished = true
        val msg = NativeBridge.message()
        val dir = NativeBridge.runDir()
        NativeBridge.destroy()
        File(rootOut, "last_run.json").writeText(
            JSONObject().put("exit_code", code).put("message", msg).put("run_dir", dir).toString(2))
        Log.i(TAG, "AGLAB DONE exit=$code $msg")
        runOnUiThread {
            status.text = msg
            finishAndRemoveTask()
            exitProcess(code)
        }
    }

    /** Copies assets/kernels and assets/levels to filesDir/payload once per installed APK. */
    private fun unpackAssets(): File {
        val root = File(filesDir, "payload")
        val stamp = File(root, ".stamp")
        val version = packageManager.getPackageInfo(packageName, 0).lastUpdateTime.toString()
        if (stamp.exists() && stamp.readText() == version) return root
        root.deleteRecursively()
        fun copy(path: String) {
            val children = assets.list(path).orEmpty()
            if (children.isEmpty()) {
                val out = File(root, path)
                out.parentFile?.mkdirs()
                assets.open(path).use { input -> out.outputStream().use { input.copyTo(it) } }
            } else {
                children.forEach { copy("$path/$it") }
            }
        }
        copy("kernels")
        copy("levels")
        stamp.writeText(version)
        return root
    }

    /** Facts the C++ side cannot read on Android (merged into run.json and device.csv, R7). */
    private fun platformState(): String {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        val bm = getSystemService(Context.BATTERY_SERVICE) as BatteryManager
        val sticky = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val plugged = sticky?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        val status = sticky?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val thermal = when (pm.currentThermalStatus) {
            PowerManager.THERMAL_STATUS_NONE -> "none"
            PowerManager.THERMAL_STATUS_LIGHT -> "light"
            PowerManager.THERMAL_STATUS_MODERATE -> "moderate"
            PowerManager.THERMAL_STATUS_SEVERE -> "severe"
            PowerManager.THERMAL_STATUS_CRITICAL -> "critical"
            PowerManager.THERMAL_STATUS_EMERGENCY -> "emergency"
            PowerManager.THERMAL_STATUS_SHUTDOWN -> "shutdown"
            else -> "unknown"
        }
        val winBright = window.attributes.screenBrightness
        val brightness: Any = if (winBright >= 0f) (winBright * 100f).roundToInt()
            else runCatching { (Settings.System.getInt(contentResolver, Settings.System.SCREEN_BRIGHTNESS) * 100.0 / 255.0).roundToInt() }
                .getOrElse { "n/a: not readable" }
        val state = JSONObject()
            .put("thermal_state", thermal)
            // JSONObject.put(double) throws on NaN; the Tab S6 Lite always returns NaN here (D62).
            .put("thermal_headroom_10s", pm.getThermalHeadroom(10).let { if (it.isFinite()) it.toDouble() else "n/a: $it" })
            .put("battery_percent", bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY))
            // "Charging" means on the charger (D3, D43): BatteryManager.isCharging() reads false on
            // HyperOS 3 while plugged into AC with the level rising, so it is logged but not used.
            .put("charging", plugged != 0)
            .put("battery_status", when (status) {
                BatteryManager.BATTERY_STATUS_CHARGING -> "charging"
                BatteryManager.BATTERY_STATUS_FULL -> "full"
                BatteryManager.BATTERY_STATUS_DISCHARGING -> "discharging"
                BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "not_charging"
                else -> "unknown"
            })
            .put("is_charging_api", bm.isCharging)
            .put("current_ua", bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW))
            .put("voltage_mv", sticky?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1) ?: -1)
            .put("power_source", when (plugged) {
                BatteryManager.BATTERY_PLUGGED_AC -> "AC"
                BatteryManager.BATTERY_PLUGGED_USB -> "USB"
                BatteryManager.BATTERY_PLUGGED_WIRELESS -> "wireless"
                else -> "battery"
            })
            .put("low_power_mode", pm.isPowerSaveMode)
            .put("display_refresh_hz", (display?.refreshRate ?: -1f).roundToInt())
            .put("brightness", brightness)
        val device = JSONObject()
            .put("manufacturer", Build.MANUFACTURER)
            .put("model", Build.MODEL)
            .put("device", Build.DEVICE)
            .put("hardware", Build.HARDWARE)
            .put("os", "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            .put("os_build", Build.DISPLAY)
            .put("sdk_int", Build.VERSION.SDK_INT)
            .put("fingerprint", Build.FINGERPRINT)
            .put("soc", "${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        val gm = getSystemService(Context.GAME_SERVICE) as GameManager?
        val gameMode = when (gm?.gameMode) {
            GameManager.GAME_MODE_STANDARD -> "standard"
            GameManager.GAME_MODE_PERFORMANCE -> "performance"
            GameManager.GAME_MODE_BATTERY -> "battery"
            GameManager.GAME_MODE_UNSUPPORTED -> "unsupported"
            null -> "unknown"
            else -> "other(${gm.gameMode})"
        }
        val osGame = JSONObject().put("game_mode", gameMode)
        return JSONObject().put("device", device).put("state", state).put("os_game", osGame).toString()
    }
}
