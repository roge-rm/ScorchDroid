package com.rm.scorchdroid.web

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.ComposeViewport
import com.rm.scorchdroid.GameSettings
import com.rm.scorchdroid.NativeBridge
import com.rm.scorchdroid.readEngineText
import kotlinx.coroutines.delay

// ScorchDroid in a browser. The page has already loaded the engine
// (globalThis.sd) and mounted the saves folder from browser storage. This does
// what MainActivity.onCreate does on a phone, in the same order, then shows
// the menu.

/** Where the page put the game data (web/engine/CMakeLists.txt). */
private const val DATA_ROOT = "/scorched_root"

private fun jsPageReady(): Unit = js("globalThis.sdUiReady && globalThis.sdUiReady()")

@OptIn(ExperimentalComposeUiApi::class)
fun main() {
    val sound = WebSound(DATA_ROOT)
    val prefs = LocalStore("scorchdroid")
    val controller = WebController(GameSettings(LocalStore("scorchdroid.settings"), sound), sound, prefs)

    controller.splashStatus = "Starting engine..."
    val initOk = NativeBridge.initEngine(DATA_ROOT)
    if (!initOk) {
        controller.splashStatus = "Failed to initialise engine data root"
    } else {
        controller.licenseText = readEngineText("$DATA_ROOT/licenses/GPL-2.0.txt")
            ?: "Couldn't load the GNU General Public License v2 text. You can read it at " +
            "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html"
        controller.onEngineReady(DATA_ROOT, WebMusic(DATA_ROOT), WebAmbient(DATA_ROOT, sound))
    }

    ComposeViewport("root") {
        // No theme around it, as on the phone: the screens bring their own colours.
        LaunchedEffect(Unit) { jsPageReady() }
        // Skiko clears its canvas to white before every frame, and the game is
        // drawn on the canvas behind this one. Clearing that white away first
        // leaves the battlefield showing wherever the HUD doesn't draw.
        Box(Modifier.fillMaxSize().drawBehind { drawRect(Color.Transparent, blendMode = BlendMode.Clear) }) {
            controller.Content()
            Toast(controller)
        }
    }
}

/** The phone's Toast, for the few words the game says after an action. */
@Composable
private fun Toast(controller: WebController) {
    val (text, shownAt) = controller.toast ?: return
    LaunchedEffect(shownAt) {
        delay(2500)
        controller.dismissToast()
    }
    Box(Modifier.fillMaxSize().padding(bottom = 96.dp), contentAlignment = Alignment.BottomCenter) {
        Text(
            text,
            color = Color.White,
            modifier = Modifier
                .widthIn(max = 420.dp)
                .background(Color(0xE0303030), RoundedCornerShape(20.dp))
                .padding(horizontal = 16.dp, vertical = 10.dp),
        )
    }
}
