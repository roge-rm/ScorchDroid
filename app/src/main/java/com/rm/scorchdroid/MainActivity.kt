package com.rm.scorchdroid

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.util.TypedValue
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.MotionEvent
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.ComposeView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections
import kotlin.math.ceil
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The phone's shell around [GameController]: the Activity, its GL surface and
 * touch handling, and everything multiplayer needs from Android - Wi-Fi
 * Direct, Bluetooth and LAN discovery, with their permissions and system
 * prompts. The game itself, menus and HUD included, is shared with the
 * browser build and lives in the shared module.
 */
class MainActivity : AppCompatActivity() {
    private lateinit var gameSurface: GLSurfaceView
    private lateinit var gameRenderer: GameRenderer

    // M9: the container the GL surface is added to when a game starts and
    // removed from on quit - see startGame/quitToMenu.
    private lateinit var surfaceHost: android.widget.FrameLayout

    // Wi-Fi Direct's discovery permission (see WifiDirectTransport). The
    // first runtime permission this game has ever asked for, and asked for
    // once, on the way into multiplayer - not at launch, where a player who
    // only ever plays solo would be handed a prompt for something they will
    // never use, and not at the moment of hosting, where the system dialog
    // would land on top of a game that has already started.
    //
    // Nothing is gated on the answer: refusing costs the Wi-Fi Direct rows in
    // "Find Games" and nothing else, so the result is only worth acting on to
    // the extent of not asking twice.
    private var askedForNearbyPermission = false
    private val nearbyPermissionLauncher = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestMultiplePermissions()
    ) { /* Either way the game works; see above. */ }

    // Bluetooth's are asked for separately and only when the player chooses
    // to host over it: they are three permissions on a modern device, and
    // asking for them on the way into multiplayer would put a prompt in
    // front of everyone who only ever plays over Wi-Fi.
    private val bluetoothPermissionLauncher = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestMultiplePermissions()
    ) { granted ->
        val next = afterBluetoothReady
        afterBluetoothReady = null
        if (granted.values.all { it }) {
            next?.invoke()
        } else {
            // Refused, which is a choice and not a fault - but a silent
            // return to the menu would read as the button being broken.
            controller.showMenuMessage(
                "Bluetooth play needs the nearby devices permission. You can turn it " +
                    "on in Android Settings under Apps > ScorchDroid > Permissions."
            )
        }
    }

    // What to do once Bluetooth is usable, held across the system prompt that
    // makes it so. Never more than one: these are menu taps.
    private var afterBluetoothReady: (() -> Unit)? = null

    private lateinit var controller: Controller

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The transport is reached from C++ with no context of its own (see
        // JniTransport.cpp), so it is given one once, here.
        BluetoothTransport.init(applicationContext)
        hideSystemBars()
        // A round can sit idle for a while waiting on other players/bots,
        // with no touch input in between - without this the screen times
        // out mid-game exactly like any other idle app.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)

        surfaceHost = findViewById(R.id.game_surface_host)
        val store = SharedPreferencesStore(getSharedPreferences("scorchdroid.settings", MODE_PRIVATE))
        controller = Controller(GameSettings(store, SoundPlayer))
        controller.hudState.statusText = NativeBridge.helloFromNative()

        findViewById<ComposeView>(R.id.hud_compose_view).setContent {
            // M9: the system back gesture walks the menu back up a level.
            // Enabled only off the main menu, so back there still leaves the
            // app as Android expects.
            BackHandler(enabled = controller.backEnabled) { controller.onBack() }
            controller.Content()
        }

        // First run has real work to do - extracting upstream's ~90MB data/
        // tree - so the splash stays up until the engine has a data root.
        CoroutineScope(Dispatchers.Main).launch {
            controller.splashStatus = "Extracting game data..."
            val dataRoot = withContext(Dispatchers.IO) {
                AssetDataExtractor.ensureExtracted(applicationContext)
            }
            controller.splashStatus = "Starting engine..."
            val initOk = withContext(Dispatchers.Default) {
                NativeBridge.initEngine(dataRoot.absolutePath)
            }
            if (!initOk) {
                controller.splashStatus = "Failed to initialise engine data root"
                return@launch
            }
            controller.licenseText = withContext(Dispatchers.IO) { readLicenseText() }
            // Only now: the players cross into the engine, which has just
            // been given its data root.
            controller.onEngineReady(
                dataRoot.absolutePath,
                MusicPlayer(dataRoot),
                AmbientPlayer(dataRoot.absolutePath),
            )
        }
    }

    /**
     * M9: the licence text the About screen shows, staged into the APK from
     * the repository's own LICENSE (see stageLicense in build.gradle.kts).
     * Read once, off the main thread - it is ~18KB.
     */
    private fun readLicenseText(): String = try {
        assets.open("licenses/GPL-2.0.txt").bufferedReader().use { it.readText() }
    } catch (e: Exception) {
        // The notice above it in the About screen is written out in full and
        // stands on its own, so a missing asset degrades rather than misleads.
        "Couldn't load the GNU General Public License v2 text. You can read it " +
            "at https://www.gnu.org/licenses/old-licenses/gpl-2.0.html " +
            "or in the LICENSE file in the source code."
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: android.content.Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        controller.onActivityResult(requestCode, resultCode)
    }

    override fun onResume() {
        super.onResume()
        if (controller.appScreen == AppScreen.GAME && ::gameSurface.isInitialized) gameSurface.onResume()
        controller.music?.resume()
        controller.ambient?.resume()
    }

    override fun onPause() {
        super.onPause()
        if (controller.appScreen == AppScreen.GAME && ::gameSurface.isInitialized) gameSurface.onPause()
        controller.music?.pause()
        controller.ambient?.pause()
    }

    override fun onDestroy() {
        controller.music?.release()
        controller.ambient?.release()
        controller.music = null
        super.onDestroy()
        controller.stopAdvertising()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // System bars can reappear (a swipe-reveal, a dialog, the user
        // switching apps and back) - re-hide whenever focus returns, the
        // standard pattern for a persistently-immersive app.
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowCompat.getInsetsController(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    /** The game, with what only a phone has added. */
    inner class Controller(settings: GameSettings) : GameController(settings, SoundPlayer) {
        override val versionName: String = BuildConfig.VERSION_NAME
        override val upstreamCommit: String = BuildConfig.UPSTREAM_COMMIT
        override val isDebugBuild: Boolean = BuildConfig.DEBUG

        override fun notifyPlayer(text: String) {
            Toast.makeText(this@MainActivity, text, Toast.LENGTH_SHORT).show()
        }

        /** For onDestroy, which is outside this class. */
        fun stopAdvertising() = stopNetworkAdvertising()

        override fun attachGameSurface() {
            val surface = GLSurfaceView(this@MainActivity).apply {
                setEGLContextClientVersion(3)
                // Only a hint - the driver may drop the context anyway under
                // memory pressure, and nativeOnSurfaceCreated copes when it does -
                // but when honoured a resume is instant instead of rebuilding the
                // terrain mesh, the ground texture and every model from scratch.
                preserveEGLContextOnPause = true
                // M6: the 3D renderer needs a real depth buffer (the M2/M5 flat
                // 2D view never did) and GLSurfaceView's default config chooser
                // doesn't reliably request one on every device.
                setEGLConfigChooser(8, 8, 8, 8, 16, 0)
            }
            // The plate pass lays out in dp, the same dp the Compose plates used,
            // and the renderer has no way of its own to know what a dp is here.
            GameRenderer.nativeSetUiDensity(resources.displayMetrics.density)
            surface.setRenderer(GlRenderer())
            // After setRenderer, never before: GLSurfaceView has no GL thread
            // until a renderer is attached, and setRenderMode dereferences it.
            surface.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            setUpCameraControls(surface)
            gameSurface = surface
            surfaceHost.addView(surface)
        }

        override fun removeGameSurface() {
            if (::gameSurface.isInitialized) surfaceHost.removeView(gameSurface)
        }

        /**
         * The renderer draws the name plates itself, but it has no font: upstream
         * has a GL font atlas there and this port never had one. So each name is
         * drawn here, once, into a bitmap the plate pass uploads and keeps.
         *
         * White on nothing, because the *tank's* colour is applied in the shader -
         * one picture serves a player whatever colour they are playing, and a
         * player who changes colour needs no new one.
         */
        private val plateTextPaint by lazy {
            android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
                // The size the Compose plates used (labelMedium), through the
                // same sp scaling, so nothing about them changed in the move.
                textSize = TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 12f, resources.displayMetrics,
                )
                typeface = android.graphics.Typeface.create(
                    "sans-serif-medium", android.graphics.Typeface.NORMAL,
                )
                color = android.graphics.Color.WHITE
            }
        }

        override fun drawPlateText(text: String): PlateText {
            val metrics = plateTextPaint.fontMetrics
            val height = ceil(metrics.descent - metrics.ascent).toInt().coerceIn(1, 256)
            val width = ceil(plateTextPaint.measureText(text)).toInt().coerceIn(1, 2048)
            val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            android.graphics.Canvas(bitmap).drawText(text, 0f, -metrics.ascent, plateTextPaint)
            val pixels = IntArray(width * height)
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height)
            bitmap.recycle()
            return PlateText(width, height, pixels)
        }

        override fun onFindGames() = showFindGames()

        @Composable
        override fun MultiplayerMenu() {
            MultiplayerScreen(
                onHost = { openSetup("Host Game", forOthers = true) },
                onHostBluetooth = { startBluetoothHostFlow() },
                onLoadGame = {
                    loadGameForOthers = true
                    appScreen = AppScreen.LOAD_GAME
                },
                loadGameEnabled = savedGames.isNotEmpty(),
                onJoin = { startJoinFlow() },
                onJoinBluetooth = { requireBluetooth { startJoinFlow(overBluetooth = true) } },
                onBack = { appScreen = AppScreen.MENU },
                bluetoothEnabled = BluetoothTransport.isSupported(applicationContext),
            )
        }

        override fun resetHostingPrompts() {
            bluetoothVisibilityAsked = false
        }

        override fun holdStartForHosting(): Boolean {
            // Bluetooth visibility is asked for here, not on the way into setup.
            // Android grants it for five minutes at most and the clock starts the
            // moment it is granted, so a player who spends two of them choosing
            // options and waiting for a landscape has two left for the other
            // phone to find them - which is how a host can be up and genuinely
            // invisible. Answering lands in onActivityResult, which comes back
            // to startGame().
            if (hostOverBluetooth && !bluetoothVisibilityAsked) {
                bluetoothVisibilityAsked = true
                try {
                    startActivityForResult(BluetoothTransport.discoverableIntent(), REQUEST_DISCOVERABLE)
                    return true
                } catch (e: android.content.ActivityNotFoundException) {
                    // No visibility prompt on this device at all. Same
                    // consequence as refusing one, and worth the same sentence.
                    showMenuMessage(
                        "This device can't be made visible over Bluetooth, so only phones " +
                            "already paired with it can find the game."
                    )
                }
            }
            return false
        }

        override fun startHostedSave(save: SavedGame) {
            // Hosted: the engine has one network interface, so which radio it
            // comes back on has to be settled before the game starts - the same
            // reason Host Game and Host over Bluetooth are two buttons rather
            // than one with an option inside it.
            val transports = buildList {
                add("Over Wi-Fi, a hotspot, or Wi-Fi Direct" to false)
                if (BluetoothTransport.isSupported(applicationContext)) {
                    add("Over Bluetooth" to true)
                }
            }
            if (transports.size == 1) {
                startHostedSavedGame(save, overBluetooth = false)
                return
            }
            hudState.dialog = HudDialog.ListChoice(
                title = "Host it how?",
                items = transports.map { it.first },
                cancelLabel = "Cancel",
                onSelect = { index ->
                    hudState.dialog = HudDialog.None
                    val overBluetooth = transports[index].second
                    if (overBluetooth) {
                        requireBluetooth { startHostedSavedGame(save, overBluetooth = true) }
                    } else {
                        startHostedSavedGame(save, overBluetooth = false)
                    }
                },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }

        fun onActivityResult(requestCode: Int, resultCode: Int) {
            if (requestCode == REQUEST_ENABLE_BLUETOOTH) {
                val next = afterBluetoothReady
                afterBluetoothReady = null
                if (BluetoothTransport.isOff(applicationContext)) {
                    // Refused, or it did not come up. Either way, saying so is
                    // the whole point - this silently did nothing before.
                    showMenuMessage(
                        "Bluetooth needs to be switched on. Turn it on and try again."
                    )
                } else {
                    // Straight on to whatever the player was heading for before
                    // the radio got in the way.
                    next?.invoke()
                }
                return
            }

            if (requestCode != REQUEST_DISCOVERABLE) return

            // resultCode is the number of seconds granted, or RESULT_CANCELED.
            // A refusal used to walk straight on into game setup, which is only
            // half defensible: a phone that is not visible can still be joined
            // by one it has already paired with, and not by anything else. So
            // say which of those the player is choosing rather than deciding for
            // them - and having said it, make asking again the easy answer.
            if (resultCode == RESULT_CANCELED) {
                hudState.dialog = HudDialog.ListChoice(
                    title = "This phone won't be visible.\nOnly devices already paired with " +
                        "it can find the game.",
                    items = listOf("Ask again", "Host anyway (paired devices only)"),
                    cancelLabel = "Back",
                    onSelect = { index ->
                        hudState.dialog = HudDialog.None
                        // Asking again means asking again, so the one-shot guard
                        // has to be let go of first.
                        if (index == 0) bluetoothVisibilityAsked = false
                        startGame()
                    },
                    onCancel = {
                        hudState.dialog = HudDialog.None
                        bluetoothVisibilityAsked = false
                        appScreen = AppScreen.SETUP
                    },
                )
                return
            }

            startGame()
        }

        // Whether this game has already put the visibility prompt up. Asked once
        // per game, since a second prompt between setup and the first round would
        // read as the first one not having worked.
        private var bluetoothVisibilityAsked = false

        /**
         * M10: find and connect to a game, staying on a menu screen while it
         * happens.
         *
         * The GL surface is deliberately not built until the connection is up.
         * Building it first - which is what happened when Join went through
         * startGame() - put the discovery dialog on top of the aiming sliders and
         * Fire button of a game that did not exist yet.
         */
        override fun startJoinFlow(overBluetooth: Boolean) {
            if (gameJob != null) return
            appScreen = AppScreen.JOINING
            hudState.statusText = if (overBluetooth) {
                "Looking for a device to join..."
            } else {
                "Looking for a game..."
            }
            gameJob = scope.launch {
                val target = pickJoinTarget(overBluetooth)
                if (target == null) {
                    gameJob = null
                    appScreen = AppScreen.MULTIPLAYER
                    return@launch
                }

                // A Wi-Fi Direct pick is not an address yet. Forming the group
                // takes several seconds and puts an invitation prompt on the
                // host's screen, so it gets its own status line - told that it is
                // "connecting to :27270", a player would reasonably think the
                // game had hung.
                // Bluetooth has no address to connect to at any point - the
                // whole game runs over RFCOMM - so it takes its own path into
                // the engine rather than being turned into a host and port.
                if (target.bluetoothAddress != null) {
                    hudState.statusText = "Connecting to ${target.name} over Bluetooth...\n" +
                        if (target.bluetoothPaired) {
                            "This can take a few seconds."
                        } else {
                            "Accept the pairing request on both devices if it pops up."
                        }
                    val connecting = withContext(Dispatchers.Default) {
                        NativeBridge.startJoinGameBluetooth(target.bluetoothAddress)
                    }
                    if (!connecting) {
                        hudState.statusText =
                            "Couldn't start a Bluetooth connection to ${target.name}. " +
                                "Tap Cancel to go back."
                        return@launch
                    }
                    // The same lines the network path ends with - there is
                    // nothing different about a joined client from here on,
                    // whatever carried it.
                    showJoinedGame()
                    awaitJoinAndPlay()
                    return@launch
                }

                val host = if (target.p2pDeviceAddress != null) {
                    // The prompt is worth mentioning: it lands on the *other*
                    // phone, which the player is not looking at, and ignoring it
                    // is indistinguishable from the connection failing.
                    hudState.statusText = "Asking ${target.name} to connect...\n" +
                        "Accept the invite on the other device if it pops up."
                    val result = WifiDirectTransport.connectToOwner(
                        applicationContext, target.p2pDeviceAddress
                    )
                    if (result.address == null) {
                        hudState.statusText = if (result.error != null) {
                            "Couldn't connect to ${target.name}: ${result.error}. " +
                                "Tap Cancel to go back."
                        } else {
                            "Couldn't form a Wi-Fi Direct group with ${target.name}. " +
                                "Tap Cancel to go back."
                        }
                        return@launch
                    }
                    result.address
                } else {
                    target.host
                }

                hudState.statusText = "Connecting to $host:${target.port}..."
                val connecting = withContext(Dispatchers.Default) {
                    NativeBridge.startJoinGame(host, target.port)
                }
                if (!connecting) {
                    hudState.statusText =
                        "Couldn't reach $host:${target.port}. Tap Cancel to go back."
                    return@launch
                }

                // Connected, so there is now a game to draw. A joined client is
                // never the host, so the admin button stays away.
                showJoinedGame()
                awaitJoinAndPlay()
            }
        }

        /**
         * Hosting over Bluetooth, which needs two things the network paths do
         * not: the permissions, and the system's own "make this device visible"
         * dialog. A host nobody can see is the Bluetooth equivalent of a group
         * that never formed, and a player would have no way of telling.
         *
         * Already-paired devices can find the game without this, which is why a
         * refused dialog is a warning rather than a failure.
         */
        /**
         * Runs [action] once Bluetooth is actually usable, asking for whatever is
         * missing first: the permissions, then the radio itself through Android's
         * own "turn Bluetooth on?" prompt. Both answers come back through
         * [onActivityResult] or the permission launcher, which is why the action
         * is held rather than passed down.
         *
         * Hosting and joining both need this and neither should be reciting it,
         * which is also how the two came to disagree about whether to ask at all.
         */
        private fun requireBluetooth(action: () -> Unit) {
            if (!BluetoothTransport.isSupported(applicationContext)) {
                showMenuMessage("This device has no Bluetooth.")
                return
            }
            if (!BluetoothTransport.hasPermissions(applicationContext)) {
                afterBluetoothReady = { requireBluetooth(action) }
                bluetoothPermissionLauncher.launch(BluetoothTransport.requiredPermissions())
                return
            }
            // Switched off is the one fixable case, so it is offered as a fix
            // rather than reported as a fault.
            if (BluetoothTransport.isOff(applicationContext)) {
                afterBluetoothReady = action
                try {
                    startActivityForResult(BluetoothTransport.enableIntent(), REQUEST_ENABLE_BLUETOOTH)
                } catch (e: android.content.ActivityNotFoundException) {
                    afterBluetoothReady = null
                    showMenuMessage("Bluetooth needs to be switched on first.")
                }
                return
            }
            val reason = BluetoothTransport.unavailableReason(applicationContext)
            if (reason != null) {
                showMenuMessage("Can't use Bluetooth: $reason.")
                return
            }
            action()
        }

        private fun startBluetoothHostFlow() = requireBluetooth {
            openSetup("Host over Bluetooth", overBluetooth = true, forOthers = true)
        }

        /**
         * The address to put on the HUD's one line: the port is left off when it
         * is the default, because a joiner typing a bare address now gets 27270
         * anyway (see promptManualAddress). That is what buys the line enough
         * room for a long local address without clipping - "Host
         * 192.168.232.2:27270" is thirty characters and the column has about
         * twenty-two, sharing its row with four icon buttons.
         */
        private fun shownAddress(ip: String, port: Int): String =
            if (port == DEFAULT_SERVER_PORT) ip else "$ip:$port"

        /**
         * Asks for Wi-Fi Direct's discovery permission the first time the player
         * goes looking for a multiplayer game, and never again in this session -
         * see [nearbyPermissionLauncher]. Silent on hardware that cannot do
         * Wi-Fi Direct at all, and on a device where it has already been granted.
         */
        override fun onEnterMultiplayer() {
            if (askedForNearbyPermission) return
            askedForNearbyPermission = true
            if (!WifiDirectTransport.isSupported(applicationContext)) return
            if (WifiDirectTransport.hasPermissions(applicationContext)) return
            nearbyPermissionLauncher.launch(WifiDirectTransport.requiredPermissions())
        }

        /**
         * Stops telling other devices about a game that has ended, and drops any
         * Wi-Fi Direct group this device is in.
         *
         * Both halves matter for different reasons. The NSD registration going
         * stale is a nuisance - peers keep seeing a game whose socket
         * stopGame() has already closed - but a Wi-Fi Direct group left up is a
         * real cost: it holds the radio in a group and can keep the device off
         * its normal Wi-Fi, long after the game it existed for is over.
         */
        override fun stopNetworkAdvertising() {
            LanDiscovery.stopRegistration()
            LanDiscovery.stopDiscovery()
            stopWifiDirect()
            // The sockets themselves belong to NetBridge and go with stopGame();
            // this is the scan, which holds the radio and would otherwise keep
            // running after the dialog that started it went away.
            BluetoothTransport.stopDiscovery(applicationContext)
            hostOverBluetooth = false
            hostForOthers = false
            bluetoothVisibilityAsked = false
        }

        private fun stopWifiDirect() {
            if (!WifiDirectTransport.isSupported(applicationContext)) return
            WifiDirectTransport.stopDiscovery()
            WifiDirectTransport.stopAdvertising(applicationContext)
            WifiDirectTransport.disconnect(applicationContext)
        }

        // Blocks (suspends) until the user picks a discovered game or types a
        // host:port manually, or cancels (null). A thin wrapper around
        // showFindGames's dialog plus a manual-entry option, since a PC host or
        // an NSD-blocked network has nothing to discover.
        //
        // Answers with the whole FoundGame rather than a host/port pair: a Wi-Fi
        // Direct result has no address yet, and turning it into one is a
        // seconds-long negotiation the caller has to be able to narrate and
        // cancel - see startJoinFlow.
        @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
        private suspend fun pickJoinTarget(overBluetooth: Boolean = false): LanDiscovery.FoundGame? =
            kotlinx.coroutines.suspendCancellableCoroutine { cont ->
                showFindGames(
                    overBluetooth = overBluetooth,
                    onSelected = { game -> if (cont.isActive) cont.resume(game) {} },
                    onCancelled = { if (cont.isActive) cont.resume(null) {} },
                )
            }

        // M6: the battlefield surface is now camera control, not fire input -
        // one-finger drag orbits (yaw/pitch), pinch zooms (see
        // renderer_jni.cpp's file-level comment for why: firing is reliably
        // handled by the angle/elevation/power sliders + Fire button in
        // GameHud.kt, which don't depend on screen-to-world mapping, whereas a
        // screen tap/drag no longer has an unambiguous landscape meaning under
        // a real perspective camera without ray-casting against the terrain
        // mesh - not done in this slice). The old M2-era tap-to-fire
        // (NativeBridge.handleTap) and drag-slingshot fire are retired from
        // this surface as a result. That path has since been *replaced* rather
        // than merely retired - tap-to-aim now goes through the terrain
        // ray-cast (nativePickTerrain + aimAtPoint), so the old normalised-space
        // handleTap has been deleted rather than left lying around unused.
        //
        // ScaleGestureDetector owns pinch-zoom; a plain last-position diff
        // drives orbit drag, suppressed while a scale gesture is in progress
        // (or just ended) so a two-finger pinch doesn't also register as a
        // one-finger drag on whichever pointer stayed down.
        @SuppressLint("ClickableViewAccessibility")
        private fun setUpCameraControls(surface: GLSurfaceView) {
            // Anything under this much movement is a tap, not a drag. Taken from
            // the platform's own scaled touch slop so it matches every other
            // Android app on this screen density rather than a guessed pixel
            // count.
            val tapSlopPx = android.view.ViewConfiguration.get(this@MainActivity).scaledTouchSlop.toFloat()

            var lastX = 0f
            var lastY = 0f
            var dragging = false
            // Centroid of all pointers, for the two-finger pan. Tracked here
            // rather than taken from ScaleGestureDetector.focusX/focusY because
            // those only update while the *span* is changing - two fingers
            // sliding together at a fixed distance is exactly a pan with no
            // pinch, and the detector reports nothing for it.
            var lastFocusX = 0f
            var lastFocusY = 0f
            var panning = false
            // Where and when the gesture started, so a release can be told
            // apart from the end of a drag. A tap aims (upstream's AUTO_AIM);
            // a drag orbits. Without the movement test every orbit would also
            // fling the turret somewhere on release.
            var downX = 0f
            var downY = 0f
            // How far the finger has actually travelled, added up over the whole
            // gesture rather than measured from where it started. A drag that
            // curves away and comes back finishes near its own start, so net
            // displacement cannot tell it from a tap; the distance walked can.
            var pathLength = 0f
            var multiTouched = false

            fun focusOf(event: MotionEvent): Pair<Float, Float> {
                var sumX = 0f
                var sumY = 0f
                for (i in 0 until event.pointerCount) {
                    sumX += event.getX(i)
                    sumY += event.getY(i)
                }
                return Pair(sumX / event.pointerCount, sumY / event.pointerCount)
            }

            val scaleDetector = android.view.ScaleGestureDetector(
                this@MainActivity,
                object : android.view.ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    override fun onScale(detector: android.view.ScaleGestureDetector): Boolean {
                        GameRenderer.nativeCameraZoom(detector.scaleFactor)
                        return true
                    }
                },
            )

            // A long press over a tank's plate opens that tank's card. Only over
            // a plate: everywhere else a press is a press however long it is
            // held, which is the rule the battlefield tap has kept since
            // 02f5f9d - and over a plate a *tap* still aims at the tank, so the
            // card costs the hold rather than the shot.
            var plateHeld = false
            val plateLongPress = Runnable {
                plateHeld = true
                val tankId = GameRenderer.nativePickTankPlate(downX, downY)
                if (tankId != 0) showTankInfo(tankId)
            }

            surface.setOnTouchListener { _, event ->
                scaleDetector.onTouchEvent(event)

                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        lastX = event.x
                        lastY = event.y
                        dragging = true
                        downX = event.x
                        downY = event.y
                        pathLength = 0f
                        multiTouched = false
                        plateHeld = false
                        // Armed only where there is a plate to open, so a press
                        // anywhere else carries no hidden timer at all.
                        if (settings.showTankInfo &&
                            GameRenderer.nativePickTankPlate(event.x, event.y) != 0
                        ) {
                            surface.postDelayed(
                                plateLongPress,
                                android.view.ViewConfiguration.getLongPressTimeout().toLong(),
                            )
                        }
                    }
                    MotionEvent.ACTION_POINTER_DOWN -> {
                        // A second finger rules the gesture out as a tap.
                        multiTouched = true
                        surface.removeCallbacks(plateLongPress)
                        // A second finger just went down - this is a pinch/pan,
                        // not a one-finger orbit; stop treating pointer 0's
                        // movement as one and start tracking the centroid.
                        dragging = false
                        val (fx, fy) = focusOf(event)
                        lastFocusX = fx
                        lastFocusY = fy
                        panning = true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        if (dragging && event.pointerCount == 1 && !scaleDetector.isInProgress) {
                            val dx = event.x - lastX
                            // M11: upstream's InvertMouse, for the one axis where
                            // people genuinely disagree - dragging down to look up
                            // is the flight-sim convention and feels wrong to
                            // everyone else, and vice versa.
                            val dy = (event.y - lastY) * (if (settings.invertDrag) -1f else 1f)
                            GameRenderer.nativeCameraDrag(dx, dy)
                        }
                        if (panning && event.pointerCount >= 2) {
                            // Pan and pinch run together rather than one winning:
                            // they read different things from the same two
                            // fingers (centroid movement vs. span change), so
                            // moving and zooming at once behaves the way it does
                            // in any map app.
                            val (fx, fy) = focusOf(event)
                            GameRenderer.nativeCameraPan(fx - lastFocusX, fy - lastFocusY)
                            lastFocusX = fx
                            lastFocusY = fy
                        }
                        pathLength += kotlin.math.hypot(event.x - lastX, event.y - lastY)
                        lastX = event.x
                        lastY = event.y
                        // A finger that has travelled is orbiting the camera, not
                        // holding a plate.
                        if (pathLength > tapSlopPx) surface.removeCallbacks(plateLongPress)
                    }
                    MotionEvent.ACTION_POINTER_UP -> {
                        // One finger lifted out of a multi-touch gesture - resume
                        // dragging from whichever pointer remains, next MOVE.
                        dragging = false
                        // Below two fingers there is no pan; re-seed the centroid
                        // on the next POINTER_DOWN rather than letting it jump
                        // from a two-finger centroid to a one-finger position.
                        if (event.pointerCount - 1 < 2) panning = false
                    }
                    MotionEvent.ACTION_UP -> {
                        surface.removeCallbacks(plateLongPress)
                        // A press that never went anywhere is a tap, however long
                        // it was held. There used to be a 250ms ceiling on it as
                        // well, and it was quietly throwing away deliberate taps:
                        // anyone lining a shot up rather than stabbing at the
                        // screen holds the screen for longer than that, and the
                        // tap simply did nothing. Nothing needed the limit -
                        // there is no long press on the battlefield for it to
                        // protect, and the distance walked already separates a
                        // tap from an orbit.
                        // plateHeld: the card is already open, and the press
                        // that opened it is not also a shot.
                        if (!multiTouched && !plateHeld && pathLength <= tapSlopPx) {
                            handleBattlefieldTap(event.x, event.y)
                        }
                        dragging = false
                        panning = false
                    }
                    MotionEvent.ACTION_CANCEL -> {
                        surface.removeCallbacks(plateLongPress)
                        dragging = false
                        panning = false
                    }
                }
                true
            }
        }

        // M5: shows "Hosting on <ip>:<port>" once startLocalGame() has bound a
        // real listening socket (see NativeBridge.isHostingOnNetwork/
        // engine_jni.cpp's startLocalGame), so another device has an address to
        // connect to. Falls back to explaining why not, rather than silently
        // doing nothing, if the port didn't bind.
        override fun updateHostingLabel() {
            // A solo game announces nothing and says nothing about announcing.
            //
            // This is the first thing checked because everything below it either
            // publishes the game or reports on having published it: the NSD
            // registration, the Wi-Fi Direct group, the address on the HUD, and
            // the two toasts saying how those went. None of it has a reader in a
            // single-player game, and one of them proved it - "No Wi-Fi Direct:
            // the nearby devices permission was refused" greeted the player over
            // the tutorial's first card, which is a fine thing to say to someone
            // hosting and nonsense to say to someone learning to aim.
            //
            // Leaving the label empty rather than writing something solo-shaped
            // into it: HudText is skipped when it is empty, so the status column
            // loses the line instead of spending one on the player's own IP.
            if (!hostForOthers) {
                hudState.hostingLabel = ""
                return
            }
            scope.launch {
                val hosting = withContext(Dispatchers.Default) { NativeBridge.isHostingOnNetwork() }
                val port = withContext(Dispatchers.Default) { NativeBridge.getServerPort() }

                // A Bluetooth game has no address and no port to publish, and
                // nothing to advertise over the network either - the other
                // device finds this one by its Bluetooth name.
                if (hostOverBluetooth) {
                    // Asked of the adapter rather than assumed from the prompt
                    // having been answered: being connectable and being
                    // *findable* are different states, and only the second one
                    // gets an unpaired player in. Claiming the second while in
                    // the first is how a host can look fine to its own player
                    // and be invisible to everyone else.
                    val name = BluetoothTransport.localName(applicationContext)
                    val visible = BluetoothTransport.isDiscoverable(applicationContext)
                    hudState.hostingLabel = when {
                        !hosting -> "Bluetooth hosting failed"
                        visible -> "Bluetooth: $name"
                        else -> "Bluetooth: $name (not visible)"
                    }
                    // The detail that used to ride along on that line. It is
                    // worth saying once and not worth a permanent line of HUD:
                    // the five minutes is Android's own cap and is already
                    // running, and a host that is not findable at all is
                    // something the player has to be told rather than left to
                    // infer from nobody arriving.
                    if (hosting) {
                        notifyPlayer(
                            if (visible) {
                                "New devices can find this game for 5 minutes"
                            } else {
                                "Not visible, only paired devices can join"
                            }
                        )
                    }
                    return@launch
                }

                if (!hosting) {
                    hudState.hostingLabel = "Solo only (port $port busy)"
                    return@launch
                }

                // No address at all means no network is up, which "Hosting on
                // unknown IP" managed to say without saying what to do about it.
                // "Host" and the address, and nothing else. The status column
                // shares its row with four icon buttons, so it has about
                // twenty-five characters: "Hosting on 192.168.232.2:27270" is
                // thirty and came out as "Hosting on 192.168.23...", which cut
                // the one thing on the line worth reading out to someone.
                //
                // If a long address still clips, what goes is the port - and the
                // port is now the part a joiner can leave out, since typing a
                // bare address uses 27270.
                val ip = getLocalIpAddress()
                hudState.hostingLabel = if (ip != null) "Host ${shownAddress(ip, port)}" else "No network"
                if (ip == null) {
                    notifyPlayer("No network, turn on Wi-Fi or your hotspot so others can join")
                }
                LanDiscovery.registerService(applicationContext, port)

                // Wi-Fi Direct is advertised alongside, not instead: the two
                // reach different people. NSD finds anyone already on this
                // network (including a PC); Wi-Fi Direct reaches someone sitting
                // next to you with no network at all. Only claimed in the label
                // once the group has actually formed - announcing a way to be
                // reached that isn't up is worse than not offering it.
                WifiDirectTransport.advertise(applicationContext, port) { advertising, why ->
                    // One line, and the reason goes to a toast instead.
                    //
                    // A host that believes it is reachable over Wi-Fi Direct when
                    // no group formed waits for peers that can never arrive, so
                    // the reason still has to be said - the first two-device test
                    // of this could not tell the two apart from the screen. But it
                    // was five wrapped lines of HUD sitting over the battlefield
                    // for the whole game, which is too high a price for something
                    // a player reads once.
                    hudState.hostingLabel = when {
                        ip != null -> "Host ${shownAddress(ip, port)}"
                        advertising -> "Host over Wi-Fi Direct"
                        else -> hudState.hostingLabel
                    }
                    // Whether Wi-Fi Direct came up is said once rather than
                    // carried on the line for the whole game - the line has
                    // room for the address and that is what it is for.
                    notifyPlayer(
                        if (advertising) {
                            "Wi-Fi Direct is on, nearby phones can find this game"
                        } else {
                            "No Wi-Fi Direct: ${why ?: "it did not start"}"
                        }
                    )
                }
            }
        }

        // M5 Phase 2: scans the LAN for other advertised ScorchDroid games (see
        // LanDiscovery.kt) and lists what it finds. Tapping a result now calls
        // [onSelected] with its host/port; there's also a manual "Enter
        // address..." row for a PC host, or a network that drops NSD's
        // multicast traffic. [onCancelled] fires if the user backs out without
        // picking anything. Defaults let the main-screen "Find Games" button
        // (used mid-game, purely to browse - see the button's own click
        // handler) keep working as a no-op-on-selection browse dialog without
        // having to pass callbacks it doesn't care about.
        private fun showFindGames(
            overBluetooth: Boolean = false,
            onSelected: (LanDiscovery.FoundGame) -> Unit = {},
            onCancelled: () -> Unit = {},
        ) {
            val found = mutableListOf<LanDiscovery.FoundGame>()
            var resolved = false
            // Both scans run at once and land in the same list. Each reports
            // finishing separately, so the closing title waits for both rather
            // than for whichever happened to be quicker.
            // Not while this device is the one hosting a group: the HUD's "Find
            // Games" button is reachable mid-game, and putting the radio into a
            // peer scan there would disturb the very group the host is running
            // the game on. A host has no reason to be looking anyway.
            //
            // Whichever reason keeps Wi-Fi Direct out of a search, the player is
            // told it. Silently searching one radio while appearing to search
            // both is how two devices sitting next to each other can each report
            // finding nothing with nothing wrong with either of them.
            //
            // Bluetooth is a search of its own rather than a third row of this
            // one. It cannot be narrowed to devices running the game - asking
            // each one costs an SDP lookup that is slow and often answers
            // nothing - so it lists every speaker, headset and car in range, and
            // burying two phones in that was worse than having one more button.
            val hostingHere = WifiDirectTransport.isAdvertising()
            val wifiDirectProblem = when {
                overBluetooth -> null  // Not part of this search at all.
                hostingHere -> "this device is hosting"
                else -> WifiDirectTransport.unavailableReason(applicationContext)
            }
            val wifiDirect = !overBluetooth && wifiDirectProblem == null
            val bluetoothProblem = when {
                !overBluetooth -> null
                hostOverBluetooth -> "this device is hosting"
                else -> BluetoothTransport.unavailableReason(applicationContext)
            }
            val bluetooth = overBluetooth && bluetoothProblem == null
            val network = !overBluetooth
            var scansRunning =
                (if (network) 1 else 0) + (if (wifiDirect) 1 else 0) + (if (bluetooth) 1 else 0)
            // A search with nothing to search still has to report itself
            // finished - see the scanFinished() call at the end - rather than
            // sitting on "Searching..." for as long as the player will stare
            // at it.
            if (scansRunning == 0) scansRunning = 1
            // Devices the radio can see that answered no service query - see
            // WifiDirectTransport.startDiscovery. Listed after the real results,
            // and joinable anyway: service discovery over Wi-Fi Direct is the
            // flakiest part of this path, and a player who can see the other
            // phone's name should be able to try it.
            val peersWithoutGames = mutableListOf<LanDiscovery.FoundGame>()

            // Nothing to type in a Bluetooth search: an address there is a MAC
            // nobody knows by heart, and there is no port at all.
            fun manualEntryLabel() = if (overBluetooth) null else "Enter address manually..."
            fun helpLabel() = "How do I connect?"

            fun stopScans() {
                if (network) LanDiscovery.stopDiscovery()
                if (wifiDirect) WifiDirectTransport.stopDiscovery()
                if (bluetooth) BluetoothTransport.stopDiscovery(applicationContext)
            }

            val listDialog = HudDialog.ListChoice(
                title = when {
                    overBluetooth -> "Searching for Bluetooth devices..."
                    wifiDirect -> "Searching for games..."
                    else -> "Searching for LAN games..."
                },
                items = listOf(manualEntryLabel(), helpLabel()).filterNotNull(),
                cancelLabel = "Cancel",
                onSelect = { index ->
                    resolved = true
                    stopScans()
                    hudState.dialog = HudDialog.None
                    val rows = found.size + peersWithoutGames.size
                    val manualRow = if (overBluetooth) -1 else rows
                    when (index) {
                        in found.indices -> onSelected(found[index])
                        in found.size until rows -> onSelected(peersWithoutGames[index - found.size])
                        manualRow -> promptManualAddress(onSelected, onCancelled)
                        else -> showConnectionHelp { showFindGames(overBluetooth, onSelected, onCancelled) }
                    }
                },
                onCancel = {
                    stopScans()
                    hudState.dialog = HudDialog.None
                    if (!resolved) onCancelled()
                },
            )
            hudState.dialog = listDialog

            fun refresh() {
                // A Wi-Fi Direct peer has no address to show yet - there is no IP
                // until a group forms - so it is labelled by how it was found
                // instead, which is the part the player cares about anyway.
                listDialog.items = found.map { game ->
                    when {
                        game.p2pDeviceAddress != null -> "${game.name} - Wi-Fi Direct"
                        // Said plainly, because it is what decides whether the
                        // next tap shows a pairing prompt on both devices.
                        game.bluetoothAddress != null && game.bluetoothPaired -> "${game.name} - paired"
                        game.bluetoothAddress != null -> "${game.name} - not paired yet"
                        else -> "${game.name} - ${game.host}:${game.port}"
                    }
                } + peersWithoutGames.map { "${it.name} - nearby, no game seen (try anyway)" } +
                    listOfNotNull(manualEntryLabel(), helpLabel())
            }

            fun add(game: LanDiscovery.FoundGame) {
                val duplicate = found.any {
                    when {
                        game.p2pDeviceAddress != null -> it.p2pDeviceAddress == game.p2pDeviceAddress
                        game.bluetoothAddress != null -> it.bluetoothAddress == game.bluetoothAddress
                        else -> it.host == game.host && it.port == game.port
                    }
                }
                if (duplicate) return
                found.add(game)
                // A device that answers properly should not also be offered as a
                // guess.
                peersWithoutGames.removeAll { it.p2pDeviceAddress == game.p2pDeviceAddress }
                refresh()
            }

            // A device the radio can see that advertised no game. Offered last
            // and labelled as the guess it is: it may be a phone hosting one
            // whose service query went unanswered, or it may be a printer.
            fun addPeer(name: String, deviceAddress: String) {
                if (found.any { it.p2pDeviceAddress == deviceAddress }) return
                if (peersWithoutGames.any { it.p2pDeviceAddress == deviceAddress }) return
                peersWithoutGames.add(
                    LanDiscovery.FoundGame(
                        name = name,
                        host = "",
                        port = DEFAULT_SERVER_PORT,
                        p2pDeviceAddress = deviceAddress,
                    )
                )
                refresh()
            }

            fun scanFinished() {
                if (scansRunning <= 0 || --scansRunning > 0) return
                // Every radio that sat this one out says so, by name and with
                // its reason. This line is the one a player actually reads, and
                // "no games found" with half the search silently skipped is how
                // a working pair of phones comes to look broken - which is
                // exactly how the first Wi-Fi Direct test went.
                val skipped = listOfNotNull(
                    wifiDirectProblem?.let { "Wi-Fi Direct ($it)" },
                    bluetoothProblem?.let { "Bluetooth ($it)" },
                )
                val outcome = when {
                    found.isNotEmpty() && overBluetooth -> "${found.size} device(s) nearby"
                    found.isNotEmpty() -> "Found ${found.size} game(s)"
                    // Naming the thing that does work. A Bluetooth scan finds
                    // only devices that are currently *discoverable*, which is a
                    // five-minute state the host has to have granted; pairing
                    // the two phones once, in Android's own settings, sidesteps
                    // that permanently - a paired device is listed here from the
                    // bonded list with no scan at all.
                    overBluetooth ->
                        "No Bluetooth devices found. Pair the two phones in Android " +
                            "Settings and they'll show up here without searching"
                    peersWithoutGames.isNotEmpty() ->
                        "No games found (${peersWithoutGames.size} nearby device(s) aren't hosting)"
                    else -> "No games found"
                }
                listDialog.title = if (skipped.isEmpty()) {
                    outcome
                } else {
                    "$outcome\nNot searched: ${skipped.joinToString(", ")}"
                }
            }

            if (network) {
                LanDiscovery.startDiscovery(
                    applicationContext,
                    durationMs = 4000,
                    onFound = { add(it) },
                    onFinished = { scanFinished() },
                )
            }

            if (bluetooth) {
                // Paired devices come back immediately; the scan itself is the
                // slow part and runs for as long as the Wi-Fi Direct one.
                BluetoothTransport.startDiscovery(
                    applicationContext,
                    durationMs = 20000,
                    onFound = { add(it) },
                    onFinished = { scanFinished() },
                )
            }

            if (wifiDirect) {
                // Far longer than the NSD scan, and re-issued throughout (see
                // WifiDirectTransport.startDiscovery): a Wi-Fi Direct service
                // query has to get the radio scanning for peers before any of
                // them can answer, and both devices have to be listening in the
                // same round for an answer to arrive at all - where mDNS is one
                // multicast onto a network that already exists. Twenty seconds
                // is a long time to stare at a dialog, but results land in the
                // list as they arrive rather than at the end.
                WifiDirectTransport.startDiscovery(
                    applicationContext,
                    durationMs = 20000,
                    onFound = { add(it) },
                    onPeerSeen = { name, address -> addPeer(name, address) },
                    onFinished = { scanFinished() },
                )
            }

            // Every radio this search wanted is unavailable, so no scan will ever
            // report in. Saying why is the whole of what is left to do.
            if (!network && !wifiDirect && !bluetooth) scanFinished()
        }

        /**
         * What to do when "Find Games" turns up nothing, which is the moment a
         * player is most likely to conclude multiplayer is broken. All four
         * routes work today and none of them is obvious from the outside -
         * particularly the hotspot one, which needs no code at all and is the
         * answer whenever there is no router in reach.
         *
         * [onDismiss] goes back to the search rather than out to the menu: a
         * player who came here to find out how to connect still wants to.
         */
        private fun showConnectionHelp(onDismiss: () -> Unit) {
            hudState.dialog = HudDialog.Message(
                "Ways to play together:\n\n" +
                    "Same Wi-Fi: put both devices on the same network. One taps " +
                    "Host Game, the other taps Join Game.\n\n" +
                    "Wi-Fi Direct: no router needed, both devices just need Wi-Fi " +
                    "turned on. The host's game shows up in this list marked " +
                    "\"Wi-Fi Direct\". It needs the nearby devices permission, which " +
                    "is only asked for once. If you said no, you can turn it on in " +
                    "Android Settings under Apps > ScorchDroid > Permissions.\n\n" +
                    "Bluetooth: no Wi-Fi needed. The host picks \"Host over " +
                    "Bluetooth\" and allows the visibility prompt, the other picks " +
                    "\"Join over Bluetooth\". Pairing the two phones first makes it " +
                    "quicker and more reliable.\n\n" +
                    "Hotspot: turn on a hotspot on the host and connect the other " +
                    "device to it, then Host and Join as usual.\n\n" +
                    "By address: some guest and work networks block games from " +
                    "finding each other. The host's screen shows its address, type " +
                    "it in with \"Enter address manually\"."
            ) {
                hudState.dialog = HudDialog.None
                onDismiss()
            }
        }

        // M5 Phase 2: falls back to a typed "host:port" when nothing useful
        // showed up via NSD - the only way to reach a PC host today, since
        // desktop Scorched3D doesn't advertise itself via Android's NSD/mDNS.
        private fun promptManualAddress(
            onSelected: (LanDiscovery.FoundGame) -> Unit,
            onCancelled: () -> Unit,
        ) {
            hudState.dialog = HudDialog.ManualAddress(
                onConnect = { text ->
                    val typed = text.trim()
                    val host = typed.substringBefore(':').trim().takeIf { it.isNotEmpty() }
                    // The port is optional, and anything unusable in its place is
                    // treated as absent rather than as a reason to give up: an
                    // address with no port used to close the dialog and return to
                    // the menu having said nothing, which reads as the Connect
                    // button being broken. Every Scorched3D host listens on
                    // 27270 unless its config says otherwise, so that is the
                    // right guess - and the status line names the port it dialled,
                    // so a wrong guess explains itself.
                    val port = typed.substringAfter(':', "").trim().toIntOrNull()
                        ?.takeIf { it in 1..65535 }
                        ?: DEFAULT_SERVER_PORT
                    hudState.dialog = HudDialog.None
                    if (host != null) {
                        onSelected(LanDiscovery.FoundGame(name = host, host = host, port = port))
                    } else {
                        onCancelled()
                    }
                },
                onCancel = {
                    hudState.dialog = HudDialog.None
                    onCancelled()
                },
            )
        }

        // Plain Android API - the actual LAN IP a peer would dial in to, not
        // something the native engine (which just binds INADDR_ANY) knows.
        //
        // Ranked rather than "the first one found", which was arbitrary as soon
        // as a device had more than one address up, and a device hosting a game
        // usually does. Normal Wi-Fi first, since that is the address someone on
        // the same network types in. Then the hotspot interface, so a host who
        // turned their hotspot on to play (see the connection help) still shows
        // an address that works - their normal Wi-Fi is often down at that
        // point. Wi-Fi Direct's own interface last: peers reach a group owner
        // through the group, never by typing 192.168.49.1 at it.
        private fun getLocalIpAddress(): String? {
            fun rank(interfaceName: String): Int = when {
                interfaceName.startsWith("wlan") -> 0
                interfaceName.startsWith("ap") ||
                    interfaceName.startsWith("swlan") ||
                    interfaceName.startsWith("rndis") -> 1
                interfaceName.startsWith("p2p") -> 3
                else -> 2
            }

            return try {
                Collections.list(NetworkInterface.getNetworkInterfaces())
                    .flatMap { nic -> Collections.list(nic.inetAddresses).map { nic.name to it } }
                    .filter { (_, address) -> !address.isLoopbackAddress && address is Inet4Address }
                    .minByOrNull { (name, _) -> rank(name) }
                    ?.second?.hostAddress
            } catch (e: Exception) {
                null
            }
        }
    }

    private companion object {
        // Upstream's PortNo default, which every ScorchDroid host uses since
        // nothing in the port lets a player change it. Only ever a guess for
        // a Wi-Fi Direct peer that advertised no service record and so never
        // told us its port - a real result carries its own.
        const val DEFAULT_SERVER_PORT = 27270

        // Bluetooth's "let other devices see this one" dialog, and the
        // system's own "turn Bluetooth on?" prompt.
        const val REQUEST_DISCOVERABLE = 4001
        const val REQUEST_ENABLE_BLUETOOTH = 4002
    }
}
