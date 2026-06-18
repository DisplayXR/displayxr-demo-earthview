// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thin NativeActivity wrapper for the EarthView Android leg (M3 bring-up).
// Adapted from displayxr-demo-modelviewer's MainActivity. Three jobs:
//   1. Push the authoritative 4-way display rotation to native on launch + on
//      every rotation (incl. 180° flips, via a DisplayListener) — the renderer
//      can't derive true rotation from its own surface.
//   2. Forward touch from the runtime's MonadoView overlay (which covers our
//      window and is the only view that receives touch) to native via
//      dispatchTouchEvent (runtime#499) — a NativeActivity's native input queue
//      is NOT fed by dispatchTouchEvent.
//   3. Wake the DisplayXR runtime out of Android's "stopped" state before
//      xrCreateInstance and, if it can't be reached, prompt the user.
//
// Vendor-neutral + out-of-process (ADR-025): no CNSDK, no CAMERA — the OOP
// runtime service owns eye tracking + weave. Beyond the bring-up jobs above it
// also owns: (4) the first-run Map Tiles API-key dialog (persisted in
// SharedPreferences, handed to native) and (5) the Google attribution overlay
// (logo + the data-provider credits native reports — required by the Map Tiles
// ToS). See docs/m3-android-plan.md / docs/api-key.md.

package com.displayxr.earthview_vk_android

import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast

class MainActivity : NativeActivity() {

    companion object {
        init {
            System.loadLibrary("earthview_vk_android")
        }

        // Runtime flavors, in discovery preference order. ADR-025: the
        // out-of-process runtime is production; in_process is dev-only.
        private val RUNTIME_PACKAGES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )

        private const val PREFS = "earthview"
        private const val PREF_API_KEY = "api_key"
        private const val KEY_HELP_URL =
            "https://developers.google.com/maps/documentation/tile/get-api-key"
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3.
    private external fun nativeSetRotation(rotation: Int)

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

    // Touch bridge: single-finger drag / two-finger pinch (handled native-side).
    // The MonadoView overlay forwards events here.
    private external fun nativeOnTouch(
        action: Int, count: Int, x0: Float, y0: Float, x1: Float, y1: Float,
    )

    // Double-tap a landmark → focus/orbit it; double-tap the sky → release to
    // fly. (x,y) are view pixels; (viewW,viewH) the touch-surface dims native
    // needs to build the pick NDC (canvasRectPx is portrait-native on a rotated
    // panel and would transpose the NDC). Native unprojects via a depth pick.
    private external fun nativeOnDoubleTap(x: Float, y: Float, viewW: Float, viewH: Float)

    // The user's Map Tiles API key (re-creates the tileset native-side).
    private external fun nativeSetApiKey(key: String)

    // True once the tileset is streaming (false ⇒ keyless ⇒ show the dialog).
    private external fun nativeTilesActive(): Boolean

    // Joined data-provider credits for the attribution overlay (logo is local).
    private external fun nativeGetAttribution(): String

    private val prefs: SharedPreferences by lazy { getSharedPreferences(PREFS, Context.MODE_PRIVATE) }

    private val installedRuntime: String? by lazy {
        RUNTIME_PACKAGES.firstOrNull {
            try {
                packageManager.getLaunchIntentForPackage(it) != null ||
                    packageManager.getPackageInfo(it, 0) != null
            } catch (_: Throwable) {
                false
            }
        }
    }

    private val gestureDetector by lazy {
        GestureDetector(
            this,
            object : GestureDetector.SimpleOnGestureListener() {
                override fun onDoubleTap(e: MotionEvent): Boolean {
                    // The touch surface (decorView) is in the rotated/landscape
                    // basis the rendered tile uses; pass its dims so native can
                    // build a coherent pick NDC (NOT the portrait-native canvas).
                    val vw = window.decorView.width.toFloat()
                    val vh = window.decorView.height.toFloat()
                    try { nativeOnDoubleTap(e.x, e.y, vw, vh) } catch (_: Throwable) {}
                    return true
                }

                override fun onLongPress(e: MotionEvent) {
                    showKeyDialog(prefs.getString(PREF_API_KEY, null))  // change key
                }
            },
        )
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event)
        val n = event.pointerCount
        val x1 = if (n > 1) event.getX(1) else 0f
        val y1 = if (n > 1) event.getY(1) else 0f
        try {
            nativeOnTouch(event.actionMasked, n, event.getX(0), event.getY(0), x1, y1)
        } catch (_: Throwable) {
            // Native lib not bound yet — ignore until it is.
        }
        return super.dispatchTouchEvent(event)
    }

    private fun watchForRuntimeUnavailable() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                var tries = 0
                override fun run() {
                    if (isFinishing) return
                    val unavailable = try { nativeRuntimeUnavailable() } catch (_: Throwable) { false }
                    if (unavailable) { showRuntimeMissingDialog(); return }
                    val ready = try { nativeXrReady() } catch (_: Throwable) { false }
                    if (ready) return
                    if (tries++ < 15) handler.postDelayed(this, 1000)
                }
            },
            2000,
        )
    }

    private fun showRuntimeMissingDialog() {
        try {
            AlertDialog.Builder(this)
                .setTitle("DisplayXR not running")
                .setMessage(
                    "Couldn't reach the DisplayXR runtime.\n\n" +
                        "Open the DisplayXR app once (it shows the logo), then reopen this app.",
                )
                .setCancelable(false)
                .setPositiveButton("Open DisplayXR") { _, _ ->
                    installedRuntime?.let { pkg ->
                        packageManager.getLaunchIntentForPackage(pkg)?.let { startActivity(it) }
                    }
                    finish()
                }
                .setNegativeButton("Close") { _, _ -> finish() }
                .show()
        } catch (_: Throwable) {}
    }

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) = pushRotation()
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
    }

    private fun pushRotation() {
        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation
        try { nativeSetRotation(rotation) } catch (_: Throwable) {}
    }

    // Wake the runtime package before xrCreateInstance (clears Android's
    // "stopped" flag so the loader's broker lookup can discover it on a cold tap).
    private fun wakeRuntime() {
        val pkg = installedRuntime ?: return
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = pkg
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {}
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        wakeRuntime()
        // Hand any saved key to native BEFORE bring-up so tiles_init finds it.
        prefs.getString(PREF_API_KEY, null)?.takeIf { it.isNotEmpty() }?.let {
            try { nativeSetApiKey(it) } catch (_: Throwable) {}
        }
        super.onCreate(savedInstanceState)
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        watchForRuntimeUnavailable()
        watchForKeyAndAttribution()
        showControlsHint()
    }

    // ── Map Tiles API key ───────────────────────────────────────────────────
    // After bring-up, if the tileset isn't streaming and we have no saved key,
    // prompt for one. Also kicks the attribution overlay once tiles are live.
    private fun watchForKeyAndAttribution() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                var tries = 0
                var prompted = false
                override fun run() {
                    if (isFinishing) return
                    val active = try { nativeTilesActive() } catch (_: Throwable) { false }
                    if (active) {
                        ensureAttributionOverlay()
                        return  // streaming — done
                    }
                    val haveKey = !prefs.getString(PREF_API_KEY, null).isNullOrEmpty()
                    val ready = try { nativeXrReady() } catch (_: Throwable) { false }
                    if (ready && !haveKey && !prompted) {
                        prompted = true
                        showKeyDialog(null)
                    }
                    if (tries++ < 60) handler.postDelayed(this, 1000)
                }
            },
            3000,
        )
    }

    private fun showKeyDialog(current: String?) {
        try {
            val density = resources.displayMetrics.density
            val pad = (20 * density).toInt()
            val field = EditText(this).apply {
                hint = "AIza…  (paste your Map Tiles API key)"
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                setText(current ?: "")
            }
            val container = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(pad, pad / 2, pad, 0)
                addView(field)
            }
            AlertDialog.Builder(this)
                .setTitle("Google Map Tiles API key")
                .setMessage(
                    "EarthView streams Google Photorealistic 3D Tiles, which need " +
                        "your own Map Tiles API key. Paste it below, or get one from " +
                        "the Google Cloud Console (enable “Map Tiles API”).",
                )
                .setView(container)
                .setCancelable(current != null)
                .setPositiveButton("Save") { _, _ ->
                    val key = field.text.toString().trim()
                    if (key.isNotEmpty()) {
                        prefs.edit().putString(PREF_API_KEY, key).apply()
                        try { nativeSetApiKey(key) } catch (_: Throwable) {}
                    }
                }
                .setNeutralButton("Get a key") { _, _ ->
                    try { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(KEY_HELP_URL))) } catch (_: Throwable) {}
                    // Re-prompt after they return.
                    Handler(Looper.getMainLooper()).postDelayed({ if (!isFinishing) showKeyDialog(current) }, 500)
                }
                .apply { if (current != null) setNegativeButton("Cancel", null) }
                .show()
        } catch (_: Throwable) {}
    }

    // ── Google attribution overlay (required by the Map Tiles ToS) ──────────
    // A bottom strip ABOVE the runtime's MonadoView weave surface: the Google
    // logo + the data-provider credits native reports (~2 Hz). Added as its own
    // WindowManager window (like the modelviewer bar) so it stacks over the
    // weave window.
    private var attrBar: LinearLayout? = null
    private var attrText: TextView? = null
    private val attrHandler = Handler(Looper.getMainLooper())

    private fun ensureAttributionOverlay() {
        if (attrBar != null) return
        try {
            val density = resources.displayMetrics.density
            val bar = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding((12 * density).toInt(), (6 * density).toInt(),
                    (12 * density).toInt(), (6 * density).toInt())
                background = GradientDrawable().apply { setColor(0x66000000) }  // subtle scrim
            }
            val logo = ImageView(this).apply {
                setImageResource(R.drawable.google_logo)
                val h = (16 * density).toInt()
                layoutParams = LinearLayout.LayoutParams((h * 66 / 26), h).apply {
                    marginEnd = (10 * density).toInt()
                }
            }
            attrText = TextView(this).apply {
                setTextColor(Color.WHITE)
                textSize = 11f
                text = "Google"
                maxLines = 2
            }
            bar.addView(logo)
            bar.addView(attrText)
            val wlp = WindowManager.LayoutParams().apply {
                width = WindowManager.LayoutParams.MATCH_PARENT
                height = WindowManager.LayoutParams.WRAP_CONTENT
                gravity = Gravity.BOTTOM
                type = WindowManager.LayoutParams.TYPE_APPLICATION
                flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                format = PixelFormat.TRANSLUCENT
            }
            windowManager.addView(bar, wlp)
            attrBar = bar
            refreshAttribution()
        } catch (_: Throwable) {}
    }

    private val refreshRunnable = object : Runnable {
        override fun run() {
            refreshAttribution()
            attrHandler.postDelayed(this, 2000)
        }
    }

    private fun refreshAttribution() {
        try {
            val credits = nativeGetAttribution()
            attrText?.text = if (credits.isNullOrEmpty()) "Google" else "Google  ·  $credits"
            attrHandler.removeCallbacks(refreshRunnable)
            attrHandler.postDelayed(refreshRunnable, 2000)
        } catch (_: Throwable) {}
    }

    private fun showControlsHint() {
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                Toast.makeText(
                    this,
                    "Drag: look · Pinch: zoom · 2-finger drag: pan (exits inspect) · " +
                        "Double-tap: inspect a place · 2-finger double-tap: reset view",
                    Toast.LENGTH_LONG,
                ).show()
            }
        }, 2500)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushRotation()
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {}
        attrHandler.removeCallbacks(refreshRunnable)
        attrBar?.let { try { windowManager.removeViewImmediate(it) } catch (_: Throwable) {} }
        attrBar = null
        super.onDestroy()
    }
}
