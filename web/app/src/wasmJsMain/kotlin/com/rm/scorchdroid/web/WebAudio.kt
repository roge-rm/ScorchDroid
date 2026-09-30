package com.rm.scorchdroid.web

import com.rm.scorchdroid.AmbientOutput
import com.rm.scorchdroid.AmbientSound
import com.rm.scorchdroid.MusicOutput
import com.rm.scorchdroid.MusicState
import com.rm.scorchdroid.SoundEffects
import com.rm.scorchdroid.logInfo
import com.rm.scorchdroid.readEngineText
import kotlin.random.Random
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

// The browser's sound, on audio.js (globalThis.sdAudio). The same three the
// phone has - effects, music and the landscape's ambience - doing what
// SoundPlayer, MusicPlayer and AmbientPlayer do there.

private fun jsSetEffects(on: Boolean, volume: Float): Unit = js("globalThis.sdAudio.setEffects(on, volume)")
private fun jsPreload(path: String): Unit = js("globalThis.sdAudio.preload(path)")
private fun jsPlay(path: String, gain: Float, pan: Float): Unit = js("globalThis.sdAudio.play(path, gain, pan)")
private fun jsStartLoop(key: String, path: String, gain: Float, pan: Float, rate: Float): Unit =
    js("globalThis.sdAudio.startLoop(key, path, gain, pan, rate)")
private fun jsUpdateLoop(key: String, gain: Float, pan: Float, rate: Float): Unit =
    js("globalThis.sdAudio.updateLoop(key, gain, pan, rate)")
private fun jsStopLoop(key: String): Unit = js("globalThis.sdAudio.stopLoop(key)")
private fun jsStopAllLoops(): Unit = js("globalThis.sdAudio.stopAllLoops()")
private fun jsStartTrack(path: String, level: Float, fadeSeconds: Float, loop: Boolean): Int =
    js("globalThis.sdAudio.startTrack(path, level, fadeSeconds, loop)")
private fun jsSetTrackLevel(id: Int, level: Float): Unit = js("globalThis.sdAudio.setTrackLevel(id, level)")
private fun jsStopTrack(id: Int, fadeSeconds: Float): Unit = js("globalThis.sdAudio.stopTrack(id, fadeSeconds)")
private fun jsExists(path: String): Boolean = js("globalThis.sdAudio.exists(path)")

/** The engine's paths are relative to the data root, its working directory. */
private fun resolve(dataRoot: String, path: String): String? = when {
    path.startsWith("/") && jsExists(path) -> path
    jsExists("$dataRoot/$path") -> "$dataRoot/$path"
    else -> null
}

class WebSound(private val dataRoot: String) : SoundEffects {
    override var enabled: Boolean = true
        set(value) {
            field = value
            jsSetEffects(value, masterVolume)
        }
    override var masterVolume: Float = 1.0f
        set(value) {
            field = value.coerceIn(0f, 1f)
            jsSetEffects(enabled, field)
        }

    private fun path(file: String) = resolve(dataRoot, file) ?: file

    override fun play(filePath: String, gain: Float, priority: Int, pan: Float) {
        if (!enabled) return
        jsPlay(path(filePath), gain.coerceIn(0f, 1f), pan)
    }

    override fun preload(filePaths: List<String>) {
        filePaths.forEach { jsPreload(path(it)) }
    }

    override fun startLoop(key: String, filePath: String, gain: Float, priority: Int, pan: Float, rate: Float) {
        if (!enabled) return
        jsStartLoop(key, path(filePath), gain.coerceIn(0f, 1f), pan, rate)
    }

    override fun updateLoop(key: String, gain: Float, pan: Float, rate: Float) {
        jsUpdateLoop(key, gain.coerceIn(0f, 1f), pan, rate)
    }

    override fun stopLoop(key: String) = jsStopLoop(key)
    override fun stopAllLoops() = jsStopAllLoops()
    override fun release() = jsStopAllLoops()
}

/**
 * Upstream's music, keyed to game state by the mod's music.xml, the same
 * mapping MusicPlayer reads on a phone, crossfading over [FADE_SECONDS] when
 * the file changes.
 */
class WebMusic(private val dataRoot: String) : MusicOutput {
    private class Track(val path: String, val gain: Float)

    private var tracks: Map<MusicState, Track> = emptyMap()
    private var state: MusicState? = null
    private var current: Int = 0
    private var currentPath: String? = null
    private var currentGain = 1f
    private var paused = false

    override var enabled: Boolean = true
        set(value) {
            field = value
            if (!value) stopAll() else state?.let { play(it, force = true) }
        }
    override var volume: Float = 0.6f
        set(value) {
            field = value.coerceIn(0f, 1f)
            if (current != 0) jsSetTrackLevel(current, field * currentGain)
        }

    override fun load(mod: String) {
        val candidates = listOf(
            "$dataRoot/data/globalmods/$mod/data/music.xml",
            "$dataRoot/data/globalmods/none/data/music.xml",
        )
        val xml = candidates.firstOrNull { jsExists(it) }
        if (xml == null) {
            tracks = emptyMap()
            return
        }
        val modDir = xml.substringBeforeLast("/data/music.xml")
        val text = readEngineText(xml)?.replace(Regex("<!--.*?-->", RegexOption.DOT_MATCHES_ALL), "") ?: return
        val result = mutableMapOf<MusicState, Track>()
        // The same reading of the file MusicPlayer does: the innermost
        // <music> entries, each with its states, one file and one gain.
        val entry = Regex("<music>((?:(?!<music>).)*?)</music>", RegexOption.DOT_MATCHES_ALL)
        val stateTag = Regex("<playstate>\\s*([a-z]+)\\s*</playstate>")
        val fileTag = Regex("<file>\\s*([^<]+?)\\s*</file>")
        val gainTag = Regex("<gain>\\s*([0-9.]+)\\s*</gain>")
        for (m in entry.findAll(text)) {
            val body = m.groupValues[1]
            val file = fileTag.find(body)?.groupValues?.get(1) ?: continue
            val gain = gainTag.find(body)?.groupValues?.get(1)?.toFloatOrNull() ?: 1f
            val track = Track("$modDir/$file", gain)
            for (sm in stateTag.findAll(body)) {
                MusicState.entries.firstOrNull { it.xmlName == sm.groupValues[1] }?.let { result[it] = track }
            }
        }
        tracks = result
        logInfo(TAG, "music: ${tracks.size} states mapped from $xml")
    }

    override fun setState(next: MusicState) {
        if (state == next) return
        state = next
        if (enabled && !paused) play(next, force = false)
    }

    private fun play(s: MusicState, force: Boolean) {
        val track = tracks[s]
        if (track == null) {
            if (currentPath != null) stopAll()
            return
        }
        if (!force && track.path == currentPath) return
        if (current != 0) jsStopTrack(current, FADE_SECONDS)
        current = jsStartTrack(track.path, volume * track.gain, FADE_SECONDS, true)
        currentPath = track.path
        currentGain = track.gain
    }

    private fun stopAll() {
        if (current != 0) jsStopTrack(current, 0.1f)
        current = 0
        currentPath = null
    }

    override fun pause() {
        paused = true
        stopAll()
    }

    override fun resume() {
        paused = false
        if (enabled) state?.let { play(it, force = true) }
    }

    override fun release() = stopAll()

    private companion object {
        const val TAG = "ScorchDroidMusic"
        const val FADE_SECONDS = 1.2f
    }
}

/** The landscape's ambience: looped tracks, and repeats fired every so often. */
class WebAmbient(private val dataRoot: String, private val sound: SoundEffects) : AmbientOutput {
    private val scope: CoroutineScope = MainScope()
    private val loops = mutableListOf<Pair<Int, Float>>()
    private var repeats: Job? = null
    private var current: List<AmbientSound> = emptyList()
    private var paused = false

    override var enabled: Boolean = true
        set(value) {
            field = value
            if (!value) stop() else apply(current)
        }
    override var volume: Float = 0.5f
        set(value) {
            field = value.coerceIn(0f, 1f)
            loops.forEach { (id, gain) -> jsSetTrackLevel(id, field * gain) }
        }

    override fun apply(sounds: List<AmbientSound>) {
        if (sounds == current && loops.isNotEmpty()) return
        stop()
        current = sounds
        if (!enabled || paused || sounds.isEmpty()) return
        val intermittent = mutableListOf<Pair<AmbientSound, String>>()
        sounds.forEach { s ->
            val path = resolve(dataRoot, s.file) ?: return@forEach
            if (s.looped) {
                loops.add(jsStartTrack(path, volume * s.gain, 0.5f, true) to s.gain)
            } else {
                intermittent.add(s to path)
            }
        }
        if (intermittent.isNotEmpty()) {
            repeats = scope.launch {
                intermittent.forEach { (s, path) ->
                    launch {
                        // Upstream's own interval arithmetic
                        // (LandscapeSoundTimingRepeat): min + max * random.
                        while (isActive) {
                            val wait = s.minSeconds + s.maxSeconds * Random.nextFloat()
                            delay((wait * 1000f).toLong().coerceAtLeast(250L))
                            if (enabled && !paused) sound.play(path)
                        }
                    }
                }
            }
        }
    }

    override fun pause() {
        paused = true
        stop()
    }

    override fun resume() {
        paused = false
        if (enabled) apply(current.also { current = emptyList() })
    }

    override fun stop() {
        repeats?.cancel()
        repeats = null
        loops.forEach { (id, _) -> jsStopTrack(id, 0.3f) }
        loops.clear()
    }

    override fun release() {
        stop()
        current = emptyList()
    }
}
