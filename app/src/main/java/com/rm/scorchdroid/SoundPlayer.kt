package com.rm.scorchdroid

import android.media.AudioAttributes
import android.media.SoundPool
import android.util.Log
import java.io.File

/**
 * M3: plays sound events queued by the real simulation (SoundAction, see
 * SoundEventQueue.h/the porting plan), the way upstream plays them - through
 * a fixed pool of channels rather than one player per sound.
 *
 * This used to start an unbounded `MediaPlayer` per event, at full gain, with
 * upstream's positional parameters skipped as unnecessary. They were not. A
 * weapon like Death's Head detonates dozens of times, and the result was
 * dozens of copies of the same sample at once - painful rather than loud.
 *
 * Upstream cannot do that: `Sound::init()` creates exactly `SoundChannels`
 * OpenAL sources (default 8, `OptionsDisplay.cpp:412`) and
 * `Sound::updateSources()` hands them to the nearest, highest-priority
 * sources and stops the rest. `SoundPool` is the same shape - a mixer with a
 * hard `maxStreams` that evicts the lowest-priority stream when it runs out -
 * so the cap is expressed here and the arbitration native-side, where the
 * positions and the listener are (see SoundEventQueue.h).
 *
 * The long loops - music and the landscape's ambience - stay on `MediaPlayer`
 * in [MusicPlayer] and [AmbientPlayer]. `SoundPool` decodes into memory up
 * front, which is right for a hundred short effects and wrong for a
 * multi-megabyte OGG.
 */
object SoundPlayer {
    private const val TAG = "SoundPlayer"

    /**
     * Upstream's `SoundChannels` default. Kept in step with
     * `SoundEventQueue.h`'s own budget: native cuts each drained batch to
     * this many, and this is the ceiling on how many can still be sounding
     * from earlier batches.
     */
    private const val CHANNELS = 8

    /** Upstream's `VirtualSoundPriority::eAction` - see SoundEventQueue.h. */
    const val PRIORITY_ACTION = 10000

    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()

    /**
     * M11: the sound setting. Checked here rather than at every call site,
     * because the caller is the tick loop draining a queue and has no business
     * knowing about preferences.
     */
    @Volatile
    var enabled: Boolean = true

    /**
     * Upstream's "SoundVolume" - a master multiplier over each sound's own
     * gain. Kept apart from that gain because they mean different things:
     * gain is upstream's distance attenuation, computed per sound, and this
     * is what the player asked for.
     */
    @Volatile
    var masterVolume: Float = 1.0f

    private var pool: SoundPool? = null

    // Every sample decoded so far, by absolute path. Upstream caches the same
    // way (Sound::fetchOrCreateBuffer) - a landscape reuses a handful of
    // sounds for a whole game, so this loads each one once.
    private val sampleIds = mutableMapOf<String, Int>()
    private val loaded = mutableSetOf<Int>()

    // A sound asked for while its sample was still decoding, and the loudest
    // gain it was asked for at. Loading is asynchronous and a first shot would
    // otherwise be silent - the sound the player most wants to hear.
    private val pendingGain = mutableMapOf<Int, Float>()
    private val pendingPriority = mutableMapOf<Int, Int>()

    // Live looping streams by caller-chosen key - one per aiming axis. Held
    // so a loop can be stopped when the gesture ends; SoundPool has no way
    // to ask "what am I looping".
    private val loopStreams = mutableMapOf<String, Int>()

    private fun poolOrCreate(): SoundPool {
        pool?.let { return it }
        val created = SoundPool.Builder()
            .setMaxStreams(CHANNELS)
            .setAudioAttributes(attributes)
            .build()
        created.setOnLoadCompleteListener { soundPool, sampleId, status ->
            synchronized(sampleIds) {
                if (status != 0) {
                    // Left out of `loaded`, so it is never played; dropped from
                    // the cache so a later attempt can retry rather than being
                    // permanently poisoned by one bad decode.
                    Log.e(TAG, "Failed to decode sample $sampleId (status $status)")
                    sampleIds.values.remove(sampleId)
                    pendingGain.remove(sampleId)
                    pendingPriority.remove(sampleId)
                    return@synchronized
                }
                loaded.add(sampleId)
                val priority = pendingPriority.remove(sampleId) ?: PRIORITY_ACTION
                pendingGain.remove(sampleId)?.let { gain ->
                    playLoaded(soundPool, sampleId, gain, priority)
                }
            }
        }
        pool = created
        return created
    }

    /**
     * Plays [filePath] at [gain], which is upstream's own inverse-distance
     * attenuation as computed against the live listener - not a preference -
     * and at [priority], which is upstream's `VirtualSoundPriority`.
     *
     * Both matter to `SoundPool`, which evicts the lowest-priority stream
     * when the pool is full. Band first, then loudness within the band, so
     * the eviction order is upstream's: any explosion outranks a countdown
     * beep, and among explosions the most distant goes first.
     *
     * Getting this wrong is not academic. Upstream's `beep.wav` is a
     * five-second file carrying 0.07 seconds of beep and 4.9 seconds of
     * silence, so six countdown beeps can hold six of the eight channels
     * saying nothing at all. What makes that harmless upstream is precisely
     * that they are eText and yield to anything else.
     */
    fun play(filePath: String, gain: Float = 1.0f, priority: Int = PRIORITY_ACTION) {
        if (!enabled) return
        val file = File(filePath)
        if (!file.exists()) return

        val soundPool = poolOrCreate()
        synchronized(sampleIds) {
            val existing = sampleIds[filePath]
            if (existing != null) {
                if (loaded.contains(existing)) {
                    playLoaded(soundPool, existing, gain, priority)
                } else {
                    // Still decoding. Keep the loudest request, since that is
                    // the one that would have been audible.
                    pendingGain[existing] = maxOf(pendingGain[existing] ?: 0f, gain)
                    pendingPriority[existing] = maxOf(pendingPriority[existing] ?: 0, priority)
                }
                return
            }

            val sampleId = soundPool.load(filePath, 1)
            if (sampleId == 0) {
                Log.e(TAG, "Could not load $filePath")
                return
            }
            sampleIds[filePath] = sampleId
            pendingGain[sampleId] = gain
            pendingPriority[sampleId] = priority
        }
    }

    /**
     * Decodes these samples now, so the first time they are asked for they
     * can actually play.
     *
     * Needed for the aiming loops specifically. `SoundPool.load` is
     * asynchronous, and a one-shot can wait - [play] remembers the request
     * and fires it on load - but a loop cannot: by the time it decoded, the
     * gesture that wanted it would be over. Without this the first turret
     * swing of every game is silent.
     */
    fun preload(filePaths: List<String>) {
        if (!enabled) return
        val soundPool = poolOrCreate()
        synchronized(sampleIds) {
            for (path in filePaths) {
                if (sampleIds.containsKey(path)) continue
                if (!File(path).exists()) continue
                val sampleId = soundPool.load(path, 1)
                if (sampleId != 0) sampleIds[path] = sampleId
            }
        }
    }

    /**
     * Starts a looping sound under [key], replacing whatever that key was
     * already playing. Keyed rather than returning a stream id because the
     * callers are the aiming controls, and each axis has exactly one loop -
     * upstream holds one `VirtualSoundSource` per axis for the same reason.
     *
     * Silently does nothing if the sample has not been decoded yet: a loop is
     * a response to a gesture that is still happening, and starting it a
     * beat late is worse than not starting it. The one-shot that accompanies
     * it warms the cache anyway.
     */
    fun startLoop(key: String, filePath: String, gain: Float, priority: Int) {
        if (!enabled) return
        val soundPool = poolOrCreate()
        synchronized(sampleIds) {
            stopLoopLocked(key)
            val sampleId = sampleIds[filePath]
            if (sampleId == null) {
                // Decode it for next time; nothing to start now.
                val loading = soundPool.load(filePath, 1)
                if (loading != 0) sampleIds[filePath] = loading
                return
            }
            if (!loaded.contains(sampleId)) return
            val volume = (gain * masterVolume).coerceIn(0f, 1f)
            val streamPriority = priority + (gain.coerceIn(0f, 1f) * 99f).toInt()
            val stream = soundPool.play(sampleId, volume, volume, streamPriority, -1, 1.0f)
            if (stream != 0) loopStreams[key] = stream
        }
    }

    fun stopLoop(key: String) {
        synchronized(sampleIds) { stopLoopLocked(key) }
    }

    /**
     * Stops every loop. Upstream does the same when a move ends
     * (`TankKeyboardControlUtil::endPlayMove`) - a turret sound left running
     * because a gesture was interrupted would never stop on its own.
     */
    fun stopAllLoops() {
        synchronized(sampleIds) {
            loopStreams.keys.toList().forEach { stopLoopLocked(it) }
        }
    }

    private fun stopLoopLocked(key: String) {
        loopStreams.remove(key)?.let { pool?.stop(it) }
    }

    private fun playLoaded(soundPool: SoundPool, sampleId: Int, gain: Float, priority: Int) {
        val volume = (gain * masterVolume).coerceIn(0f, 1f)
        // Band-major, loudness-minor: the band separates eAction from eText
        // outright, and the gain orders sounds within a band by how near they
        // are. 99 rather than 100 so a band can never reach into the next.
        //
        // Ranked on the gain, not the volume: turning the master down should
        // not reorder which sounds matter.
        val streamPriority = priority + (gain.coerceIn(0f, 1f) * 99f).toInt()
        soundPool.play(sampleId, volume, volume, streamPriority, 0, 1.0f)
    }

    /**
     * Drops every decoded sample. Called when a game ends: the next one may be
     * a different landscape or a different mod, and holding a hundred decoded
     * effects for sounds that will not be asked for again is pure resident
     * memory.
     */
    fun release() {
        synchronized(sampleIds) {
            pool?.release()
            pool = null
            loopStreams.clear()
            sampleIds.clear()
            loaded.clear()
            pendingGain.clear()
            pendingPriority.clear()
        }
    }
}
