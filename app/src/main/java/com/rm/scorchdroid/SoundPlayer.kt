package com.rm.scorchdroid

import android.media.AudioAttributes
import android.media.MediaPlayer
import java.io.File

/**
 * M3: plays sound events queued by the real simulation (SoundAction, see
 * SoundEventQueue.h/the porting plan) using Android's own MediaPlayer -
 * upstream's sound files are plain .wav, and nothing here needs the
 * positional-audio parameters (gain/rolloff/reference distance) the
 * excluded desktop client would normally apply, so a fire-and-forget
 * MediaPlayer per sound is enough for this milestone.
 */
object SoundPlayer {
    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()

    fun play(filePath: String) {
        val file = File(filePath)
        if (!file.exists()) return

        val player = MediaPlayer()
        player.setAudioAttributes(attributes)
        try {
            player.setDataSource(filePath)
            player.setOnCompletionListener { it.release() }
            player.setOnErrorListener { mp, _, _ -> mp.release(); true }
            player.prepare()
            player.start()
        } catch (e: Exception) {
            player.release()
        }
    }
}
