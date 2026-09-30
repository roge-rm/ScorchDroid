package com.rm.scorchdroid

/**
 * The game's sound effects: the one-shots the engine queues each tick
 * (SoundEventQueue.h), and the loops for shells in flight and the turret servo.
 * SoundPlayer on a phone, Web Audio in a browser.
 */
interface SoundEffects {
    var enabled: Boolean
    var masterVolume: Float
    fun play(filePath: String, gain: Float = 1.0f, priority: Int = PRIORITY_ACTION, pan: Float = 0f)
    fun preload(filePaths: List<String>)
    fun startLoop(
        key: String,
        filePath: String,
        gain: Float = 1.0f,
        priority: Int = PRIORITY_ACTION,
        pan: Float = 0f,
        rate: Float = 1f,
    )
    fun updateLoop(key: String, gain: Float, pan: Float, rate: Float = 1f)
    fun stopLoop(key: String)
    fun stopAllLoops()
    fun release()

    companion object {
        /** Upstream's priority for an action's sound (SoundAction). */
        const val PRIORITY_ACTION = 10000

        /** A shell's engine loop, below an explosion's. */
        const val PRIORITY_MISSILE = 200
    }
}

/** Upstream's music states (music.xml's <playstate>). */
enum class MusicState(val xmlName: String) {
    LOADING("loading"),
    WAIT("wait"),
    BUYING("buying"),
    PLAYING("playing"),
    SHOT("shot"),
    SCORE("score"),
}

/** The music, keyed to game state by the mod's music.xml. */
interface MusicOutput {
    var enabled: Boolean
    var volume: Float
    fun load(mod: String)
    fun setState(next: MusicState)
    fun pause()
    fun resume()
    fun release()
}

/** The landscape's own ambient sound. */
interface AmbientOutput {
    var enabled: Boolean
    var volume: Float
    fun apply(sounds: List<AmbientSound>)
    fun pause()
    fun resume()
    fun stop()
    fun release()
}
