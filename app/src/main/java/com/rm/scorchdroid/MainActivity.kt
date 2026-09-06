package com.rm.scorchdroid

import android.opengl.GLSurfaceView
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {
    private lateinit var gameSurface: GLSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        val statusText = findViewById<TextView>(R.id.status_text)
        statusText.text = NativeBridge.helloFromNative()

        gameSurface = findViewById(R.id.game_surface)
        gameSurface.setEGLContextClientVersion(3)
        gameSurface.setRenderer(GameRenderer())
        gameSurface.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY

        CoroutineScope(Dispatchers.Main).launch {
            statusText.text = "Extracting game data..."
            val dataRoot = withContext(Dispatchers.IO) {
                AssetDataExtractor.ensureExtracted(applicationContext)
            }
            val initOk = withContext(Dispatchers.Default) {
                NativeBridge.initEngine(dataRoot.absolutePath)
            }
            if (!initOk) {
                statusText.text = "Failed to initialize engine data root"
                return@launch
            }

            statusText.text = "Starting local game..."
            val gameOk = withContext(Dispatchers.Default) {
                NativeBridge.startLocalGame()
            }
            if (!gameOk) {
                statusText.text = "Failed to start local game (see logcat)"
                return@launch
            }

            // Drives the real ServerSimulator tick loop (see the porting
            // plan) - this is the same role the dedicated server's own
            // main loop plays on desktop, so the ServerState machine
            // (waiting for players -> new level -> buying -> playing)
            // progresses on its own as bots get added. Rendering happens
            // separately, driven by GLSurfaceView's own thread (GameRenderer).
            while (isActive) {
                withContext(Dispatchers.Default) {
                    NativeBridge.tickEngine()
                }
                statusText.text = withContext(Dispatchers.Default) {
                    NativeBridge.getGameStateDebugString()
                }
                kotlinx.coroutines.delay(100)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (::gameSurface.isInitialized) gameSurface.onResume()
    }

    override fun onPause() {
        super.onPause()
        if (::gameSurface.isInitialized) gameSurface.onPause()
    }
}
