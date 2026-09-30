package com.rm.scorchdroid

import android.content.SharedPreferences

/** The phone's [KeyValueStore]: the settings' own SharedPreferences file. */
class SharedPreferencesStore(private val prefs: SharedPreferences) : KeyValueStore {
    override fun getString(key: String, default: String): String = prefs.getString(key, default) ?: default
    override fun getInt(key: String, default: Int): Int = prefs.getInt(key, default)
    override fun getFloat(key: String, default: Float): Float = prefs.getFloat(key, default)
    override fun getBoolean(key: String, default: Boolean): Boolean = prefs.getBoolean(key, default)
    override fun putString(key: String, value: String) = prefs.edit().putString(key, value).apply()
    override fun putInt(key: String, value: Int) = prefs.edit().putInt(key, value).apply()
    override fun putFloat(key: String, value: Float) = prefs.edit().putFloat(key, value).apply()
    override fun putBoolean(key: String, value: Boolean) = prefs.edit().putBoolean(key, value).apply()
}
