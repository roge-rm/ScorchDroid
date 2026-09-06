package com.rm.scorchdroid

import android.opengl.GLSurfaceView
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * M2 vertical slice: a minimal GLES3 renderer (see renderer_jni.cpp) drawing
 * the real, live heightmap/tank state from the running ScorchedServer
 * instance. All GL calls happen on the GL thread via these callbacks, as
 * GLSurfaceView requires.
 */
class GameRenderer : GLSurfaceView.Renderer {
    external fun nativeOnSurfaceCreated()
    external fun nativeOnSurfaceChanged(width: Int, height: Int)
    external fun nativeOnDrawFrame()

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
