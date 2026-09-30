package com.rm.scorchdroid

// globalThis.sd is the engine module the page sets up (web/engine, built by
// build.sh): engine_jni.cpp and renderer_jni.cpp compiled to WebAssembly. These
// make and read the JNI objects its bridge uses (web/engine/include/jni.h) for
// the generated halves of NativeBridge and GameRenderer.

private fun jniEnv(): Int = js("globalThis.sd._sd_jni_env()")
private fun jniRelease(): Unit = js("globalThis.sd._sd_jni_release()")
private fun jniString(s: String): Int = js(
    "(() => { const m = globalThis.sd; const p = m.stringToNewUTF8(s); " +
        "const r = m._sd_jni_string(p); m._free(p); return r; })()",
)
private fun jniChars(p: Int): String = js("globalThis.sd.UTF8ToString(globalThis.sd._sd_jni_chars(p))")
private fun jniLength(p: Int): Int = js("globalThis.sd._sd_jni_length(p)")
private fun jniObjectAt(p: Int, i: Int): Int = js("globalThis.sd._sd_jni_object_at(p, i)")
private fun jniObjects(n: Int): Int = js("globalThis.sd._sd_jni_objects(n)")
private fun jniSetObject(p: Int, i: Int, v: Int): Unit = js("globalThis.sd._sd_jni_set_object(p, i, v)")
private fun jniInts(n: Int): Int = js("globalThis.sd._sd_jni_ints(n)")
private fun jniIntData(p: Int): Int = js("globalThis.sd._sd_jni_int_data(p)")
private fun heapInt(address: Int): Int = js("globalThis.sd.HEAP32[address >> 2]")
private fun setHeapInt(address: Int, value: Int): Unit = js("globalThis.sd.HEAP32[address >> 2] = value")

/**
 * The JNI objects the engine's calls take and give, on the engine's heap.
 * Made to pass in, read when they come back, and freed together once a call
 * is done ([release]). The page has one thread, so nothing else can be using
 * the arena while a call is in flight.
 */
internal object Jni {
    val env: Int by lazy { jniEnv() }
    fun release() = jniRelease()

    fun string(s: String): Int = jniString(s)
    fun readString(p: Int): String = if (p == 0) "" else jniChars(p)

    fun strings(a: Array<String>): Int {
        val array = jniObjects(a.size)
        a.forEachIndexed { i, s -> jniSetObject(array, i, jniString(s)) }
        return array
    }
    fun readStrings(p: Int): Array<String> {
        if (p == 0) return emptyArray()
        return Array(jniLength(p)) { readString(jniObjectAt(p, it)) }
    }

    fun ints(a: IntArray): Int {
        val array = jniInts(a.size)
        val data = jniIntData(array)
        for (i in a.indices) setHeapInt(data + i * 4, a[i])
        return array
    }
    fun readInts(p: Int): IntArray {
        if (p == 0) return IntArray(0)
        val n = jniLength(p)
        val data = jniIntData(p)
        return IntArray(n) { heapInt(data + it * 4) }
    }
}
