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
                    return@synchronized
                }
                loaded.add(sampleId)
                pendingGain.remove(sampleId)?.let { gain ->
                    playLoaded(soundPool, sampleId, gain)
                }
            }
        }
        pool = created
        return created
    }

    /**
     * Plays [filePath] at [gain], which is upstream's own inverse-distance
     * attenuation as computed against the live listener - not a preference.
     *
     * The stream priority is derived from that gain, so when the pool is full
     * `SoundPool` evicts the quietest (which is to say the most distant)
     * stream. That is the same rule upstream's own sort applies, reached
     * through the mixer's own mechanism instead of a second one here.
     */
    fun play(filePath: String, gain: Float = 1.0f) {
        if (!enabled) return
        val file = File(filePath)
        if (!file.exists()) return

        val soundPool = poolOrCreate()
        synchronized(sampleIds) {
            val existing = sampleIds[filePath]
            if (existing != null) {
                if (loaded.contains(existing)) {
                    playLoaded(soundPool, existing, gain)
                } else {
                    // Still decoding. Keep the loudest request, since that is
                    // the one that would have been audible.
                    pendingGain[existing] = maxOf(pendingGain[existing] ?: 0f, gain)
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
        }
    }

    private fun playLoaded(soundPool: SoundPool, sampleId: Int, gain: Float) {
        val volume = gain.coerceIn(0f, 1f)
        val priority = (volume * 100f).toInt()
        soundPool.play(sampleId, volume, volume, priority, 0, 1.0f)
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
            sampleIds.clear()
            loaded.clear()
            pendingGain.clear()
        }
    }
}
