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
