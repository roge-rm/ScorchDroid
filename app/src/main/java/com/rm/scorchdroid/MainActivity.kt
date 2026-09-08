package com.rm.scorchdroid

import android.annotation.SuppressLint
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.MotionEvent
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.ComposeView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.Collections
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {
    private lateinit var gameSurface: GLSurfaceView
    private lateinit var gameRenderer: GameRenderer

    // M9: the container the GL surface is added to when a game starts and
    // removed from on quit - see startGame/quitToMenu.
    private lateinit var surfaceHost: android.widget.FrameLayout
    // Which top-level screen is showing. The game is one of these now.
    private var appScreen by mutableStateOf(AppScreen.SPLASH)
    private var splashStatus by mutableStateOf("Starting...")
    private var licenseText by mutableStateOf("")

    // M11: the player's own preferences, as opposed to a game's rules - see
    // GameSettings. Created in onCreate, before anything reads a setting.
    private lateinit var settings: GameSettings
    // M15: created once the data root exists; state-driven from the tick.
    private var music: MusicPlayer? = null
    // The running game's tick loop, so quit-to-menu can stop it. Non-null
    // exactly while a game is running.
    private var gameJob: Job? = null

    // M10: the game-setup screen's state. The options come from the engine
    // (upstream's own entries, ranges and descriptions) rather than being
    // declared here - see GameSetup.h.
    private var setupOptions by mutableStateOf<List<SetupOption>>(emptyList())
    private var setupTitle by mutableStateOf("New Game")
    private var availableMods by mutableStateOf<List<String>>(emptyList())
    // M12: non-null exactly while a tutorial game is running.
    private var tutorial by mutableStateOf<TutorialState?>(null)
    private var selectedMod by mutableStateOf("none")
    // M14: the ready-made games the installed mods describe in their own
    // modinfo.xml. Read once, after the engine has a data root - the list
    // cannot change while the app is running.
    private var presets by mutableStateOf<List<GamePreset>>(emptyList())

    // M4: the real Compose HUD's mutable state (see GameHud.kt) - written
    // to directly from the tick loop, touch handlers, and dialogs below,
    // all plain (non-Composable) Kotlin code, so a plain mutable holder is
    // simpler here than threading Compose State through every function
    // that used to take a `statusText: TextView` parameter.
    private val hudState = GameHudState()

    // M4: touch-controllable elevation, in degrees, shared by both fire
    // gestures below - mirrors hudState.elevationDegrees (the Slider's
    // displayed value) but kept as a separate @Volatile field since it's
    // read from a background coroutine dispatcher when firing, and Compose
    // State reads/writes are only safe on the main thread.
    @Volatile
    private var currentElevationDegrees = 45f

    // Slider-based aiming (see the porting plan's "aiming controls
    // direction" note) - angle/power set via the sliders in GameHud.kt,
    // fired explicitly via the Fire button (fireFromSliders()) rather than
    // on gesture release like the battlefield tap/drag. Same
    // mirrors-Compose-state-into-a-@Volatile-field pattern as
    // currentElevationDegrees above, for the same reason.
    @Volatile
    private var currentAngleDegrees = 0f

    @Volatile
    private var currentPowerFraction = DEFAULT_POWER_FRACTION

    // M6 parity: upstream's UNDO_MOVE ("Revert to last angles") - the
    // angle/elevation/power of the last shot actually fired, so a player
    // can get back to it after nudging the sliders around. Null until
    // something has been fired this session.
    private var lastFiredAim: Triple<Float, Float, Float>? = null

    // M6: whether the aiming sliders have been seeded from the tank's real
    // starting turret rotation yet (see the tick loop). One-shot, so it
    // never fights the player's own adjustments afterwards.
    private var aimSeeded = false
    // M6 parity: chat polling state. The version is the cheap "did anything
    // arrive" check; the line id is how far the HUD has already been told
    // about, so a message it is already timing is never restarted.
    private var lastChatVersion = 0
    private var lastChatLineId = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hideSystemBars()
        // A round can sit idle for a while waiting on other players/bots,
        // with no touch input in between - without this the screen times
        // out mid-game exactly like any other idle app.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)
        hudState.statusText = NativeBridge.helloFromNative()

        surfaceHost = findViewById(R.id.game_surface_host)
        settings = GameSettings(this)

        findViewById<ComposeView>(R.id.hud_compose_view).setContent {
            // M9: the system back gesture walks the menu back up a level.
            // Enabled only off the main menu, so back there still leaves the
            // app as Android expects. In a game it does nothing: quitting is
            // a confirmed action behind the overflow menu, and a stray back
            // swipe mid-round should never throw the game away.
            BackHandler(enabled = appScreen != AppScreen.MENU) {
                when (appScreen) {
                    AppScreen.GAME -> Unit
                    // The only screen two levels down; back should undo one
                    // step, not both.
                    AppScreen.QUICK_GAME -> appScreen = AppScreen.SINGLE_PLAYER
                    else -> appScreen = AppScreen.MENU
                }
            }
            when (appScreen) {
                AppScreen.SPLASH -> SplashScreen(splashStatus, null)
                AppScreen.MENU -> MainMenuScreen(
                    onSinglePlayer = { appScreen = AppScreen.SINGLE_PLAYER },
                    onMultiplayer = { appScreen = AppScreen.MULTIPLAYER },
                    onSettings = { appScreen = AppScreen.SETTINGS },
                    onAbout = { appScreen = AppScreen.ABOUT },
                )
                AppScreen.SINGLE_PLAYER -> SinglePlayerScreen(
                    onQuickGame = { openQuickGame() },
                    quickGameEnabled = presets.isNotEmpty(),
                    onNewGame = { openSetup("New Game") },
                    onTutorial = { startTutorial() },
                    tutorialEnabled = true,
                    onBack = { appScreen = AppScreen.MENU },
                )
                AppScreen.QUICK_GAME -> QuickGameScreen(
                    presets = presets,
                    onPick = { startPreset(it) },
                    onBack = { appScreen = AppScreen.SINGLE_PLAYER },
                )
                AppScreen.MULTIPLAYER -> MultiplayerScreen(
                    onHost = { openSetup("Host Game") },
                    onJoin = { startJoinFlow() },
                    onBack = { appScreen = AppScreen.MENU },
                )
                // M11 builds this screen; until then the button is honest
                // about it rather than doing nothing when tapped.
                AppScreen.SETUP -> GameSetupScreen(
                    title = setupTitle,
                    options = setupOptions,
                    mods = availableMods,
                    selectedMod = selectedMod,
                    onModChange = {
                        NativeBridge.setSelectedMod(it)
                        selectedMod = NativeBridge.getSelectedMod()
                    },
                    onChange = { option, value -> changeSetupOption(option, value) },
                    onReset = {
                        NativeBridge.resetSetupOptions()
                        setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
                        selectedMod = NativeBridge.getSelectedMod()
                    },
                    onStart = { startGame() },
                    onBack = { appScreen = AppScreen.MENU },
                )
                AppScreen.JOINING -> JoiningScreen(
                    status = hudState.statusText,
                    dialog = hudState.dialog,
                    onBack = { cancelJoinFlow() },
                )
                AppScreen.SETTINGS -> SettingsScreen(
                    settings = settings,
                    onBack = { appScreen = AppScreen.MENU },
                )
                AppScreen.ABOUT -> AboutScreen(
                    versionName = BuildConfig.VERSION_NAME,
                    upstreamCommit = BuildConfig.UPSTREAM_COMMIT,
                    licenseText = licenseText,
                    onBack = { appScreen = AppScreen.MENU },
                )
                AppScreen.GAME -> GameHud(
                state = hudState,
                onFindGames = { showFindGames() },
                onShop = { showWeaponShop() },
                onWeapon = { showWeaponQuickSelect() },
                onElevationChange = { degrees ->
                    currentElevationDegrees = degrees
                    hudState.elevationDegrees = degrees
                    pushAimToEngine()
                },
                onAngleChange = { degrees ->
                    currentAngleDegrees = degrees
                    hudState.angleDegrees = degrees
                    pushAimToEngine()
                },
                onPowerChange = { power ->
                    currentPowerFraction = power
                    hudState.powerFraction = power
                    pushAimToEngine()
                },
                onFire = { fireFromSliders() },
                onToggleCamera = { hudState.cameraFollow = gameRenderer.nativeToggleCameraMode() },
                onDefenses = { showDefenses() },
                onActions = { showActionsMenu() },
                onUndo = { revertToLastAim() },
                onQuitToMenu = { confirmQuitToMenu() },
                onSkip = { submitMoveAsync(MoveType.SKIP) },
                onDoneBuying = { submitMoveAsync(MoveType.FINISHED_BUY) },
                onScores = { showScores() },
                onCameraPresets = { showCameraPresets() },
                onSimulationSpeed = { showSimulationSpeed() },
                onSendChat = { text -> sendChatAsync(hudState.chatChannel, text) },
                )
            }
            // A game that fails to load has to be able to say so from the
            // screen it was picked on. HudDialogHost is drawn by the game and
            // joining screens; these two raise dialogs without being either.
            if (appScreen == AppScreen.SINGLE_PLAYER || appScreen == AppScreen.QUICK_GAME) {
                HudDialogHost(hudState.dialog)
            }
            // M12: over the HUD, and only during a tutorial game.
            tutorial?.let { active ->
                if (appScreen == AppScreen.GAME) {
                    TutorialOverlay(active) { active.skip() }
                }
            }
        }

        // First run has real work to do - extracting upstream's ~90MB data/
        // tree - so the splash stays up until the engine has a data root.
        CoroutineScope(Dispatchers.Main).launch {
            splashStatus = "Extracting game data..."
            val dataRoot = withContext(Dispatchers.IO) {
                AssetDataExtractor.ensureExtracted(applicationContext)
            }
            splashStatus = "Starting engine..."
            val initOk = withContext(Dispatchers.Default) {
                NativeBridge.initEngine(dataRoot.absolutePath)
            }
            if (!initOk) {
                splashStatus = "Failed to initialize engine data root"
                return@launch
            }
            // Only now: applyAll() crosses into the engine, which has just
            // been given its data root.
            music = MusicPlayer(dataRoot).also {
                it.load(NativeBridge.getSelectedMod())
                settings.music = it
            }
            settings.applyAll()
            presets = parsePresets(NativeBridge.getPresets())
            // Upstream's menu music is its "wait" loop.
            music?.setState(MusicPlayer.State.WAIT)
            licenseText = withContext(Dispatchers.IO) { readLicenseText() }
            appScreen = AppScreen.MENU
        }
    }

    /**
     * M12: starts the tutorial - upstream's own easy-game configuration with
     * this port's own coach marks over it.
     *
     * No setup screen: the whole point is a game that needs no decisions
     * first. The preset is loaded rather than merged, so a player who has been
     * fiddling with rounds and wall types still gets the gentle version.
     */
    private fun startTutorial() {
        val loaded = NativeBridge.loadSetupPreset("data/singletutorial.xml")
        if (!loaded) {
            // Nothing was changed, so a normal game would start instead - with
            // tutorial text over it, which would be worse than saying so.
            hudState.dialog = HudDialog.Message("The tutorial's settings could not be loaded.") {
                hudState.dialog = HudDialog.None
            }
            return
        }
        tutorial = TutorialState()
        startGame()
    }

    private fun openQuickGame() {
        appScreen = AppScreen.QUICK_GAME
    }

    /**
     * M14: starts one of the mods' own ready-made games.
     *
     * Like the tutorial, and for the same reason: the preset *replaces* the
     * setup rather than merging into it, so "Easy Game" is upstream's easy
     * game and not upstream's easy game plus whatever was last fiddled with in
     * New Game. The mod comes with it - a mod's preset file names the mod
     * itself, which is why picking an Apocalypse game needs no separate mod
     * choice - and startGame() writes the session config from that, so the
     * server loads the right mod before it reads anything else.
     */
    private fun startPreset(preset: GamePreset) {
        if (!NativeBridge.loadSetupPreset(preset.gameFile)) {
            hudState.dialog = HudDialog.Message("\"${preset.name}\" could not be loaded.") {
                hudState.dialog = HudDialog.None
            }
            return
        }
        startGame()
    }

    /**
     * M10: opens the pre-game setup screen. Both New Game and Host Game land
     * here - they differ in wording, not in what they configure, because a
     * single-player game on this port *is* a hosted game that nobody joined.
     */
    private fun openSetup(title: String) {
        // Back to the shipped config: a player who ran the tutorial and then
        // started a real game would otherwise inherit its seven inert targets
        // and its missing shot clock, with the setup screen showing them as
        // though they had chosen them.
        NativeBridge.resetSetupOptions()
        setupTitle = title
        setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
        // The mod reaches the server through the session config, which is the
        // only route that can work: startServerInternal() loads mod files
        // partway through its own startup, so anything applied after
        // startServer() is far too late for it.
        availableMods = NativeBridge.getAvailableMods().toList()
        selectedMod = NativeBridge.getSelectedMod()
        appScreen = AppScreen.SETUP
    }

    /**
     * Sends one choice to the engine and re-reads the list.
     *
     * Re-read rather than patched locally: the engine is the authority on
     * whether a value was accepted, and on what the option now reads as. A
     * rejected value (out of upstream's range, or not one of an enum's
     * choices) then simply leaves the control where it was, which is the right
     * behaviour and costs no validation logic here.
     */
    private fun changeSetupOption(option: SetupOption, value: String) {
        NativeBridge.setSetupOption(option.name, value)
        setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
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
        "The full GNU General Public License v2 text could not be loaded. " +
            "It is available at https://www.gnu.org/licenses/old-licenses/gpl-2.0.html " +
            "and in the LICENSE file of the source repository."
    }

    /**
     * M9: starts a game and switches to the game screen, creating the GL
     * surface as it goes.
     *
     * A fresh GLSurfaceView per game is deliberate. It gives a fresh EGL
     * context, so nativeOnSurfaceCreated runs and the renderer forgets every
     * build-once cache it holds - terrain, ground texture, models, trees,
     * water, sky. That reset already exists and is already correct, because
     * the minimise/resume bug forced it to be; reusing it is much safer than
     * writing a second "forget everything" path that would need to stay in
     * step with the first.
     */
    private fun startGame() {
        if (gameJob != null) return
        music?.load(NativeBridge.getSelectedMod())
        applySettingsToHud()
        attachGameSurface()
        appScreen = AppScreen.GAME
        gameJob = CoroutineScope(Dispatchers.Main).launch { startAsHost() }
    }

    /**
     * M10: find and connect to a game, staying on a menu screen while it
     * happens.
     *
     * The GL surface is deliberately not built until the connection is up.
     * Building it first - which is what happened when Join went through
     * startGame() - put the discovery dialog on top of the aiming sliders and
     * Fire button of a game that did not exist yet.
     */
    private fun startJoinFlow() {
        if (gameJob != null) return
        appScreen = AppScreen.JOINING
        hudState.statusText = "Looking for a game..."
        gameJob = CoroutineScope(Dispatchers.Main).launch {
            val target = pickJoinTarget()
            if (target == null) {
                gameJob = null
                appScreen = AppScreen.MULTIPLAYER
                return@launch
            }

            hudState.statusText = "Connecting to ${target.first}:${target.second}..."
            val connecting = withContext(Dispatchers.Default) {
                NativeBridge.startJoinGame(target.first, target.second)
            }
            if (!connecting) {
                hudState.statusText =
                    "Couldn't reach ${target.first}:${target.second}. Tap Cancel to go back."
                return@launch
            }

            // Connected, so there is now a game to draw.
            applySettingsToHud()
            attachGameSurface()
            appScreen = AppScreen.GAME
            awaitJoinAndPlay()
        }
    }

    /**
     * M11: copies the display-side settings into the HUD's own state.
     *
     * Copied rather than read through, so a composition never touches
     * preferences: the HUD reads one object, and this is the single place the
     * two are joined. Called when a game starts, which is the only time they
     * can have changed - the settings screen is not reachable mid-game.
     */
    private fun applySettingsToHud() {
        hudState.showNamePlates = settings.showNamePlates
        hudState.showHealthBars = settings.showHealthBars
        hudState.chatToastMillis = settings.chatToastSeconds * 1000L
        hudState.leftHandMode = settings.leftHandMode
        hudState.controlOpacity = settings.controlOpacity
    }

    /** Abandons a join that hasn't connected yet and returns to the menu. */
    private fun cancelJoinFlow() {
        music?.setState(MusicPlayer.State.WAIT)
        gameJob?.cancel()
        gameJob = null
        hudState.dialog = HudDialog.None
        NativeBridge.stopGame()
        hudState.reset()
        appScreen = AppScreen.MULTIPLAYER
    }

    private fun attachGameSurface() {
        val surface = GLSurfaceView(this).apply {
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
        gameRenderer = GameRenderer()
        surface.setRenderer(gameRenderer)
        // After setRenderer, never before: GLSurfaceView has no GL thread
        // until a renderer is attached, and setRenderMode dereferences it.
        surface.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        setUpCameraControls(surface, gameRenderer)
        gameSurface = surface
        surfaceHost.addView(surface)
    }

    /**
     * M10: quitting is a long press on the undo button rather than an entry in
     * the overflow menu (rm's call). It keeps its confirmation - it abandons
     * the game outright, with no saving or rejoining - and a long press is
     * hard enough to do by accident that the pairing is safe.
     */
    private fun confirmQuitToMenu() {
        hudState.dialog = HudDialog.ListChoice(
            title = "Leave this game?",
            items = listOf("Yes, quit to menu"),
            cancelLabel = "Cancel",
            onSelect = {
                hudState.dialog = HudDialog.None
                quitToMenu()
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    /**
     * M9: end the game and go back to the menu.
     *
     * Order matters. The tick loop is stopped first so nothing is mid-call
     * into the engine when it goes away; then the engine is torn down (see
     * stopGame in engine_jni.cpp, and testServerRestart for the evidence that
     * a second game really can start afterwards); then the GL surface is
     * destroyed, which is what makes the next game's context - and so the
     * renderer's whole cache - genuinely fresh.
     */
    private fun quitToMenu() {
        gameJob?.cancel()
        gameJob = null
        NativeBridge.stopGame()
        if (::gameSurface.isInitialized) {
            surfaceHost.removeView(gameSurface)
        }
        hudState.reset()
        tutorial = null
        music?.setState(MusicPlayer.State.WAIT)
        aimSeeded = false
        lastChatVersion = 0
        lastChatLineId = 0
        appScreen = AppScreen.MENU
    }

    private suspend fun CoroutineScope.startAsHost() {
        hudState.statusText = "Starting local game..."
        val gameOk = withContext(Dispatchers.Default) { NativeBridge.startLocalGame(BuildConfig.DEBUG) }
        if (!gameOk) {
            hudState.statusText = "Failed to start local game (see logcat)"
            return
        }
        updateHostingLabel()
        runTickLoop()
    }

    // M5 Phase 2: joining side of the host/join choice above. Reuses the
    // same "Find Games" LAN-discovery dialog as the informational one that
    // already existed (see showFindGames doc comment history) - now tapping
    // a result actually connects, and there's a manual host:port entry too
    // for a PC host (or a device not advertising via NSD).
    /**
     * The second half of joining: the connection is open, so pump the
     * handshake through to sJoined and then play. Split from the finding and
     * connecting half (startJoinFlow) because only this part belongs on the
     * game screen.
     */
    private suspend fun CoroutineScope.awaitJoinAndPlay() {
        // The handshake itself (connect -> auth -> mod-check -> load-level,
        // see ClientContext.hpp) only advances as tickEngine() pumps the
        // network, same as everything else - so this loop has to run
        // (renamed) tickEngine() from the very start, not just once joined.
        while (isActive) {
            withContext(Dispatchers.Default) { NativeBridge.tickEngine() }
            val state = withContext(Dispatchers.Default) { NativeBridge.getClientJoinState() }
            if (state == ClientJoinState.JOINED) break
            if (state == ClientJoinState.FAILED) {
                val reason = withContext(Dispatchers.Default) { NativeBridge.getClientFailureReason() }
                hudState.statusText = "Join failed: $reason"
                return
            }
            hudState.statusText = "Connecting... (state $state)"
            kotlinx.coroutines.delay(100)
        }

        runTickLoop()
    }

    // Blocks (suspends) until the user picks a discovered game or types a
    // host:port manually, or cancels (null). A thin wrapper around
    // showFindGames's dialog plus a manual-entry option, since a PC host or
    // an NSD-blocked network has nothing to discover.
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    private suspend fun pickJoinTarget(): Pair<String, Int>? =
        kotlinx.coroutines.suspendCancellableCoroutine { cont ->
            showFindGames(
                onSelected = { host, port -> if (cont.isActive) cont.resume(host to port) {} },
                onCancelled = { if (cont.isActive) cont.resume(null) {} },
            )
        }

    // Drives the real game simulation forward every 100ms, in either role -
    // tickEngine()/getMyStatusLabel()/getCurrentWeaponName() are all
    // mode-agnostic now (see engine_jni.cpp's activeContext()). Host mode's
    // tickEngine() call is what actually advances ServerState (waiting for
    // players -> new level -> buying -> playing); client mode's just pumps
    // ClientContext::tick(). Rendering happens separately, driven by
    // GLSurfaceView's own thread (GameRenderer).
    private suspend fun CoroutineScope.runTickLoop() {
        while (isActive) {
            withContext(Dispatchers.Default) {
                NativeBridge.tickEngine()
                // M3: SoundAction events queued this tick (see
                // SoundEventQueue.h) - played via Android's own media
                // stack, not vendored OpenAL/OGG (see the porting plan).
                for (soundPath in NativeBridge.pollSoundEvents()) {
                    SoundPlayer.play(soundPath)
                }
            }
            // M5: a plain-language "what's happening / can I fire" label
            // (see getMyStatusLabel()) - replaces the raw ServerState-enum
            // debug string this used to show, which was never meant as a
            // real HUD and left a real player with no way to tell whether
            // they were in a buying phase, a live round, or waiting -
            // "there's no strict turn order, so how do I know when it's my
            // turn to shoot" was the direct report that prompted this.
            // Falls back to the debug string before a tank of ours exists
            // yet (label is "" during connect/buying-roster setup).
            val label = withContext(Dispatchers.Default) { NativeBridge.getMyStatusLabel() }
            val baseStatus = label.ifEmpty {
                withContext(Dispatchers.Default) { NativeBridge.getGameStateDebugString() }
            }
            // M6: how long is left in the current phase. Both timed phases
            // end on a deadline the player otherwise can't see - a buying
            // phase that closes mid-purchase, or a shot clock that expires
            // while you are still nudging the sliders, both just happen.
            // Prefixed to the status rather than given its own line, so the
            // battlefield keeps the space.
            // M6: a granted move id means it is our turn to act again, so
            // whatever we committed last round has been played out. See
            // GameHudState.shotLocked - the Fire button reads this.
            // M6 name plates - the renderer projected these on its own
            // thread last frame; this just picks up the result.
            hudState.tankOverlays = parseTankOverlays(gameRenderer.nativeGetTankOverlays())
            hudState.floatingLabels = parseFloatingLabels(gameRenderer.nativeGetFloatingLabels())

            // M6 parity: the scoreboard between rounds. Upstream puts it up
            // by itself and holds the game there for RoundScoreTime (5s), or
            // ScoreTime (15s) after the last round; before this the port
            // sat through that pause showing the empty battlefield, and the
            // player had to know to open the table by hand. Opened only
            // over an idle HUD - a dialog the player opened themselves is
            // never yanked away - and closed again only if this is the one
            // that opened it.
            val scoreboard = withContext(Dispatchers.Default) { NativeBridge.getScoreboardState() }
            if (scoreboard != 0 && autoScoreDialog == null &&
                hudState.dialog is HudDialog.None) {
                autoScoreDialog = showScores()
            } else if (scoreboard == 0 && autoScoreDialog != null) {
                if (hudState.dialog === autoScoreDialog) hudState.dialog = HudDialog.None
                autoScoreDialog = null
            }

            tutorial?.observe(hudState)

            val moveId = withContext(Dispatchers.Default) { NativeBridge.getMyMoveId() }
            if (moveId != 0) hudState.shotLocked = false

            val seconds = withContext(Dispatchers.Default) { NativeBridge.getPhaseSecondsRemaining() }
            hudState.statusText = if (seconds >= 0) "${seconds}s | $baseStatus" else baseStatus
            // M6: drives the contextual "done buying" button - it only
            // exists during the buying phase, which is the one time it
            // does anything (see ServerPlayedMoveHandler's eFinishedBuy).
            hudState.buyingPhase = label.startsWith("Buying")
            // M15: upstream's music follows its client state - buying,
            // playing, a shot in flight, the score screen - and these are the
            // same signals the status line is already built from.
            music?.setState(
                when {
                    scoreboard != 0 -> MusicPlayer.State.SCORE
                    hudState.buyingPhase -> MusicPlayer.State.BUYING
                    hudState.shotLocked -> MusicPlayer.State.SHOT
                    else -> MusicPlayer.State.PLAYING
                }
            )
            // M4: keep the weapon-select button's label in sync with
            // the current weapon, in case it changed via the shop
            // dialog or a fresh round's default selection.
            val weaponName = withContext(Dispatchers.Default) {
                NativeBridge.getCurrentWeaponName()
            }
            hudState.weaponLabel = weaponName.ifEmpty { "Weapon" }
            // M6 parity: wind really does perturb shots (see TankLib's
            // windoffsetFB) but nothing ever showed it - upstream has a
            // wind dialog of its own (SHOW_WIND_DIALOG). Rendered as a
            // compass-style bearing to match the angle slider's
            // "clockwise from up" convention (see fireFromSliders).
            val wind = withContext(Dispatchers.Default) { NativeBridge.getWindInfo() }
            hudState.windLabel = formatWindLabel(wind)
            // M6 tank movement: Fuel and friends are used by tapping the
            // ground rather than by aiming, so the HUD needs to know which
            // mode the battlefield tap is in - see handleBattlefieldTap.
            val positionSelect = withContext(Dispatchers.Default) {
                NativeBridge.getPositionSelect()
            }
            hudState.positionSelectWeapon =
                positionSelect.split("|").getOrNull(1).orEmpty()
            // Development perf readout - see GameHudState.perfLabel. Debug
            // builds only: it is a diagnostic that has earned its keep
            // several times over (it is what found the trees not drawing),
            // but it has no business on screen in a release.
            if (BuildConfig.DEBUG) {
                val stats = gameRenderer.nativeGetFrameStats().split("|")
                val fps = stats.getOrNull(0).orEmpty()
                val calls = stats.getOrNull(1).orEmpty()
                val targets = stats.getOrNull(2).orEmpty()
                hudState.perfLabel = if (fps.isEmpty()) {
                    ""
                } else {
                    "$fps fps | $calls draws | $targets targets"
                }
            }
            // M6: seed the aiming sliders from where the tank is actually
            // pointing, once, as soon as we have a tank - the engine gives
            // every tank a real starting turret rotation, so leaving the
            // sliders at a flat 0 meant the UI disagreed with the tank and
            // the first shot never went where the sliders said.
            if (!aimSeeded) {
                val aim = withContext(Dispatchers.Default) { NativeBridge.getMyAim() }
                if (seedAimFromEngine(aim)) aimSeeded = true
            }
            // M6 parity: the simulation-speed multiplier, shown only when
            // it is not 1x - upstream's SpeedChange draws it on the same
            // condition.
            val speed = withContext(Dispatchers.Default) { NativeBridge.getSimulationSpeed() }
            val speedParts = speed.split("|")
            val num = speedParts.getOrNull(0)?.toIntOrNull() ?: 1
            val den = speedParts.getOrNull(1)?.toIntOrNull() ?: 1
            hudState.speedLabel = when {
                num == den -> ""
                den == 1 -> "Speed: ${num}x"
                else -> "Speed: 1/${den}x"
            }
            // M6 parity: new chat. The version check keeps this to one cheap
            // int most ticks - the strings are only crossed over the JNI
            // boundary when something was actually said.
            val chatVersion = withContext(Dispatchers.Default) { NativeBridge.getChatVersion() }
            if (chatVersion != lastChatVersion) {
                lastChatVersion = chatVersion
                val fresh = withContext(Dispatchers.Default) {
                    parseChatLines(NativeBridge.getChatLines(lastChatLineId))
                }
                if (fresh.isNotEmpty()) {
                    lastChatLineId = fresh.last().id
                    // Each gets its own arrival stamp here, which is what
                    // lets the HUD expire them independently.
                    val now = System.currentTimeMillis()
                    hudState.chatToasts = hudState.chatToasts + fresh.map { ChatToast(it, now) }
                }
            }
            kotlinx.coroutines.delay(100)
        }
    }

    /**
     * The player-facing compass dial (0 up/north, 90 right/east, clockwise)
     * converted to the bearing the engine takes, which turns the other way:
     * `TankLib::getVelocityVector` fires along `(-sin(xy), cos(xy))`, so
     * engine 0 is north and engine 90 is *west*. A compass and a
     * counter-clockwise bearing are mirror images, hence `360 - d`.
     *
     * This was an identity mapping for a while, and it genuinely measured
     * correct at the time - because the renderer was drawing the whole world
     * mirrored (see worldZFromEngineY in renderer_jni.cpp), which reversed
     * the apparent sweep on screen and cancelled this one. Correcting the
     * renderer's handedness uncovered it: the dial started sweeping the
     * barrel backwards again. Two mirrors cancelling is exactly the trap
     * this port kept falling into, so: this one is derived, and the sweep
     * was then checked on a top-down view.
     */
    private fun engineAngleFromDial(dialDegrees: Float): Float =
        ((360f - dialDegrees) % 360f + 360f) % 360f

    /** Inverse of [engineAngleFromDial] - a mirror is its own inverse. */
    private fun dialAngleFromEngine(engineDegrees: Float): Float =
        engineAngleFromDial(engineDegrees)

    // M6: applies "angleDegrees|elevationDegrees|powerFraction" from
    // NativeBridge.getMyAim() to the sliders. Returns whether it applied -
    // false while there's no tank yet, so the caller can keep trying.
    //
    // Angle and elevation are taken from the tank; power is not. The engine
    // starts every tank at full power, which is a poor opening shot and an
    // awkward slider position to nudge down from, so the round opens at
    // DEFAULT_POWER_FRACTION instead - and is pushed straight back to the
    // engine, because the gun and aim sight read TanketShotInfo, not the
    // sliders. Setting the slider alone would put the UI back to claiming a
    // power the tank does not have, which is the exact bug that seeding was
    // introduced to fix.
    private fun seedAimFromEngine(raw: String): Boolean {
        val parts = raw.split("|")
        val engineAngle = parts.getOrNull(0)?.toFloatOrNull() ?: return false
        val elevation = parts.getOrNull(1)?.toFloatOrNull() ?: return false

        currentAngleDegrees = dialAngleFromEngine(engineAngle)
        currentElevationDegrees = elevation.coerceIn(0f, 90f)
        currentPowerFraction = DEFAULT_POWER_FRACTION
        hudState.angleDegrees = currentAngleDegrees
        hudState.elevationDegrees = currentElevationDegrees
        hudState.powerFraction = currentPowerFraction
        pushAimToEngine()
        return true
    }

    // M6 parity: turns NativeBridge.getWindInfo()'s "speed|angle" into a
    // HUD line with a direction arrow.
    //
    // Wind's own angle convention is already the player-friendly one and,
    // conveniently, the same as the angle slider's: Wind.cpp builds its
    // direction as (sin(angle), cos(angle)), so 0 = up and 90 = right
    // (clockwise from up). That's the opposite rotation direction from the
    // engine's *fire* angle (vx = -sin, vy = cos - see fireFromSliders),
    // so unlike the fire angle this needs no mirroring to display.
    private fun formatWindLabel(raw: String): String {
        val parts = raw.split("|")
        val speed = parts.getOrNull(0)?.toFloatOrNull() ?: return ""
        val angle = parts.getOrNull(1)?.toFloatOrNull() ?: return ""
        if (speed <= 0.01f) return "Wind: none"
        val arrows = listOf("↑", "↗", "→", "↘", "↓", "↙", "←", "↖")
        // Round to the nearest 45-degree bucket rather than truncating, so
        // e.g. 169 degrees reads as "down" instead of "down-right".
        val arrow = arrows[(((angle + 22.5f) / 45f).toInt() % 8 + 8) % 8]
        return "Wind: %.1f %s %.0f°".format(speed, arrow, angle)
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
    // ray-cast (nativePickTerrain + aimAtPoint), so the old normalized-space
    // handleTap has been deleted rather than left lying around unused.
    //
    // ScaleGestureDetector owns pinch-zoom; a plain last-position diff
    // drives orbit drag, suppressed while a scale gesture is in progress
    // (or just ended) so a two-finger pinch doesn't also register as a
    // one-finger drag on whichever pointer stayed down.
    @SuppressLint("ClickableViewAccessibility")
    private fun setUpCameraControls(surface: GLSurfaceView, renderer: GameRenderer) {
        // Anything under this much movement is a tap, not a drag. Taken from
        // the platform's own scaled touch slop so it matches every other
        // Android app on this screen density rather than a guessed pixel
        // count.
        val tapSlopPx = android.view.ViewConfiguration.get(this).scaledTouchSlop.toFloat()

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
        var downTime = 0L
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
            this,
            object : android.view.ScaleGestureDetector.SimpleOnScaleGestureListener() {
                override fun onScale(detector: android.view.ScaleGestureDetector): Boolean {
                    renderer.nativeCameraZoom(detector.scaleFactor)
                    return true
                }
            },
        )

        surface.setOnTouchListener { _, event ->
            scaleDetector.onTouchEvent(event)

            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastX = event.x
                    lastY = event.y
                    dragging = true
                    downX = event.x
                    downY = event.y
                    downTime = event.eventTime
                    multiTouched = false
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    // A second finger rules the gesture out as a tap.
                    multiTouched = true
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
                        renderer.nativeCameraDrag(dx, dy)
                    }
                    if (panning && event.pointerCount >= 2) {
                        // Pan and pinch run together rather than one winning:
                        // they read different things from the same two
                        // fingers (centroid movement vs. span change), so
                        // moving and zooming at once behaves the way it does
                        // in any map app.
                        val (fx, fy) = focusOf(event)
                        renderer.nativeCameraPan(fx - lastFocusX, fy - lastFocusY)
                        lastFocusX = fx
                        lastFocusY = fy
                    }
                    lastX = event.x
                    lastY = event.y
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
                    val movedX = event.x - downX
                    val movedY = event.y - downY
                    val moved = kotlin.math.hypot(movedX, movedY)
                    val heldMs = event.eventTime - downTime
                    if (!multiTouched && moved <= tapSlopPx && heldMs <= TAP_MAX_MS) {
                        handleBattlefieldTap(event.x, event.y)
                    }
                    dragging = false
                    panning = false
                }
                MotionEvent.ACTION_CANCEL -> {
                    dragging = false
                    panning = false
                }
            }
            true
        }
    }

    // Slider-based aiming (see GameHud.kt's angle/power sliders and the
    // Fire button) - fires directly with the slider-set angle/elevation/
    // power, the same NativeBridge.fireWeapon() call the drag gesture uses,
    // just triggered explicitly instead of on gesture release. This is the
    // precise/repeatable path for small between-round adjustments; the
    // battlefield tap/drag gestures above remain as the quick/casual one.
    //
    // The angle slider is deliberately a "clockwise from up" dial for the
    // player (0=forward/up, 90=right, 180=back, 270=left - the usual
    // clock/compass-face reading), while the engine's bearing runs
    // counter-clockwise - see engineAngleFromDial for the conversion.
    private fun fireFromSliders() {
        val engineAngle = engineAngleFromDial(currentAngleDegrees)
        CoroutineScope(Dispatchers.Main).launch {
            val fired = withContext(Dispatchers.Default) {
                val myTankId = NativeBridge.getMyTankId()
                if (myTankId != 0) {
                    NativeBridge.fireWeapon(myTankId, engineAngle, currentElevationDegrees, currentPowerFraction)
                } else {
                    false
                }
            }
            if (fired) {
                // Remember what we actually fired so "revert to last
                // angles" can restore it (see showActionsMenu).
                lastFiredAim = Triple(currentAngleDegrees, currentElevationDegrees, currentPowerFraction)
                // The shot is committed but nothing flies until every player
                // has committed too - the tick loop clears this once the
                // server grants the next move.
                hudState.shotLocked = true
            }
        }
    }

    // M6 tap-to-aim - upstream's AUTO_AIM ("Aim at point"), which the
    // control-parity audit flagged as a real binding rather than a
    // convenience. Tapping the ground casts a ray against the terrain (see
    // GameRenderer.nativePickTerrain) and swings the turret to face the
    // hit point, leaving elevation and power alone: it aims *at* a
    // direction, it does not solve the shot for you.
    //
    // The battlefield tap was free - since the 3D camera landed, dragging
    // orbits and a plain tap did nothing at all.
    /**
     * Acknowledges a purchase at once, then reconciles it with the engine.
     *
     * A buy is a queued simulator action, not an immediate one:
     * ServerSimulator only promotes it at a send boundary a couple of
     * fixed-seconds out, so the real count can take several seconds to
     * appear. The shop used to read straight back after sending and so
     * showed the *old* row - during a ~20 second buying phase that reads as
     * "the tap did nothing", and there is no time to close and reopen the
     * shop to find out otherwise.
     *
     * So the row goes to "buying..." and flashes immediately (the money is
     * deducted locally too), and this then polls until the engine confirms.
     * Whatever the engine reports wins in the end, so a purchase the server
     * rejects corrects itself rather than leaving a lie on screen.
     */
    private suspend fun awaitPurchase(shop: HudDialog.Shop, weapon: WeaponShopEntry) {
        shop.markPending(weapon.accessoryId, weapon.price)

        // ~4s at 120ms. Longer than the couple of seconds a send boundary
        // needs, short enough that a rejected buy doesn't sit as
        // "buying..." for the rest of the phase.
        repeat(34) {
            delay(120)
            val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
            val entries = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
            }
            val updated = entries.firstOrNull { it.accessoryId == weapon.accessoryId }
            if (updated != null && updated.ownedCount != weapon.ownedCount) {
                shop.settle(weapon.accessoryId, money, entries)
                return
            }
        }

        // Never confirmed - show whatever is actually true now.
        val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
        val entries = withContext(Dispatchers.Default) {
            parseWeaponShop(NativeBridge.getWeaponShop())
        }
        shop.settle(weapon.accessoryId, money, entries)
    }

    /**
     * A tap on the battlefield: normally aims, but with a position-select
     * weapon current (Fuel, Rocket Fuel, Teleport) it *uses* that weapon on
     * the tapped square instead - which for the fuel weapons means driving
     * the tank there. Upstream splits the same way, in
     * TargetCamera::landIntersect.
     *
     * The pick is done once here and handed to whichever branch, rather
     * than in each, so a tap can't ray-cast twice.
     */
    private fun handleBattlefieldTap(screenX: Float, screenY: Float) {
        // M11: with tap-to-aim off, a tap on the battlefield does nothing and
        // the sliders are the only way to aim. Position-selecting weapons are
        // the exception - Fuel and Teleport have no other way to choose a
        // square, so turning aiming off must not take them with it.
        if (!settings.tapToAim && hudState.positionSelectWeapon.isEmpty()) return
        CoroutineScope(Dispatchers.Main).launch {
            val hit = withContext(Dispatchers.Default) {
                gameRenderer.nativePickTerrain(screenX, screenY)
            }
            val parts = hit.split("|")
            val landscapeX = parts.getOrNull(0)?.toFloatOrNull() ?: return@launch
            val landscapeY = parts.getOrNull(1)?.toFloatOrNull() ?: return@launch

            if (hudState.positionSelectWeapon.isNotEmpty()) {
                val used = withContext(Dispatchers.Default) {
                    NativeBridge.firePositionSelect(landscapeX, landscapeY)
                }
                // Upstream's click handler simply returns when the square is
                // out of reach, which on a touch screen is indistinguishable
                // from a missed tap - so say it instead. The reachable area
                // is painted on the ground either way.
                if (!used) {
                    Toast.makeText(
                        this@MainActivity,
                        "Out of range for ${hudState.positionSelectWeapon}",
                        Toast.LENGTH_SHORT,
                    ).show()
                }
                return@launch
            }

            aimAtLandscapePoint(landscapeX, landscapeY)
        }
    }

    /** Swings the turret to face an already-picked landscape point. */
    private suspend fun aimAtLandscapePoint(landscapeX: Float, landscapeY: Float) {
        val angle = withContext(Dispatchers.Default) {
            NativeBridge.aimAtPoint(landscapeX, landscapeY)
        }
        if (angle < 0f) return

        // aimAtPoint has already moved the real turret; this just keeps
        // the dial showing what the tank is actually doing.
        currentAngleDegrees = dialAngleFromEngine(angle)
        hudState.angleDegrees = currentAngleDegrees
    }

    // M6: pushes the current slider values onto "my tank"'s real turret so
    // the rendered gun and the aim sight follow the player's aim live.
    // Without this the sliders were Kotlin-only state handed over at fire
    // time, so the turret (and therefore the sight, which reads
    // TanketShotInfo) never moved until a shot was actually fired - which
    // is exactly how it looked: adjusting angle or elevation changed
    // nothing on screen.
    private fun pushAimToEngine() {
        CoroutineScope(Dispatchers.Main).launch {
            withContext(Dispatchers.Default) {
                NativeBridge.setAim(
                    engineAngleFromDial(currentAngleDegrees),
                    currentElevationDegrees,
                    currentPowerFraction,
                )
            }
        }
    }

    // M6 parity: fire-and-forget submission of a non-shot move type (see
    // NativeBridge.submitMove). Skip and done-buying are common enough to
    // be their own HUD buttons rather than menu entries.
    private fun submitMoveAsync(moveType: Int) {
        CoroutineScope(Dispatchers.Main).launch {
            withContext(Dispatchers.Default) { NativeBridge.submitMove(moveType) }
        }
    }

    // M6 parity: upstream's UNDO_MOVE - puts the sliders back to the aim of
    // the last shot actually fired, for the usual fire/nudge/fire-again
    // loop. No-ops (with a nudge) before anything has been fired.
    private fun revertToLastAim() {
        val aim = lastFiredAim
        if (aim == null) {
            hudState.statusText = "Nothing fired yet to revert to"
            return
        }
        currentAngleDegrees = aim.first
        currentElevationDegrees = aim.second
        currentPowerFraction = aim.third
        hudState.angleDegrees = aim.first
        hudState.elevationDegrees = aim.second
        hudState.powerFraction = aim.third
        pushAimToEngine()
    }

    // M6 parity: the overflow menu - deliberately only holds things that
    // are genuinely rare. Everything a player reaches for regularly (fire,
    // undo, skip, done-buying, defenses, shop, weapon) is a direct button
    // on the HUD instead, so common actions never cost an extra tap.
    // M6 parity: the score / player list (upstream's SHOW_SCORE_DIALOG),
    // with the chat history under it. Every number is read straight off
    // TankScore, which this build already keeps - nothing here is simulated
    // or estimated. Refreshed while open so it tracks the round rather than
    // freezing at the moment it was opened.
    private fun showScores(): HudDialog.Scores {
        val dialog = HudDialog.Scores(
            entries = emptyList(),
            roundInfo = "",
            chat = emptyList(),
            onCancel = { hudState.dialog = HudDialog.None },
        )
        hudState.dialog = dialog

        CoroutineScope(Dispatchers.Main).launch {
            while (hudState.dialog === dialog) {
                val players = withContext(Dispatchers.Default) {
                    parsePlayerList(NativeBridge.getPlayerList())
                }
                val info = withContext(Dispatchers.Default) { NativeBridge.getRoundInfo() }
                // afterId 0 = the whole log the native store still holds
                // (bounded at 100 lines, see ChatStore.cpp).
                val chat = withContext(Dispatchers.Default) {
                    parseChatLines(NativeBridge.getChatLines(0))
                }
                dialog.entries = players
                dialog.roundInfo = info
                dialog.chat = chat
                delay(1000)
            }
        }
        return dialog
    }

    // Set while the between-rounds scoreboard is on screen because the
    // engine asked for it, so the tick below knows to take it down again -
    // and knows not to touch a score dialog the player opened themselves.
    private var autoScoreDialog: HudDialog.Scores? = null

    // M6 parity: simulation speed (upstream's SIMULATION_SPEED_* keys).
    // The seven upstream offers, no more: this is upstream's own
    // Simulator::setFast, so the set of speeds is its set, not a range
    // invented here.
    private fun showSimulationSpeed() {
        val speeds = listOf(
            "1/8 speed" to (1 to 8),
            "1/4 speed" to (1 to 4),
            "1/2 speed" to (1 to 2),
            "Normal speed" to (1 to 1),
            "2x speed" to (2 to 1),
            "4x speed" to (4 to 1),
            "8x speed" to (8 to 1),
        )
        hudState.dialog = HudDialog.ListChoice(
            title = "Game speed",
            items = speeds.map { it.first },
            cancelLabel = "Cancel",
            onSelect = { index ->
                val (numerator, denominator) = speeds[index].second
                CoroutineScope(Dispatchers.Main).launch {
                    val ok = withContext(Dispatchers.Default) {
                        NativeBridge.setSimulationSpeed(numerator, denominator)
                    }
                    // Joined clients follow the host's pace - say so rather
                    // than letting the tap look like it did nothing.
                    if (!ok) hudState.statusText = "Only the host can change the game speed"
                }
                hudState.dialog = HudDialog.None
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // M6 parity: upstream's camera presets (TargetCamera::CamType). The
    // camera *button* stays the quick free/follow toggle - it is the one
    // control reached mid-aim - so the fixed framings live here instead of
    // crowding it. Selecting one is a framing, not a mode lock: a drag drops
    // straight back out of it (see nativeCameraDrag).
    private fun showCameraPresets() {
        val presets = CameraPreset.entries
        hudState.dialog = HudDialog.ListChoice(
            title = "Camera view",
            items = presets.map { it.label },
            cancelLabel = "Cancel",
            onSelect = { index ->
                gameRenderer.nativeSetCameraPreset(index)
                // Keep the camera button's icon honest - selecting Free or
                // Follow moves the toggle it shows.
                hudState.cameraFollow = presets[index] == CameraPreset.FOLLOW
                hudState.dialog = HudDialog.None
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // Chat send. Off the main thread because it takes the engine mutex, which
    // the simulation tick holds for the duration of a step.
    private fun sendChatAsync(channel: String, text: String) {
        CoroutineScope(Dispatchers.Main).launch {
            val sent = withContext(Dispatchers.Default) { NativeBridge.sendChat(channel, text) }
            if (!sent) {
                hudState.statusText = "Could not send that message"
            }
        }
    }

    private fun showActionsMenu() {
        // Resigning ends your round, so it keeps a confirmation step rather
        // than firing off a single tap.
        val entries = listOf<Pair<String, () -> Unit>>(
            // Scores/chat, the camera views and the game speed used to be
            // here; each now hangs off a long press on the button it
            // extends - message, camera, and skip-turn respectively. Only
            // genuinely rare things are left behind the overflow.
            (if (hudState.hudHidden) "Show HUD" else "Hide HUD") to {
                hudState.hudHidden = !hudState.hudHidden
                hudState.dialog = HudDialog.None
            },
            "Resign round..." to {
                hudState.dialog = HudDialog.ListChoice(
                    title = "Resign this round?",
                    items = listOf("Yes, resign"),
                    cancelLabel = "Cancel",
                    onSelect = {
                        submitMoveAsync(MoveType.RESIGN)
                        hudState.dialog = HudDialog.None
                    },
                    onCancel = { hudState.dialog = HudDialog.None },
                )
            },
        )

        hudState.dialog = HudDialog.ListChoice(
            title = "More actions",
            items = entries.map { it.first },
            cancelLabel = "Close",
            onSelect = { index -> entries[index].second() },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // M4 economy, M6 parity: exercises the
    // getMyMoney/getWeaponShop/buyAccessory/selectWeapon JNI surface (see
    // engine_jni.cpp) - lets the human player buy accessories with real
    // AccessoryStore/TankScore state and switch between owned ones.
    // Tapping a row buys one unit if unowned; if already owned, a weapon
    // becomes the current weapon and a defense accessory is activated
    // (see showDefenses() - the Shop is also a reasonable place to use one
    // you just bought). Renders via HudDialog.ListChoice (see
    // HudDialogs.kt).
    //
    // M6: the list now covers all five upstream accessory types, not just
    // weapons - shields/parachutes/batteries were previously unbuyable and
    // unusable (see the getWeaponShop() comment in engine_jni.cpp).
    private fun showWeaponShop() {
        CoroutineScope(Dispatchers.Main).launch {
            val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
            val weapons = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
            }

            hudState.dialog = HudDialog.Shop(
                money = money,
                entries = weapons,
                onSelect = { weapon ->
                    CoroutineScope(Dispatchers.Main).launch {
                        if (weapon.isOwned) {
                            withContext(Dispatchers.Default) {
                                val change = weapon.activationChange
                                if (weapon.isWeapon || change == null) {
                                    NativeBridge.selectWeapon(weapon.accessoryId)
                                } else {
                                    NativeBridge.useDefense(weapon.accessoryId, change)
                                }
                            }
                            hudState.dialog = HudDialog.None
                        } else {
                            // The server only accepts a buy during the
                            // Buying phase (see ServerBuyAccessoryHandler.cpp) -
                            // outside that window it silently no-ops with
                            // nothing but a server-console log line the
                            // player never sees, which looked exactly like
                            // "tapping doesn't do anything". Check first so
                            // there's at least a visible reason why.
                            val status = withContext(Dispatchers.Default) { NativeBridge.getMyStatusLabel() }
                            if (status.startsWith("Buying")) {
                                val sent = withContext(Dispatchers.Default) {
                                    NativeBridge.buyAccessory(weapon.accessoryId, true)
                                }
                                // Stay open and refresh rather than close:
                                // the buying phase is for kitting out, and
                                // reopening the shop after every purchase
                                // (then finding your place in the list
                                // again) was needless work. money and
                                // entries are dialog state precisely so this
                                // can update in place.
                                val shop = hudState.dialog
                                if (sent && shop is HudDialog.Shop) {
                                    awaitPurchase(shop, weapon)
                                }
                            } else {
                                hudState.dialog = HudDialog.Message(
                                    text = "You can only buy weapons during the buying phase (between rounds).",
                                    onDismiss = { hudState.dialog = HudDialog.None },
                                )
                            }
                        }
                    }
                },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    // M4: in-game weapon quick-switch, separate from the Shop dialog above
    // (buying vs. selecting are different actions upstream too - see
    // TankAccessorySimAction vs. TanketWeapon::setWeapon). Lists only
    // already-owned weapons (isOwned - a starting weapon is commonly
    // unlimited, ownedCount == -1, not "> 0"; filtering on ">0" excluded it
    // entirely, making even a fresh tank's own default weapon
    // unreachable here), reusing the same getWeaponShop() JNI surface as
    // the shop.
    //
    // M6: filters to weapons specifically now that getWeaponShop() also
    // returns shields/parachutes/batteries - selecting one of those as a
    // "current weapon" is meaningless (they're activated via useDefense -
    // see showDefenses below).
    private fun showWeaponQuickSelect() {
        CoroutineScope(Dispatchers.Main).launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop()).filter { it.isOwned && it.isWeapon }
            }

            if (owned.isEmpty()) {
                hudState.dialog = HudDialog.Message(
                    text = "No weapons owned yet - buy some in the Shop first.",
                    onDismiss = { hudState.dialog = HudDialog.None },
                )
                return@launch
            }

            val labels = owned.map { weapon ->
                val marker = if (weapon.isCurrentWeapon) "> " else "  "
                "$marker${weapon.name} (${weapon.ownedLabel})"
            }

            hudState.dialog = HudDialog.ListChoice(
                title = "Select weapon",
                items = labels,
                cancelLabel = "Cancel",
                onSelect = { index ->
                    val weapon = owned[index]
                    CoroutineScope(Dispatchers.Main).launch {
                        withContext(Dispatchers.Default) {
                            NativeBridge.selectWeapon(weapon.accessoryId)
                        }
                    }
                    hudState.dialog = HudDialog.None
                },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    // M6 parity: the defense panel - raise/lower shields, enable/disable
    // parachutes, use a battery to repair. Upstream binds these to keys
    // (ENABLE_SHIELDS/ENABLE_PARACHUTES/USE_BATTERY in data/keys.xml) and
    // routes them through ComsDefenseMessage; ScorchDroid had no path to
    // any of it before M6 (see NativeBridge.useDefense). Lists owned
    // shields/parachutes/batteries plus explicit "down" entries for
    // whatever is currently up.
    private fun showDefenses() {
        CoroutineScope(Dispatchers.Main).launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
                    .filter { it.isOwned && it.activationChange != null }
            }
            val active = withContext(Dispatchers.Default) { NativeBridge.getActiveDefenses() }
            val activeParts = active.split("|")
            val activeShield = activeParts.getOrNull(0).orEmpty()
            val activeParachute = activeParts.getOrNull(1).orEmpty()

            // Each entry is a label plus the action to run when tapped, so
            // the "turn it off" rows can sit in the same list as the owned
            // accessories without a parallel index-mapping to get wrong.
            val entries = mutableListOf<Pair<String, () -> Unit>>()
            for (item in owned) {
                val change = item.activationChange ?: continue
                val activeMarker = when {
                    item.type == AccessoryType.SHIELD && item.name == activeShield -> "> "
                    item.type == AccessoryType.PARACHUTE && item.name == activeParachute -> "> "
                    else -> "  "
                }
                val verb = if (item.type == AccessoryType.BATTERY) "use" else "activate"
                entries += "$activeMarker${item.name} [${item.type}] x${item.ownedLabel} - $verb" to {
                    CoroutineScope(Dispatchers.Main).launch {
                        withContext(Dispatchers.Default) { NativeBridge.useDefense(item.accessoryId, change) }
                    }
                    hudState.dialog = HudDialog.None
                }
            }
            if (activeShield.isNotEmpty()) {
                entries += "Lower shield ($activeShield)" to {
                    CoroutineScope(Dispatchers.Main).launch {
                        withContext(Dispatchers.Default) { NativeBridge.useDefense(0, DefenseChange.SHIELD_DOWN) }
                    }
                    hudState.dialog = HudDialog.None
                }
            }
            if (activeParachute.isNotEmpty()) {
                entries += "Disable parachutes ($activeParachute)" to {
                    CoroutineScope(Dispatchers.Main).launch {
                        withContext(Dispatchers.Default) { NativeBridge.useDefense(0, DefenseChange.PARACHUTES_DOWN) }
                    }
                    hudState.dialog = HudDialog.None
                }
            }

            if (entries.isEmpty()) {
                hudState.dialog = HudDialog.Message(
                    text = "No defenses owned yet - buy shields, parachutes or batteries in the Shop.",
                    onDismiss = { hudState.dialog = HudDialog.None },
                )
                return@launch
            }

            hudState.dialog = HudDialog.ListChoice(
                title = "Defenses",
                items = entries.map { it.first },
                cancelLabel = "Close",
                onSelect = { index -> entries[index].second() },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }


    // M5: shows "Hosting on <ip>:<port>" once startLocalGame() has bound a
    // real listening socket (see NativeBridge.isHostingOnNetwork/
    // engine_jni.cpp's startLocalGame), so another device has an address to
    // connect to. Falls back to explaining why not, rather than silently
    // doing nothing, if the port didn't bind.
    private fun updateHostingLabel() {
        CoroutineScope(Dispatchers.Main).launch {
            val hosting = withContext(Dispatchers.Default) { NativeBridge.isHostingOnNetwork() }
            val port = withContext(Dispatchers.Default) { NativeBridge.getServerPort() }
            hudState.hostingLabel = if (hosting) {
                val ip = getLocalIpAddress() ?: "unknown IP"
                "Hosting on $ip:$port"
                    .also { LanDiscovery.registerService(applicationContext, port) }
            } else {
                "Solo only - could not open port $port for LAN play"
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
    private fun showFindGames(onSelected: (String, Int) -> Unit = { _, _ -> }, onCancelled: () -> Unit = {}) {
        val found = mutableListOf<LanDiscovery.FoundGame>()
        var resolved = false

        fun manualEntryLabel() = "Enter address manually..."

        val listDialog = HudDialog.ListChoice(
            title = "Searching for LAN games...",
            items = listOf(manualEntryLabel()),
            cancelLabel = "Cancel",
            onSelect = { index ->
                resolved = true
                LanDiscovery.stopDiscovery()
                hudState.dialog = HudDialog.None
                if (index < found.size) {
                    val game = found[index]
                    onSelected(game.host, game.port)
                } else {
                    promptManualAddress(onSelected, onCancelled)
                }
            },
            onCancel = {
                LanDiscovery.stopDiscovery()
                hudState.dialog = HudDialog.None
                if (!resolved) onCancelled()
            },
        )
        hudState.dialog = listDialog

        fun refresh() {
            listDialog.items = found.map { "${it.name} - ${it.host}:${it.port}" } + manualEntryLabel()
        }

        LanDiscovery.startDiscovery(
            applicationContext,
            durationMs = 4000,
            onFound = { game ->
                if (found.none { it.host == game.host && it.port == game.port }) {
                    found.add(game)
                    refresh()
                }
            },
            onFinished = {
                listDialog.title = if (found.isEmpty()) "No LAN games found" else "Found ${found.size} game(s)"
            },
        )
    }

    // M5 Phase 2: falls back to a typed "host:port" when nothing useful
    // showed up via NSD - the only way to reach a PC host today, since
    // desktop Scorched3D doesn't advertise itself via Android's NSD/mDNS.
    private fun promptManualAddress(onSelected: (String, Int) -> Unit, onCancelled: () -> Unit) {
        hudState.dialog = HudDialog.ManualAddress(
            onConnect = { text ->
                val parts = text.trim().split(":")
                val host = parts.getOrNull(0)?.takeIf { it.isNotEmpty() }
                val port = parts.getOrNull(1)?.toIntOrNull()
                hudState.dialog = HudDialog.None
                if (host != null && port != null) {
                    onSelected(host, port)
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
    private fun getLocalIpAddress(): String? {
        return try {
            Collections.list(NetworkInterface.getNetworkInterfaces())
                .flatMap { Collections.list(it.inetAddresses) }
                .firstOrNull { !it.isLoopbackAddress && it is Inet4Address }
                ?.hostAddress
        } catch (e: Exception) {
            null
        }
    }

    override fun onResume() {
        super.onResume()
        if (appScreen == AppScreen.GAME && ::gameSurface.isInitialized) gameSurface.onResume()
        music?.resume()
    }

    override fun onPause() {
        super.onPause()
        if (appScreen == AppScreen.GAME && ::gameSurface.isInitialized) gameSurface.onPause()
        music?.pause()
    }

    override fun onDestroy() {
        music?.release()
        music = null
        super.onDestroy()
        LanDiscovery.stopRegistration()
        LanDiscovery.stopDiscovery()
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

    private companion object {
        // A press longer than this is a deliberate hold, not a tap - it
        // stops a slow, still finger from firing off an aim on release.
        const val TAP_MAX_MS = 250L


    }
}
