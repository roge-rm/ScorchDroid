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
