package com.rm.scorchdroid

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * M11: the player's own preferences, as opposed to a game's rules.
 *
 * The split matters and is worth stating: anything that changes how the *game*
 * behaves - rounds, wall type, starting money - is a Scorched3D option and
 * lives in the setup screen, read from the engine (see GameSetup.h). Everything
 * here is about this device and this person, means nothing to the engine, and
 * survives from one game to the next.
 *
 * Backed by SharedPreferences rather than DataStore, which the plan originally
 * named. DataStore's reads are asynchronous, and these values are needed before
 * the first frame is drawn and before the first tank is named - so using it
 * would mean either blocking on a coroutine at startup or rendering one frame
 * with the wrong settings. For a dozen scalars the simpler synchronous store is
 * the better fit, and it is one fewer dependency.
 *
 * The properties are Compose state, so a screen observing them updates as they
 * are set, and the setters write through to disk immediately - there is no
 * "save" button to forget to press.
 */
class GameSettings(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("scorchdroid.settings", Context.MODE_PRIVATE)

    // --- Player -------------------------------------------------------------

    /** Shown over your tank and to everyone else in a network game. */
    var playerName by mutableStateOf(prefs.getString(KEY_NAME, DEFAULT_NAME) ?: DEFAULT_NAME)
        private set

    fun updatePlayerName(value: String) {
        // The engine has the final say - it refuses an empty name and hands
        // back what is actually in force - so the stored value can never be a
        // name the game would not use.
        val accepted = NativeBridge.setPlayerName(value)
        playerName = accepted
        prefs.edit().putString(KEY_NAME, accepted).apply()
    }

    /**
     * M16: the tank this player wears. Empty means the game picks, which is
     * what it did before there was a choice - and stays the default, because
     * upstream's own new player gets a random tank too.
     */
    var tankModel by mutableStateOf(prefs.getString(KEY_MODEL, "") ?: "")
        private set

    fun updateTankModel(value: String) {
        tankModel = value
        prefs.edit().putString(KEY_MODEL, value).apply()
        applyIdentity()
    }

    /** Index into [NativeBridge.getTankColors]; -1 for whatever is free. */
    var tankColorIndex by mutableIntStateOf(prefs.getInt(KEY_COLOR, -1))
        private set

    fun updateTankColorIndex(value: Int) {
        tankColorIndex = value
        prefs.edit().putInt(KEY_COLOR, value).apply()
        applyIdentity()
    }

    /** Path relative to the data root; empty for none. */
    var avatar by mutableStateOf(prefs.getString(KEY_AVATAR, "") ?: "")
        private set

    fun updateAvatar(value: String) {
        avatar = value
        prefs.edit().putString(KEY_AVATAR, value).apply()
        applyIdentity()
    }

    private fun applyIdentity() {
        NativeBridge.setPlayerIdentity(tankModel, tankColorIndex, avatar)
    }

    // --- Sound --------------------------------------------------------------

    var soundEnabled by mutableStateOf(prefs.getBoolean(KEY_SOUND, true))
        private set

    fun updateSoundEnabled(value: Boolean) {
        soundEnabled = value
        SoundPlayer.enabled = value
        prefs.edit().putBoolean(KEY_SOUND, value).apply()
    }

    /** M15: upstream's music, keyed to game state by its music.xml. */
    var musicEnabled by mutableStateOf(prefs.getBoolean(KEY_MUSIC, true))
        private set

    fun updateMusicEnabled(value: Boolean) {
        musicEnabled = value
        music?.enabled = value
        prefs.edit().putBoolean(KEY_MUSIC, value).apply()
    }

    /** 0..1. Separate from effects, as upstream's SoundDialog has it. */
    var musicVolume by mutableFloatStateOf(prefs.getFloat(KEY_MUSIC_VOLUME, 0.6f))
        private set

    fun updateMusicVolume(value: Float) {
        musicVolume = value.coerceIn(0f, 1f)
        music?.volume = musicVolume
        prefs.edit().putFloat(KEY_MUSIC_VOLUME, musicVolume).apply()
    }

    /** The music player, once the Activity has one; applyAll() pushes to it. */
    var music: MusicPlayer? = null

    // --- HUD ----------------------------------------------------------------

    var showNamePlates by mutableStateOf(prefs.getBoolean(KEY_PLATES, true))
        private set

    fun updateShowNamePlates(value: Boolean) {
        showNamePlates = value
        prefs.edit().putBoolean(KEY_PLATES, value).apply()
    }

    var showHealthBars by mutableStateOf(prefs.getBoolean(KEY_HEALTH, true))
        private set

    fun updateShowHealthBars(value: Boolean) {
        showHealthBars = value
        prefs.edit().putBoolean(KEY_HEALTH, value).apply()
    }

    /** How long a chat message stays on screen. Upstream has no equivalent. */
    var chatToastSeconds by mutableIntStateOf(prefs.getInt(KEY_TOAST, 5))
        private set

    fun updateChatToastSeconds(value: Int) {
        chatToastSeconds = value.coerceIn(2, 15)
        prefs.edit().putInt(KEY_TOAST, chatToastSeconds).apply()
    }

    // --- Controls -----------------------------------------------------------

    /** Upstream's InvertMouse, for the camera drag. */
    var invertDrag by mutableStateOf(prefs.getBoolean(KEY_INVERT, false))
        private set

    fun updateInvertDrag(value: Boolean) {
        invertDrag = value
        prefs.edit().putBoolean(KEY_INVERT, value).apply()
    }

    /** Tap the ground to aim at it. Off leaves the sliders as the only aim. */
    var tapToAim by mutableStateOf(prefs.getBoolean(KEY_TAP_AIM, true))
        private set

    fun updateTapToAim(value: Boolean) {
        tapToAim = value
        prefs.edit().putBoolean(KEY_TAP_AIM, value).apply()
    }

    /**
     * Mirrors the HUD for left-handed play. Worth having on a phone, where a
     * control under your thumb and one across the screen are not the same
     * control at all - BlokHead has this for the same reason.
     */
    var leftHandMode by mutableStateOf(prefs.getBoolean(KEY_LEFT_HAND, false))
        private set

    fun updateLeftHandMode(value: Boolean) {
        leftHandMode = value
        prefs.edit().putBoolean(KEY_LEFT_HAND, value).apply()
    }

    /** How opaque the on-screen controls are. */
    var controlOpacity by mutableFloatStateOf(prefs.getFloat(KEY_OPACITY, 1.0f))
        private set

    fun updateControlOpacity(value: Float) {
        controlOpacity = value.coerceIn(0.3f, 1.0f)
        prefs.edit().putFloat(KEY_OPACITY, controlOpacity).apply()
    }

    // --- Graphics -----------------------------------------------------------

    var showTrees by mutableStateOf(prefs.getBoolean(KEY_TREES, true))
        private set

    fun updateShowTrees(value: Boolean) {
        showTrees = value
        prefs.edit().putBoolean(KEY_TREES, value).apply()
    }

    var showFog by mutableStateOf(prefs.getBoolean(KEY_FOG, true))
        private set

    fun updateShowFog(value: Boolean) {
        showFog = value
        prefs.edit().putBoolean(KEY_FOG, value).apply()
    }

    /**
     * Applies everything that lives outside this object - the engine's copy of
     * the name, the sound switch, the renderer's flags. Called once at startup
     * and again whenever a graphics option changes, so a setting is never only
     * true in the preferences file.
     */
    fun applyAll() {
        NativeBridge.setPlayerName(playerName)
        applyIdentity()
        SoundPlayer.enabled = soundEnabled
        music?.let { it.volume = musicVolume; it.enabled = musicEnabled }
        NativeBridge.setRenderOptions(showTrees, showFog)
    }

    private companion object {
        const val DEFAULT_NAME = "Player"
        const val KEY_NAME = "player.name"
        const val KEY_MODEL = "player.tankModel"
        const val KEY_COLOR = "player.tankColor"
        const val KEY_AVATAR = "player.avatar"
        const val KEY_SOUND = "sound.enabled"
        const val KEY_MUSIC = "music.enabled"
        const val KEY_MUSIC_VOLUME = "music.volume"
        const val KEY_PLATES = "hud.namePlates"
        const val KEY_HEALTH = "hud.healthBars"
        const val KEY_TOAST = "hud.chatToastSeconds"
        const val KEY_INVERT = "controls.invertDrag"
        const val KEY_TAP_AIM = "controls.tapToAim"
        const val KEY_LEFT_HAND = "controls.leftHand"
        const val KEY_OPACITY = "controls.opacity"
        const val KEY_TREES = "graphics.trees"
        const val KEY_FOG = "graphics.fog"
    }
}
