package com.rm.scorchdroid

/**
 * The renderer's calls (renderer_jni.cpp), on the phone and in the browser.
 * Kept as a class of their own named GameRenderer, so the JNI names stay
 * Java_com_rm_scorchdroid_GameRenderer_*. On the phone, GlRenderer is what
 * GLSurfaceView calls, and it calls these. Generated like [NativeBridge].
 */
expect object GameRenderer {

    fun nativeOnSurfaceCreated()
    fun nativeOnSurfaceChanged(width: Int, height: Int)
    fun nativeOnDrawFrame()
    /**
     * The names the plate pass wants a picture of. The renderer draws the
     * plates itself now - in the frame it projected them for, so they cannot
     * lag - but it has no font, which is the one thing it still needs from
     * this side. It asks for what it is missing; [nativeSetPlateText] answers.
     */
    fun nativeGetMissingPlateTexts(): Array<String>

    /** One string's picture, ARGB_8888 as [android.graphics.Bitmap.getPixels] gives it. */
    fun nativeSetPlateText(text: String, width: Int, height: Int, pixels: IntArray)

    /**
     * Whose name plate is at this point on screen, or 0 for none. The
     * rectangles come from the plate pass, so the target is what the player
     * can actually see. Only a *hold* on one opens a card: a tap, there as
     * anywhere else, aims.
     */
    fun nativePickTankPlate(screenX: Float, screenY: Float): Int

    /**
     * A1: the projectile engine loops that should be playing, one row each:
     * `"key|file|gain|pan"`. Upstream keeps one looping, positioned source
     * per shell in flight; this is that list, already attenuated and panned
     * against the live listener. Empty whenever nothing is in the air.
     */
    fun nativeGetSoundLoops(): Array<String>

    /** dp to px, for the plate layout - only this side knows it. */
    fun nativeSetUiDensity(density: Float)

    /**
     * M6: turns a screen tap into a landscape "x|y", or "" if the ray
     * misses the ground. Rebuilds the pick ray from the camera basis the
     * renderer published last frame rather than inverting the MVP.
     */
    fun nativePickTerrain(screenX: Float, screenY: Float): String

    /**
     * Development readout: "fps|drawCalls|targets" for the last complete
     * frame. Draw calls are counted at every glDraw* site rather than
     * estimated, because the number worth knowing - one per landscape
     * target - is the easy one to be wrong about by an order of magnitude.
     *
     * Not a player-facing feature; see GameHudState.perfLabel for where to
     * gate it before a release.
     */
    fun nativeGetFrameStats(): String

    fun nativeCameraDrag(dx: Float, dy: Float)
    fun nativeCameraZoom(scaleFactor: Float)

    /**
     * Slides the free-fly camera's look-at point across the ground, in
     * screen-relative pixels (two-finger drag - see
     * MainActivity.setUpCameraControls). A no-op in follow mode, which
     * retargets to your tank every frame and would overwrite any pan on the
     * very next one.
     */
    fun nativeCameraPan(dx: Float, dy: Float)

    // Toggles free-fly (orbit the map) vs. third-person-follow (orbit "my
    // tank") - see renderer_jni.cpp. Returns the new mode (true = follow).
    fun nativeToggleCameraMode(): Boolean

    /**
     * M6 parity: selects one of upstream's camera presets
     * (TargetCamera::CamType). See [CameraPreset]. A drag drops back out of
     * a fixed preset, so this is a framing, not a mode lock.
     */
    fun nativeSetCameraPreset(preset: Int)

    /**
     * The mini-map's picture, which is upstream's plan view: the landscape's
     * own ground texture downsampled, with the sea turned into transparency
     * rather than colour.
     *
     * Two calls because the image changes a handful of times a round while
     * the HUD asks every tick. [nativeMiniMapVersion] is an atomic read;
     * [nativeMiniMapImage] copies 64KB and is only worth calling when the
     * version has moved. Empty between landscapes, so the old map is dropped
     * rather than left over the new one.
     */
    fun nativeMiniMapVersion(): Int

    /** ARGB_8888 rows in landscape order - see [nativeMiniMapVersion]. */
    fun nativeMiniMapImage(): IntArray

    /**
     * "lookX|lookY|dirX|dirY" in landscape coordinates, for the plan view's
     * camera arrow (upstream's GLWPlanView::drawCameraPointer).
     */
    fun nativeCameraPlanInfo(): String

    /**
     * Points the camera at a spot on the landscape, and drops into free look
     * so that it stays there - what tapping the mini-map does, and what
     * upstream does on a left-click in its plan view.
     */
    fun nativeCameraLookAt(landscapeX: Float, landscapeY: Float)

    /**
     * M6: short-lived labels anchored to a world position - floating damage
     * numbers and speech bubbles - already projected to screen space. See
     * [parseFloatingLabels].
     */
    fun nativeGetFloatingLabels(): Array<String>
}

/**
 * The camera presets, matching renderer_jni.cpp's OrbitCamera::Preset. FREE
 * and FOLLOW are this port's own two - the camera button's states - and the
 * rest are upstream's own fixed framings of the current tank
 * (TargetCamera::CamType).
 */
enum class CameraPreset(val label: String) {
    FREE("Free look"),
    FOLLOW("Follow your tank"),
    TOP("Top down"),
    BEHIND("Above and behind"),
    TANK("From the tank"),
    ACTION("Action"),
    SHOT("Follow the shot"),
}

/** One world-anchored label, projected to screen space by the renderer. */
data class FloatingLabel(
    val screenX: Float,
    val screenY: Float,
    val onScreen: Boolean,
    val fade: Float,
    val color: androidx.compose.ui.graphics.Color,
    val text: String,
)

fun parseFloatingLabels(rows: Array<String>): List<FloatingLabel> = rows.mapNotNull { row ->
    val p = row.split("|", limit = 8)
    if (p.size != 8) return@mapNotNull null
    FloatingLabel(
        screenX = p[0].toFloatOrNull() ?: return@mapNotNull null,
        screenY = p[1].toFloatOrNull() ?: return@mapNotNull null,
        onScreen = p[2] == "1",
        fade = p[3].toFloatOrNull() ?: 1f,
        color = androidx.compose.ui.graphics.Color(
            red = p[4].toFloatOrNull() ?: 1f,
            green = p[5].toFloatOrNull() ?: 1f,
            blue = p[6].toFloatOrNull() ?: 1f,
            alpha = 1f,
        ),
        text = p[7],
    )
}
