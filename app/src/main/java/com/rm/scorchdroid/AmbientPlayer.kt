package com.rm.scorchdroid

import android.media.AudioAttributes
import android.media.MediaPlayer
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.File
import kotlin.random.Random

/**
 * M21: the landscape's own ambient sound.
 *
 * Upstream hangs these off the landscape's texture definition - ocean waves on
 * the coastal maps, rain on the wet ones, birdsong in the trees - with two
 * kinds of timing: a looped track that plays continuously, and a repeat that
 * fires every so often over the top of it. Both are here; which files and
 * which timing is upstream's own data (see AmbientSound.h).
 *
 * Not reproduced: position. Upstream places each source in the world and mixes
 * it by distance; this port has no spatial audio, so a sound plays flat at the
 * gain its definition asks for. On a map where the sea is a long way off that
 * is audibly different - it is still much closer to upstream than silence.
 */
class AmbientPlayer(private val dataRoot: String) {

    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()

    private val loops = mutableListOf<MediaPlayer>()
    private var scope: CoroutineScope? = null
    private var current: List<AmbientSound> = emptyList()
    private var paused = false

    @Volatile
    var enabled: Boolean = true
        set(value) {
            field = value
            if (!value) stop() else apply(current)
        }

    /** 0..1, multiplied by each sound's own gain. */
    @Volatile
    // Upstream's AmbientSoundVolume default - 64 of 0-128. Overwritten by
    // GameSettings.applyAll from the player's own slider; this is only what
    // holds before that runs.
    var volume: Float = 0.5f
        set(value) {
            field = value.coerceIn(0f, 1f)
            synchronized(loops) {
                loops.forEachIndexed { index, player ->
                    val gain = current.getOrNull(index)?.gain ?: 1f
                    runCatching { player.setVolume(field * gain, field * gain) }
                }
            }
        }

    /**
     * Plays [sounds] and stops whatever was playing. Called when the landscape
     * changes, which in this game is every round - so it has to be cheap to
     * call with the same list, and it is: an identical list is left alone
     * rather than restarted, which would otherwise clip the loop every round
     * on a map that keeps its own atmosphere.
     */
    fun apply(sounds: List<AmbientSound>) {
        if (sounds == current && loops.isNotEmpty()) return
        stop()
        current = sounds
        if (!enabled || paused || sounds.isEmpty()) return

        val repeats = mutableListOf<AmbientSound>()
        sounds.forEach { sound ->
            val file = resolve(sound.file)
            if (file == null) {
                Log.w(TAG, "ambient sound missing: ${sound.file}")
                return@forEach
            }
            if (sound.looped) startLoop(file, sound.gain) else repeats.add(sound)
        }

        if (repeats.isNotEmpty()) {
            val running = CoroutineScope(SupervisorJob() + Dispatchers.Default)
            scope = running
            repeats.forEach { sound ->
                running.launch { repeatForever(sound) }
            }
        }
        Log.i(TAG, "ambient: ${loops.size} looped, ${repeats.size} intermittent")
    }

    private suspend fun repeatForever(sound: AmbientSound) {
        val file = resolve(sound.file) ?: return
        // Upstream's own interval arithmetic (LandscapeSoundTimingRepeat):
        // min + max * random, which is a wider spread than it looks - a 10/60
        // entry waits anywhere from 10 to 70 seconds.
        while (scope?.isActive == true) {
            val wait = sound.minSeconds + sound.maxSeconds * Random.nextFloat()
            delay((wait * 1000f).toLong().coerceAtLeast(250L))
            if (!enabled || paused) continue
            SoundPlayer.play(file.path)
        }
    }

    private fun startLoop(file: File, gain: Float) {
        val player = MediaPlayer()
        try {
            player.setAudioAttributes(attributes)
            player.setDataSource(file.path)
            player.isLooping = true
            player.setVolume(volume * gain, volume * gain)
            player.setOnErrorListener { mp, _, _ -> mp.release(); true }
            player.prepare()
            player.start()
            synchronized(loops) { loops.add(player) }
        } catch (e: Exception) {
            Log.w(TAG, "ambient loop failed: ${e.message}")
            player.release()
        }
    }

    /**
     * The engine hands back paths as it resolves them - relative to the data
     * root, which is its working directory but not this process's.
     */
    private fun resolve(path: String): File? {
        val direct = File(path)
        if (direct.isAbsolute && direct.exists()) return direct
        val rooted = File(dataRoot, path)
        return if (rooted.exists()) rooted else null
    }

    fun pause() {
        paused = true
        synchronized(loops) {
            loops.forEach { runCatching { if (it.isPlaying) it.pause() } }
        }
    }

    fun resume() {
        paused = false
        if (!enabled) return
        synchronized(loops) { loops.forEach { runCatching { it.start() } } }
    }

    /** Stops everything; the next apply() starts again from nothing. */
    fun stop() {
        scope?.cancel()
        scope = null
        synchronized(loops) {
            loops.forEach { runCatching { it.stop(); it.release() } }
            loops.clear()
        }
    }

    fun release() {
        stop()
        current = emptyList()
    }

    private companion object {
        const val TAG = "ScorchDroidAmbient"
    }
}
