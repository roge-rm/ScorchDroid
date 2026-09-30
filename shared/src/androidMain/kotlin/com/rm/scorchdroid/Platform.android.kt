package com.rm.scorchdroid

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Log
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalConfiguration

actual fun nowMillis(): Long = System.currentTimeMillis()

actual fun savedGameLabel(epochSeconds: Long): String {
    val saved = java.util.Calendar.getInstance().apply { timeInMillis = epochSeconds * 1000L }
    val now = java.util.Calendar.getInstance()
    val sameDay = saved.get(java.util.Calendar.YEAR) == now.get(java.util.Calendar.YEAR) &&
        saved.get(java.util.Calendar.DAY_OF_YEAR) == now.get(java.util.Calendar.DAY_OF_YEAR)
    val time = java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault()).format(saved.time)
    if (sameDay) return "Today $time"
    val day = java.text.SimpleDateFormat("d MMM", java.util.Locale.getDefault()).format(saved.time)
    return "$day $time"
}

actual fun loadImageFile(path: String): ImageBitmap? =
    runCatching { BitmapFactory.decodeFile(path)?.asImageBitmap() }.getOrNull()

actual fun argbImage(pixels: IntArray, width: Int, height: Int): ImageBitmap =
    Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888).asImageBitmap()

actual fun logInfo(tag: String, message: String) {
    Log.i(tag, message)
}

@Composable
actual fun screenSizeDp(): Pair<Int, Int> {
    val configuration = LocalConfiguration.current
    return configuration.screenWidthDp to configuration.screenHeightDp
}
