package com.rm.scorchdroid

import android.media.AudioAttributes
import android.media.MediaPlayer
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File

/**
 * M15: background music, driven by game state exactly as upstream's is.
 *
 * Upstream ships three OGG loops and a `music.xml` that maps each of its
 * play states - loading, wait, buying, playing, shot, score - to a file and a
 * gain. That file is read here rather than re-declared, so a mod that ships
 * its own music gets it for free, and the base game's choice of which loop
 * accompanies which moment is upstream's, not ours.
 *
 * The plan recorded OGG as "not needed" because upstream's *effects* are WAV.
 * Its music is not, and that decision was made without looking in the
 * directory. Nothing needs vendoring, though: Android's MediaPlayer decodes
 * Vorbis natively.
 *
 * Two players, so a state change can crossfade instead of cutting: the
 * outgoing loop fades over [FADE_MS] while the incoming one fades in. A change
 * to a state that maps to the file already playing is a no-op, which matters
 * because upstream maps several states to the same loop (buying and playing
 * share one) and a fade between identical tracks would be a pointless stutter.
 */
class MusicPlayer(private val dataRoot: File) {

    /** Upstream's own state names, from music.xml. */
    enum class State(val xmlName: String) {
        LOADING("loading"),
        WAIT("wait"),
        BUYING("buying"),
        PLAYING("playing"),
        SHOT("shot"),
        SCORE("score"),
    }

    private data class Track(val file: File, val gain: Float)

    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    private val handler = Handler(Looper.getMainLooper())
    private var tracks: Map<State, Track> = emptyMap()
    private var current: MediaPlayer? = null
    private var currentFile: File? = null
    private var currentGain = 1f
    private var fading: MediaPlayer? = null

    @Volatile var enabled: Boolean = true
        set(value) {
            field = value
            if (!value) stopAll() else state?.let { play(it, force = true) }
        }

    /** 0..1, multiplied by the track's own gain. */
    @Volatile var volume: Float = 0.6f
        set(value) {
            field = value.coerceIn(0f, 1f)
            current?.let { runCatching { it.setVolume(field * currentGain, field * currentGain) } }
        }

    private var state: State? = null
    private var paused = false

    /**
     * Reads the mod's music.xml, falling back to the base game's. Safe to call
     * again when the mod changes; the current loop keeps playing until the
     * next state change asks for something different.
     */
    fun load(mod: String) {
        val candidates = listOf(
            File(dataRoot, "data/globalmods/$mod/data/music.xml"),
            File(dataRoot, "data/globalmods/none/data/music.xml"),
        )
        val xml = candidates.firstOrNull { it.exists() }
        if (xml == null) {
            Log.w(TAG, "no music.xml found under ${dataRoot.path}")
            tracks = emptyMap()
            return
        }
        val modDir = xml.parentFile!!.parentFile!!  // .../globalmods/<mod>
        tracks = parse(xml, modDir)
        Log.i(TAG, "music: ${tracks.size} states mapped from ${xml.path}")
    }

    private fun parse(xml: File, modDir: File): Map<State, Track> {
        // Regular expressions over each entry rather than a pull-parser state
        // machine. The file is a few hundred bytes in a shape upstream fixed
        // years ago - a root <music> holding <music> entries, each with
        // <playstate>s, one <file> and one <gain> - and the first attempt at
        // a proper parser broke on exactly the mixture of comments, nested
        // same-named tags and nextText() end-tag consumption that makes pull
        // parsing fiddly. Nothing here needs more than this.
        val result = mutableMapOf<State, Track>()
        val text = try {
            xml.readText().replace(Regex("<!--.*?-->", RegexOption.DOT_MATCHES_ALL), "")
        } catch (e: Exception) {
            Log.w(TAG, "music.xml unreadable: ${e.message}")
            return result
        }
        // Inner entries only: the root <music> contains them, so match the
        // innermost blocks, which have no further <music> inside.
        val entry = Regex("<music>((?:(?!<music>).)*?)</music>", RegexOption.DOT_MATCHES_ALL)
        val stateTag = Regex("<playstate>\\s*([a-z]+)\\s*</playstate>")
        val fileTag = Regex("<file>\\s*([^<]+?)\\s*</file>")
        val gainTag = Regex("<gain>\\s*([0-9.]+)\\s*</gain>")
        for (m in entry.findAll(text)) {
            val body = m.groupValues[1]
            val file = fileTag.find(body)?.groupValues?.get(1) ?: continue
            val gain = gainTag.find(body)?.groupValues?.get(1)?.toFloatOrNull() ?: 1f
            // Paths in the file are mod-relative, like every other data path
            // upstream writes.
            val track = Track(File(modDir, file), gain)
            for (sm in stateTag.findAll(body)) {
                State.entries.firstOrNull { it.xmlName == sm.groupValues[1] }?.let { result[it] = track }
            }
        }
        return result
    }

    /** The game has moved into [next]; start its loop if it is a different file. */
    fun setState(next: State) {
        if (state == next) return
        state = next
        if (enabled && !paused) play(next, force = false)
    }

    private fun play(s: State, force: Boolean) {
        val track = tracks[s]
        if (track == null) {
            // A state with nothing mapped is silence, as it would be upstream.
            if (currentFile != null) stopAll()
            return
        }
        if (!force && track.file == currentFile) return
        if (!track.file.exists()) {
            Log.w(TAG, "music file missing: ${track.file}")
            return
        }

        val player = MediaPlayer()
        try {
            player.setAudioAttributes(attributes)
            player.setDataSource(track.file.path)
            player.isLooping = true
            player.setVolume(0f, 0f)
            player.setOnErrorListener { mp, _, _ -> mp.release(); true }
            player.prepare()
            player.start()
        } catch (e: Exception) {
            Log.w(TAG, "music start failed: ${e.message}")
            player.release()
            return
        }

        // Retire the previous one with a fade rather than a cut.
        Log.i(TAG, "music -> ${s.xmlName}: ${track.file.name}")
        fading?.let { runCatching { it.release() } }
        fading = current
        current = player
        currentFile = track.file
        currentGain = track.gain
        crossfade(fading, player, track.gain)
    }

    private fun crossfade(out: MediaPlayer?, incoming: MediaPlayer, gain: Float) {
        val steps = 12
        val stepMs = FADE_MS / steps
        var step = 0
        val tick = object : Runnable {
            override fun run() {
                step++
                val t = step.toFloat() / steps
                val target = volume * gain
                runCatching { incoming.setVolume(target * t, target * t) }
                out?.let { runCatching { it.setVolume(target * (1 - t), target * (1 - t)) } }
                if (step < steps) {
                    handler.postDelayed(this, stepMs)
                } else {
                    out?.let { runCatching { it.stop(); it.release() } }
                    if (fading === out) fading = null
                }
            }
        }
        handler.postDelayed(tick, stepMs)
    }

    /** Activity paused: silence without forgetting where we were. */
    fun pause() {
        paused = true
        current?.let { runCatching { if (it.isPlaying) it.pause() } }
    }

    fun resume() {
        paused = false
        if (!enabled) return
        val c = current
        if (c != null) runCatching { c.start() } else state?.let { play(it, force = true) }
    }

    private fun stopAll() {
        fading?.let { runCatching { it.release() } }
        fading = null
        current?.let { runCatching { it.stop(); it.release() } }
        current = null
        currentFile = null
    }

    fun release() {
        stopAll()
        handler.removeCallbacksAndMessages(null)
    }

    private companion object {
        const val TAG = "ScorchDroidMusic"
        const val FADE_MS = 1200L
    }
}
