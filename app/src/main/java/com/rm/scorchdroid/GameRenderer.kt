package com.rm.scorchdroid

import android.opengl.GLSurfaceView
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * M6: a real 3D GLES3 renderer (see renderer_jni.cpp) drawing the real,
 * live heightmap/tank state from the running ScorchedServer/ClientContext
 * instance as an actual terrain mesh with lighting, not the earlier M2/M5
 * flat top-down view. All GL calls happen on the GL thread via these
 * callbacks, as GLSurfaceView requires - nativeCameraDrag/nativeCameraZoom
 * are the exception, safe to call from the UI thread (touch handling in
 * MainActivity), since the native side guards camera state with its own
 * mutex separate from the GL-thread/sim-thread one.
 */
class GameRenderer : GLSurfaceView.Renderer {
    external fun nativeOnSurfaceCreated()
    external fun nativeOnSurfaceChanged(width: Int, height: Int)
    external fun nativeOnDrawFrame()
    /**
     * M6 name plates: the last frame's tank positions projected to screen
     * pixels, one row each:
     * "screenX|screenY|onScreen|alive|mine|life|shield|r|g|b|name".
     *
     * Projected natively because the renderer is the only place with the
     * finished MVP - redoing the camera maths in Kotlin would be a second
     * implementation to keep in step. The *text* stays on this side: this
     * port has no GL font renderer and its UI layer is Compose.
     */
    external fun nativeGetTankOverlays(): Array<String>

    /**
     * M6: turns a screen tap into a landscape "x|y", or "" if the ray
     * misses the ground. Rebuilds the pick ray from the camera basis the
     * renderer published last frame rather than inverting the MVP.
     */
    external fun nativePickTerrain(screenX: Float, screenY: Float): String

    /**
     * Development readout: "fps|drawCalls|targets" for the last complete
     * frame. Draw calls are counted at every glDraw* site rather than
     * estimated, because the number worth knowing - one per landscape
     * target - is the easy one to be wrong about by an order of magnitude.
     *
     * Not a player-facing feature; see GameHudState.perfLabel for where to
     * gate it before a release.
     */
    external fun nativeGetFrameStats(): String

    external fun nativeCameraDrag(dx: Float, dy: Float)
    external fun nativeCameraZoom(scaleFactor: Float)

    /**
     * Slides the free-fly camera's look-at point across the ground, in
     * screen-relative pixels (two-finger drag - see
     * MainActivity.setUpCameraControls). A no-op in follow mode, which
     * retargets to your tank every frame and would overwrite any pan on the
     * very next one.
     */
    external fun nativeCameraPan(dx: Float, dy: Float)

    // Toggles free-fly (orbit the map) vs. third-person-follow (orbit "my
    // tank") - see renderer_jni.cpp. Returns the new mode (true = follow).
    external fun nativeToggleCameraMode(): Boolean

    /**
     * M6 parity: selects one of upstream's camera presets
     * (TargetCamera::CamType). See [CameraPreset]. A drag drops back out of
     * a fixed preset, so this is a framing, not a mode lock.
     */
    external fun nativeSetCameraPreset(preset: Int)

    /**
     * M6: short-lived labels anchored to a world position - floating damage
     * numbers and speech bubbles - already projected to screen space. See
     * [parseFloatingLabels].
     */
    external fun nativeGetFloatingLabels(): Array<String>

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        nativeOnSurfaceCreated()
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        nativeOnSurfaceChanged(width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        nativeOnDrawFrame()
    }
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
