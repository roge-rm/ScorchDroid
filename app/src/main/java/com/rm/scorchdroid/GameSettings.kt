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

    /**
     * Effects volume, upstream's "SoundVolume". Its range is 0-128 and its
     * default 128, so full is both upstream's default and what this port
     * played at before the slider existed - nothing gets quieter by adding
     * it.
     *
     * This is a master multiplier over the per-sound gain, which is
     * upstream's distance attenuation rather than a preference - see
     * SoundEventQueue.h.
     */
    var effectsVolume by mutableFloatStateOf(prefs.getFloat(KEY_SOUND_VOLUME, 1.0f))
        private set

    fun updateEffectsVolume(value: Float) {
        effectsVolume = value.coerceIn(0f, 1f)
        SoundPlayer.masterVolume = effectsVolume
        prefs.edit().putFloat(KEY_SOUND_VOLUME, effectsVolume).apply()
    }

    /**
     * Ambient volume, upstream's "AmbientSoundVolume" - 64 of 0-128, so half,
     * which is the one volume upstream deliberately does not run at full.
     * The landscape's atmosphere is a bed under the game, not part of it.
     */
    var ambientVolume by mutableFloatStateOf(prefs.getFloat(KEY_AMBIENT_VOLUME, 0.5f))
        private set

    fun updateAmbientVolume(value: Float) {
        ambientVolume = value.coerceIn(0f, 1f)
        ambient?.volume = ambientVolume
        prefs.edit().putFloat(KEY_AMBIENT_VOLUME, ambientVolume).apply()
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

    /**
     * M21: the landscape's own ambient sound - waves, rain, birdsong. Its own
     * switch rather than folded into sound effects, because it is the one
     * that plays continuously and so the one most likely to be unwanted.
     */
    var ambientEnabled by mutableStateOf(prefs.getBoolean(KEY_AMBIENT, true))
        private set

    fun updateAmbientEnabled(value: Boolean) {
        ambientEnabled = value
        ambient?.enabled = value
        prefs.edit().putBoolean(KEY_AMBIENT, value).apply()
    }

    /** The music player, once the Activity has one; applyAll() pushes to it. */
    var music: MusicPlayer? = null

    /** The ambient player, likewise. */
    var ambient: AmbientPlayer? = null

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

    /**
     * M23: how finely the landscape is drawn - the resolution of the mesh the
     * heightmap is sampled into.
     *
     * The default is the maximum, which is the heightmap's own resolution and
     * so what upstream draws. It is a setting rather than a fixed compromise
     * because the right answer depends on the device, and this is the screen
     * where that choice belongs.
     */
    var terrainDetail by mutableIntStateOf(prefs.getInt(KEY_DETAIL, DEFAULT_DETAIL))
        private set

    fun updateTerrainDetail(value: Int) {
        terrainDetail = value
        prefs.edit().putInt(KEY_DETAIL, value).apply()
        NativeBridge.setTerrainDetail(value)
    }

    /**
     * W3: how much the water reflects. 0 is a Fresnel-weighted sky colour -
     * this port's own; 1 adds the land, drawn a second time from a camera
     * mirrored in the water; 2 adds the tanks and scenery, which is what
     * Scorched3D reflects short of its effects.
     *
     * Defaults to the top, like every other quality setting here. It used to
     * default to 0, which was both the lowest of the three and the only one
     * that is not what Scorched3D does - so a fresh install got this port's
     * invention rather than upstream's water, and got it at the cheapest
     * setting, without anyone choosing either.
     */
    var reflectionLevel by mutableIntStateOf(prefs.getInt(KEY_REFLECT, 2))
        private set

    fun updateReflectionLevel(value: Int) {
        reflectionLevel = value.coerceIn(0, 2)
        prefs.edit().putInt(KEY_REFLECT, reflectionLevel).apply()
        NativeBridge.setReflectionStyle(reflectionLevel)
    }

    /**
     * Water detail: how finely Scorched3D's sea is drawn. 2 is Full - its
     * own 2-unit grid and its own 24 wave phases a second; 1 is Half (4
     * units, 12/s); 0 is Quarter (8 units, 6/s). The sea itself is the same
     * at every position - the Tessendorf spectrum upstream generates, driven
     * by the round's wind - only its cost changes.
     *
     * Low is on the left and high on the right, as on every slider here.
     */
    var waterDetail by mutableIntStateOf(prefs.getInt(KEY_WATER_DETAIL, 2))
        private set

    fun updateWaterDetail(value: Int) {
        waterDetail = value.coerceIn(0, 2)
        prefs.edit().putInt(KEY_WATER_DETAIL, waterDetail).apply()
        NativeBridge.setWaterDetail(waterDetail)
    }

    /**
     * M22: which aim sight to draw. False is this port's own blade - one wide
     * wedge along the barrel; true is Scorched3D's own, which surrounds the
     * tank with a protractor ring and puts a separate marker for the bearing
     * flat on the ground.
     */
    var originalSight by mutableStateOf(prefs.getBoolean(KEY_SIGHT, false))
        private set

    fun updateOriginalSight(value: Boolean) {
        originalSight = value
        prefs.edit().putBoolean(KEY_SIGHT, value).apply()
        NativeBridge.setSightStyle(if (value) 1 else 0)
    }

    /**
     * How many particles may be alight at once - flame trails, explosions,
     * smoke, and a napalm field's fire, which is far and away the biggest
     * consumer. Upstream's own setting and upstream's own three sizes: 100 at
     * low, 6000 at normal, 10000 at high (ScorchedClient.cpp).
     *
     * Defaults to high. Upstream's own default is normal, so this is the one
     * place these two rules pull apart - but high is still upstream's number
     * rather than an invention, and the setting exists precisely so the
     * player decides where to sit on that trade rather than inheriting a
     * compromise nobody picked. Low is a real rescue for a device that cannot
     * draw a burning map.
     */
    var effectsDetail by mutableIntStateOf(prefs.getInt(KEY_EFFECTS, 2))
        private set

    fun updateEffectsDetail(value: Int) {
        effectsDetail = value.coerceIn(0, 2)
        prefs.edit().putInt(KEY_EFFECTS, effectsDetail).apply()
        NativeBridge.setEffectsDetail(effectsDetail)
    }

    /**
     * The sun's shadow map, which is what puts an island's shadow on the sea
     * beside it and a tank's on the ground. Upstream's own two sizes, plus
     * off.
     *
     * Off is not unlit: it is upstream's own fallback for hardware without
     * shadows, where the sun and the shadows hills cast on each other are
     * baked into the ground texture instead. Changing this rebuilds that
     * texture, so it takes a moment and cannot be done twice a frame.
     */
    var shadowDetail by mutableIntStateOf(prefs.getInt(KEY_SHADOWS, 2))
        private set

    fun updateShadowDetail(value: Int) {
        shadowDetail = value.coerceIn(0, 2)
        prefs.edit().putInt(KEY_SHADOWS, shadowDetail).apply()
        NativeBridge.setShadowDetail(shadowDetail)
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
        SoundPlayer.masterVolume = effectsVolume
        music?.let { it.volume = musicVolume; it.enabled = musicEnabled }
        ambient?.let { it.enabled = ambientEnabled; it.volume = ambientVolume }
        NativeBridge.setRenderOptions(showTrees, showFog)
        NativeBridge.setSightStyle(if (originalSight) 1 else 0)
        NativeBridge.setTerrainDetail(terrainDetail)
        NativeBridge.setWaterDetail(waterDetail)
        NativeBridge.setReflectionStyle(reflectionLevel)
        NativeBridge.setEffectsDetail(effectsDetail)
        NativeBridge.setShadowDetail(shadowDetail)
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
        const val KEY_AMBIENT = "sound.ambient"
        const val KEY_PLATES = "hud.namePlates"
        const val KEY_HEALTH = "hud.healthBars"
        const val KEY_TOAST = "hud.chatToastSeconds"
        const val KEY_INVERT = "controls.invertDrag"
        const val KEY_TAP_AIM = "controls.tapToAim"
        const val KEY_LEFT_HAND = "controls.leftHand"
        const val KEY_OPACITY = "controls.opacity"
        const val KEY_TREES = "graphics.trees"
        const val KEY_FOG = "graphics.fog"
        const val KEY_SIGHT = "graphics.originalSight"
        const val KEY_DETAIL = "graphics.terrainDetail"
        const val KEY_SOUND_VOLUME = "audio.effectsVolume"
        const val KEY_AMBIENT_VOLUME = "audio.ambientVolume"
        const val KEY_WATER_DETAIL = "graphics.waterDetail"
        // A new key, not the old boolean one: this started as an on/off
        // switch, and reading an existing Boolean with getInt throws
        // ClassCastException on the first launch after the upgrade - which
        // is exactly what it did here. The old key is simply left behind.
        const val KEY_REFLECT = "graphics.reflectionLevel"
        const val KEY_EFFECTS = "graphics.effectsDetail"
        const val KEY_SHADOWS = "graphics.shadowDetail"
        // The maximum the renderer allows - see kTerrainGridMax.
        const val DEFAULT_DETAIL = 256
    }
}
