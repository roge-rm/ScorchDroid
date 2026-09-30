package com.rm.scorchdroid.web

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerButton
import androidx.compose.ui.input.pointer.PointerEventType
import androidx.compose.ui.input.pointer.pointerInput
import com.rm.scorchdroid.AppScreen
import com.rm.scorchdroid.GameController
import com.rm.scorchdroid.GameRenderer
import com.rm.scorchdroid.GameSettings
import com.rm.scorchdroid.HudDialog
import com.rm.scorchdroid.KeyValueStore
import com.rm.scorchdroid.MultiplayerScreen
import com.rm.scorchdroid.NativeBridge
import com.rm.scorchdroid.PlateText
import com.rm.scorchdroid.SoundEffects
import com.rm.scorchdroid.nowMillis
import kotlin.math.abs
import kotlin.math.hypot
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

// The game surface: a canvas of its own behind the Compose one, made fresh
// for each game like the phone's GLSurfaceView (see GameController.attachGameSurface).
private fun jsCreateGameCanvas(): Int = js("globalThis.sdGame.create()")
private fun jsDestroyGameCanvas(): Unit = js("globalThis.sdGame.destroy()")
// The canvas's size in device pixels as width * 65536 + height when it
// changed since the last call, or -1.
private fun jsFitGameCanvas(): Int = js("globalThis.sdGame.fit()")
private fun jsDevicePixelRatio(): Float = js("window.devicePixelRatio || 1")
private fun jsPlateText(text: String, scale: Float): JsAny? = js("globalThis.sdGame.plateText(text, scale)")
private fun jsPlateWidth(p: JsAny): Int = js("p.width")
private fun jsPlateHeight(p: JsAny): Int = js("p.height")
private fun jsPlateData(p: JsAny): JsAny = js("p.data")
private fun jsFsChanged(): Unit = js("globalThis.sdFsChanged && globalThis.sdFsChanged()")
// The relay on the server this page came from, if it came from one.
private fun jsPageServer(): String? = js("globalThis.sdPageServer ? globalThis.sdPageServer() : null")
// An https page can only open wss://.
private fun jsPageSecure(): Boolean = js("location.protocol === 'https:'")

/** The game in a browser: it can join a server, but not host one. */
class WebController(
    settings: GameSettings,
    sound: SoundEffects,
    private val prefs: KeyValueStore,
) : GameController(settings, sound) {
    override val versionName: String = BuildInfo.VERSION_NAME
    override val upstreamCommit: String = BuildInfo.UPSTREAM_COMMIT
    override val isDebugBuild: Boolean = false

    /** A brief word to the player, shown at the bottom for a couple of seconds. */
    var toast by mutableStateOf<Pair<String, Long>?>(null)
        private set

    override fun notifyPlayer(text: String) {
        toast = text to nowMillis()
    }

    fun dismissToast() {
        toast = null
    }

    override fun attachGameSurface() {
        jsCreateGameCanvas()
        GameRenderer.nativeSetUiDensity(jsDevicePixelRatio())
        GameRenderer.nativeOnSurfaceCreated()
        fitSurface()
    }

    override fun removeGameSurface() {
        jsDestroyGameCanvas()
    }

    private fun fitSurface() {
        val packed = jsFitGameCanvas()
        if (packed >= 0) GameRenderer.nativeOnSurfaceChanged(packed ushr 16, packed and 0xffff)
    }

    override fun drawPlateText(text: String): PlateText? {
        val picture = jsPlateText(text, jsDevicePixelRatio()) ?: return null
        val width = jsPlateWidth(picture)
        val height = jsPlateHeight(picture)
        // RGBA bytes from the 2D canvas, as the ARGB Ints the renderer takes.
        val rgba = com.rm.scorchdroid.bytesOf(jsPlateData(picture))
        val pixels = IntArray(width * height) { i ->
            val r = rgba[i * 4].toInt() and 0xff
            val g = rgba[i * 4 + 1].toInt() and 0xff
            val b = rgba[i * 4 + 2].toInt() and 0xff
            val a = rgba[i * 4 + 3].toInt() and 0xff
            (a shl 24) or (r shl 16) or (g shl 8) or b
        }
        return PlateText(width, height, pixels)
    }

    override fun onSavesChanged() = jsFsChanged()

    // --- Joining -------------------------------------------------------------

    @Composable
    override fun MultiplayerMenu() {
        MultiplayerScreen(
            onHost = {},
            onHostBluetooth = {},
            onLoadGame = {},
            loadGameEnabled = false,
            onJoin = { startJoinFlow() },
            onJoinBluetooth = {},
            onBack = { appScreen = AppScreen.MENU },
            joinOnly = true,
            joinSubtitle = "Play with others on a ScorchDroid server",
        )
    }

    override fun startJoinFlow(overBluetooth: Boolean) {
        if (gameJob != null) return
        appScreen = AppScreen.JOINING
        hudState.statusText = "Pick a server to join"
        pickServer(
            onPicked = { join(it) },
            onCancelled = { appScreen = AppScreen.MULTIPLAYER },
        )
    }

    override fun onFindGames() {
        showMenuMessage(
            "To play with others, quit to the menu and pick Multiplayer, then Join Game. " +
                "A browser joins a ScorchDroid server, and phones can join the same one."
        )
    }

    private fun pickServer(onPicked: (String) -> Unit, onCancelled: () -> Unit) {
        val here = jsPageServer()
        val last = prefs.getString(KEY_LAST_SERVER, "")
        val rows = buildList {
            if (here != null) add("This server" to here)
            if (last.isNotEmpty() && last != here) add("Last time: $last" to last)
        }
        // A page on an https site, like the hosted one, can only open wss://,
        // and a server on a home network doesn't do TLS. Its own page does
        // plain http, so that's where to send people.
        val title = if (here == null && jsPageSecure()) {
            "Join a game\nThis page can only join servers that use https. For a server on your " +
                "own network, open its page instead, at http://<server>:8080/play/"
        } else {
            "Join a game"
        }
        hudState.dialog = HudDialog.ListChoice(
            title = title,
            items = rows.map { it.first } + "Enter an address...",
            cancelLabel = "Cancel",
            onSelect = { index ->
                hudState.dialog = HudDialog.None
                if (index < rows.size) onPicked(rows[index].second) else promptAddress(onPicked, onCancelled)
            },
            onCancel = {
                hudState.dialog = HudDialog.None
                onCancelled()
            },
        )
    }

    private fun promptAddress(onPicked: (String) -> Unit, onCancelled: () -> Unit) {
        hudState.dialog = HudDialog.ManualAddress(
            onConnect = { text ->
                hudState.dialog = HudDialog.None
                val url = serverUrl(text)
                if (url != null) onPicked(url) else onCancelled()
            },
            onCancel = {
                hudState.dialog = HudDialog.None
                onCancelled()
            },
        )
    }

    private fun join(url: String) {
        prefs.putString(KEY_LAST_SERVER, url)
        gameJob = scope.launch {
            hudState.statusText = "Connecting to $url..."
            val connecting = NativeBridge.startJoinGameBluetooth(url)
            if (!connecting) {
                hudState.statusText = "Couldn't reach $url. Tap Cancel to go back."
                return@launch
            }
            showJoinedGame()
            awaitJoinAndPlay()
        }
    }

    // --- The battlefield ---------------------------------------------------------

    @Composable
    override fun Battlefield() {
        // The game's own frames, drawn on Compose's frame clock so the two
        // canvases move together.
        LaunchedEffect(Unit) {
            while (true) {
                withFrameNanos { }
                fitSurface()
                GameRenderer.nativeOnDrawFrame()
            }
        }
        Box(
            Modifier.fillMaxSize()
                .pointerInput(Unit) { handleWheel() }
                .pointerInput(Unit) { handlePointer() },
        )
    }

    // Where the mouse is over the battlefield, for the "aim at point" key.
    private var hover: Pair<Float, Float>? = null
    override val hoverPoint: Pair<Float, Float>? get() = hover

    private suspend fun androidx.compose.ui.input.pointer.PointerInputScope.handleWheel() {
        awaitPointerEventScope {
            while (true) {
                val event = awaitPointerEvent()
                when (event.type) {
                    PointerEventType.Move, PointerEventType.Enter ->
                        event.changes.firstOrNull()?.position?.let { hover = it.x to it.y }
                    PointerEventType.Exit -> hover = null
                }
                if (event.type != PointerEventType.Scroll) continue
                val dy = event.changes.firstOrNull()?.scrollDelta?.y ?: 0f
                // One step in or out per wheel event. How big a delta a notch
                // is differs between browsers, mice and touchpads, so its size
                // only decides the direction.
                if (dy != 0f) GameRenderer.nativeCameraZoom(if (dy < 0f) WHEEL_STEP else 1f / WHEEL_STEP)
                event.changes.forEach { it.consume() }
            }
        }
    }

    /**
     * What the phone's touch listener does (MainActivity.setUpCameraControls),
     * for a mouse as well as a finger: a drag orbits, two fingers or the right
     * button pan, two fingers pinch to zoom, a tap aims, and holding still on
     * a tank's plate opens its card.
     */
    @OptIn(ExperimentalComposeUiApi::class)
    private suspend fun androidx.compose.ui.input.pointer.PointerInputScope.handlePointer() {
        val slop = viewConfiguration.touchSlop
        val longPress = viewConfiguration.longPressTimeoutMillis
        awaitPointerEventScope {
            while (true) {
                val down = awaitPointerEvent()
                if (down.type != PointerEventType.Press) continue
                val first = down.changes.first()
                val start = first.position
                var last = start
                var travelled = 0f
                var multiTouched = false
                var plateHeld = false
                val panButton = down.button == PointerButton.Secondary || down.button == PointerButton.Tertiary
                var lastSpan = 0f
                var lastFocus = Offset.Zero
                val plate = settings.showTankInfo && GameRenderer.nativePickTankPlate(start.x, start.y) != 0
                val pressedAt = nowMillis()

                while (true) {
                    // Over a plate, a press that sits still long enough opens the card.
                    val event = if (plate && !plateHeld && !multiTouched && travelled <= slop) {
                        val left = longPress - (nowMillis() - pressedAt)
                        withTimeoutOrNull(left.coerceAtLeast(1)) { awaitPointerEvent() } ?: run {
                            plateHeld = true
                            val tankId = GameRenderer.nativePickTankPlate(start.x, start.y)
                            if (tankId != 0) showTankInfo(tankId)
                            null
                        }
                    } else {
                        awaitPointerEvent()
                    }
                    if (event == null) continue
                    val pressed = event.changes.filter { it.pressed }
                    if (pressed.isEmpty()) {
                        if (!multiTouched && !plateHeld && travelled <= slop && !panButton) {
                            handleBattlefieldTap(last.x, last.y)
                        }
                        break
                    }
                    if (pressed.size >= 2) {
                        val a = pressed[0].position
                        val b = pressed[1].position
                        val focus = Offset((a.x + b.x) / 2, (a.y + b.y) / 2)
                        val span = hypot(a.x - b.x, a.y - b.y)
                        if (multiTouched && lastSpan > 0f) {
                            GameRenderer.nativeCameraPan(focus.x - lastFocus.x, focus.y - lastFocus.y)
                            if (span > 0f) GameRenderer.nativeCameraZoom(span / lastSpan)
                        }
                        multiTouched = true
                        lastSpan = span
                        lastFocus = focus
                    } else if (!multiTouched) {
                        val now = pressed[0].position
                        val dx = now.x - last.x
                        val dy = now.y - last.y
                        travelled += abs(dx) + abs(dy)
                        if (travelled > slop) {
                            if (panButton) {
                                GameRenderer.nativeCameraPan(dx, dy)
                            } else {
                                GameRenderer.nativeCameraDrag(dx, dy * if (settings.invertDrag) -1f else 1f)
                            }
                        }
                        last = now
                    }
                    event.changes.forEach { it.consume() }
                }
            }
        }
    }

    private companion object {
        const val KEY_LAST_SERVER = "join.lastServer"
        // What one wheel step zooms by: about what a small pinch does.
        const val WHEEL_STEP = 1.1f
        // The web container's port, and the path its relay listens on
        // (web-admin/app/play.py).
        const val DEFAULT_WEB_PORT = 8080
        const val RELAY_PATH = "/play/ws"

        /**
         * What a player typed, as the relay's address: a ws:// or wss:// URL
         * as it is, an http(s):// one turned into its WebSocket twin, and a
         * bare host with the web port and relay path filled in.
         */
        fun serverUrl(typed: String): String? {
            val text = typed.trim().trimEnd('/')
            if (text.isEmpty()) return null
            if (text.startsWith("ws://") || text.startsWith("wss://")) {
                return if (text.substringAfter("//").contains('/')) text else text + RELAY_PATH
            }
            if (text.startsWith("http://") || text.startsWith("https://")) {
                val secure = text.startsWith("https://")
                val rest = text.substringAfter("//")
                val hostPort = rest.substringBefore('/')
                return (if (secure) "wss://" else "ws://") + hostPort + RELAY_PATH
            }
            val hostPort = if (text.contains(':')) text else "$text:$DEFAULT_WEB_PORT"
            return (if (jsPageSecure()) "wss://" else "ws://") + hostPort + RELAY_PATH
        }
    }
}
