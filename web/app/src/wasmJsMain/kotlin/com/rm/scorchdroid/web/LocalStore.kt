package com.rm.scorchdroid.web

import com.rm.scorchdroid.KeyValueStore

private fun storageGet(key: String): String? =
    js("(() => { try { return localStorage.getItem(key); } catch (e) { return null; } })()")
private fun storageSet(key: String, value: String): Unit =
    js("(() => { try { localStorage.setItem(key, value); } catch (e) {} })()")

/**
 * The browser's [KeyValueStore]: localStorage, under a prefix. Anything it
 * can't read back, a private window's storage for one, reads as the default.
 */
class LocalStore(private val prefix: String) : KeyValueStore {
    private fun get(key: String) = storageGet("$prefix.$key")
    private fun set(key: String, value: String) = storageSet("$prefix.$key", value)

    override fun getString(key: String, default: String): String = get(key) ?: default
    override fun getInt(key: String, default: Int): Int = get(key)?.toIntOrNull() ?: default
    override fun getFloat(key: String, default: Float): Float = get(key)?.toFloatOrNull() ?: default
    override fun getBoolean(key: String, default: Boolean): Boolean = get(key)?.toBooleanStrictOrNull() ?: default
    override fun putString(key: String, value: String) = set(key, value)
    override fun putInt(key: String, value: Int) = set(key, value.toString())
    override fun putFloat(key: String, value: Float) = set(key, value.toString())
    override fun putBoolean(key: String, value: Boolean) = set(key, value.toString())
}
