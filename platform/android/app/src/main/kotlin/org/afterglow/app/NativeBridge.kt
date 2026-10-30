package org.afterglow.app

import android.view.Surface

/** The lab runner's C ABI (lab/include/ag/ag_app.h), through platform/android/jni. Render thread only. */
object NativeBridge {
    init { System.loadLibrary("afterglow") }

    @JvmStatic external fun create(args: String, rootIn: String, rootOut: String, surface: Surface?, width: Int, height: Int): Int
    @JvmStatic external fun frame(targetPresentNs: Long, periodNs: Long): Int
    @JvmStatic external fun platformState(json: String)
    @JvmStatic external fun event(name: String)
    @JvmStatic external fun finish(): Int
    @JvmStatic external fun message(): String
    @JvmStatic external fun runDir(): String
    @JvmStatic external fun nowNs(): Long
    @JvmStatic external fun destroy()
}
