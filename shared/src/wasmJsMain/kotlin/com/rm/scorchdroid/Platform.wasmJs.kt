package com.rm.scorchdroid

import androidx.compose.runtime.Composable
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asComposeImageBitmap
import androidx.compose.ui.graphics.toComposeImageBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import kotlin.wasm.unsafe.UnsafeWasmMemoryApi
import kotlin.wasm.unsafe.withScopedMemoryAllocator
import org.jetbrains.skia.Bitmap
import org.jetbrains.skia.ColorAlphaType
import org.jetbrains.skia.ColorType
import org.jetbrains.skia.Image
import org.jetbrains.skia.ImageInfo

private fun dateNow(): Double = js("Date.now()")

actual fun nowMillis(): Long = dateNow().toLong()

private fun jsSavedGameLabel(epochSeconds: Double): String = js(
    """(() => {
        const saved = new Date(epochSeconds * 1000);
        const now = new Date();
        const two = (n) => String(n).padStart(2, '0');
        const time = two(saved.getHours()) + ':' + two(saved.getMinutes());
        if (saved.toDateString() === now.toDateString()) return 'Today ' + time;
        const month = saved.toLocaleString(undefined, { month: 'short' });
        return saved.getDate() + ' ' + month + ' ' + time;
    })()""",
)

actual fun savedGameLabel(epochSeconds: Long): String = jsSavedGameLabel(epochSeconds.toDouble())

// The engine's file system (the game data preloaded into it, see
// web/engine/CMakeLists.txt) is where every path the engine hands out lives.
private fun fsReadBytes(path: String): JsAny? =
    js("(() => { try { return globalThis.sd.FS.readFile(path); } catch (e) { return null; } })()")
private fun jsLength(a: JsAny): Int = js("a.length")
private fun jsCopyIn(a: JsAny, at: Int): Unit = js("new Uint8Array(wasmExports.memory.buffer, at, a.length).set(a)")

/** A Uint8Array's bytes, copied through Kotlin's own memory in one go. */
@OptIn(UnsafeWasmMemoryApi::class)
fun bytesOf(a: JsAny): ByteArray {
    val n = jsLength(a)
    val out = ByteArray(n)
    if (n == 0) return out
    withScopedMemoryAllocator { m ->
        val at = m.allocate(n)
        jsCopyIn(a, at.address.toInt())
        for (i in 0 until n) out[i] = (at + i).loadByte()
    }
    return out
}

/** A file from the engine's file system, or null if there's no such file. */
internal fun readEngineFile(path: String): ByteArray? = fsReadBytes(path)?.let { bytesOf(it) }

actual fun loadImageFile(path: String): ImageBitmap? = runCatching {
    val bytes = readEngineFile(path) ?: return null
    Image.makeFromEncoded(bytes).toComposeImageBitmap()
}.getOrNull()

actual fun argbImage(pixels: IntArray, width: Int, height: Int): ImageBitmap {
    // An ARGB Int laid out little-endian is B, G, R, A, which is Skia's BGRA.
    val bytes = ByteArray(width * height * 4)
    for (i in pixels.indices) {
        val p = pixels[i]
        bytes[i * 4] = (p and 0xff).toByte()
        bytes[i * 4 + 1] = ((p shr 8) and 0xff).toByte()
        bytes[i * 4 + 2] = ((p shr 16) and 0xff).toByte()
        bytes[i * 4 + 3] = ((p ushr 24) and 0xff).toByte()
    }
    val bitmap = Bitmap()
    bitmap.allocPixels(ImageInfo(width, height, ColorType.BGRA_8888, ColorAlphaType.UNPREMUL))
    bitmap.installPixels(bytes)
    return bitmap.asComposeImageBitmap()
}

private fun consoleLog(message: String): Unit = js("console.log(message)")

actual fun logInfo(tag: String, message: String) {
    consoleLog("$tag: $message")
}

@OptIn(ExperimentalComposeUiApi::class)
@Composable
actual fun screenSizeDp(): Pair<Int, Int> {
    val size = LocalWindowInfo.current.containerSize
    val density = LocalDensity.current.density
    return (size.width / density).toInt() to (size.height / density).toInt()
}

private fun fsReadText(path: String): String? =
    js("(() => { try { return globalThis.sd.FS.readFile(path, { encoding: 'utf8' }); } catch (e) { return null; } })()")

/** A text file from the engine's file system, or null if there's no such file. */
fun readEngineText(path: String): String? = fsReadText(path)
