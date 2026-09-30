package com.rm.scorchdroid

import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.ImageBitmap

// The few things the phone and the browser do differently below the UI, each
// an expect with its two halves in androidMain and wasmJsMain.

/** Milliseconds since the epoch, as System.currentTimeMillis() reads on a phone. */
expect fun nowMillis(): Long

/** "Today 15:04" for a save from today, otherwise "19 Sep 15:04". */
expect fun savedGameLabel(epochSeconds: Long): String

/** An image file (PNG, JPEG or BMP) decoded for Compose, or null if it can't be. */
expect fun loadImageFile(path: String): ImageBitmap?

/** ARGB pixels, one Int each as Android's Bitmap has them, as a Compose image. */
expect fun argbImage(pixels: IntArray, width: Int, height: Int): ImageBitmap

/** A line in the log: logcat on a phone, the console in a browser. */
expect fun logInfo(tag: String, message: String)

/** The screen's size in dp, width then height. */
@Composable
expect fun screenSizeDp(): Pair<Int, Int>

/** [value] with [decimals] places, the way "%.1f".format() would write it. */
fun formatFixed(value: Float, decimals: Int): String {
    var scale = 1L
    repeat(decimals) { scale *= 10 }
    val negative = value < 0f
    val scaled = kotlin.math.round(kotlin.math.abs(value.toDouble()) * scale).toLong()
    val whole = scaled / scale
    val fraction = (scaled % scale).toString().padStart(decimals, '0')
    val sign = if (negative && scaled != 0L) "-" else ""
    return if (decimals == 0) "$sign$whole" else "$sign$whole.$fraction"
}

/** A little persistent storage for settings: SharedPreferences, or localStorage. */
interface KeyValueStore {
    fun getString(key: String, default: String): String
    fun getInt(key: String, default: Int): Int
    fun getFloat(key: String, default: Float): Float
    fun getBoolean(key: String, default: Boolean): Boolean
    fun putString(key: String, value: String)
    fun putInt(key: String, value: Int)
    fun putFloat(key: String, value: Float)
    fun putBoolean(key: String, value: Boolean)
}
