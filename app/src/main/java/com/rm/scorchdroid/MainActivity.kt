package com.rm.scorchdroid

import android.annotation.SuppressLint
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.view.MotionEvent
import android.view.WindowManager
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
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {
    private lateinit var gameSurface: GLSurfaceView
    private lateinit var gameRenderer: GameRenderer

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
    private var currentPowerFraction = 0.5f

    // M6 parity: upstream's UNDO_MOVE ("Revert to last angles") - the
    // angle/elevation/power of the last shot actually fired, so a player
    // can get back to it after nudging the sliders around. Null until
    // something has been fired this session.
    private var lastFiredAim: Triple<Float, Float, Float>? = null

    // M6: whether the aiming sliders have been seeded from the tank's real
    // starting turret rotation yet (see the tick loop). One-shot, so it
    // never fights the player's own adjustments afterwards.
    private var aimSeeded = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        hideSystemBars()
        // A round can sit idle for a while waiting on other players/bots,
        // with no touch input in between - without this the screen times
        // out mid-game exactly like any other idle app.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)
        hudState.statusText = NativeBridge.helloFromNative()

        gameSurface = findViewById(R.id.game_surface)
        gameSurface.setEGLContextClientVersion(3)
        // M6: the 3D renderer needs a real depth buffer (the M2/M5 flat 2D
        // view never did) - GLSurfaceView's default config chooser doesn't
        // reliably request one on every device, so ask explicitly.
        gameSurface.setEGLConfigChooser(8, 8, 8, 8, 16, 0)
        gameRenderer = GameRenderer()
        gameSurface.setRenderer(gameRenderer)
        gameSurface.renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        setUpCameraControls(gameSurface, gameRenderer)

        findViewById<ComposeView>(R.id.hud_compose_view).setContent {
            GameHud(
                state = hudState,
                onHelp = { showTutorial() },
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
                onSkip = { submitMoveAsync(MoveType.SKIP) },
                onDoneBuying = { submitMoveAsync(MoveType.FINISHED_BUY) },
            )
        }

        if (!hasSeenTutorial()) {
            showTutorial()
        }

        CoroutineScope(Dispatchers.Main).launch {
            hudState.statusText = "Extracting game data..."
            val dataRoot = withContext(Dispatchers.IO) {
                AssetDataExtractor.ensureExtracted(applicationContext)
            }
            val initOk = withContext(Dispatchers.Default) {
                NativeBridge.initEngine(dataRoot.absolutePath)
            }
            if (!initOk) {
                hudState.statusText = "Failed to initialize engine data root"
                return@launch
            }

            // M5 Phase 2: let the player pick a role before anything else
            // starts - startLocalGame()/startJoinGame() are mutually
            // exclusive in the native engine (see engine_jni.cpp), so this
            // has to be decided once, up front, rather than defaulting to
            // host and bolting joining on afterwards.
            when (chooseGameMode()) {
                GameMode.HOST -> startAsHost()
                GameMode.JOIN -> startAsClient()
            }
        }
    }

    private enum class GameMode { HOST, JOIN }

    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    private suspend fun chooseGameMode(): GameMode = kotlinx.coroutines.suspendCancellableCoroutine { cont ->
        hudState.dialog = HudDialog.GameModeChoice(
            onHost = { hudState.dialog = HudDialog.None; cont.resume(GameMode.HOST) {} },
            onJoin = { hudState.dialog = HudDialog.None; cont.resume(GameMode.JOIN) {} },
        )
    }

    private suspend fun CoroutineScope.startAsHost() {
        hudState.statusText = "Starting local game..."
        val gameOk = withContext(Dispatchers.Default) { NativeBridge.startLocalGame() }
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
    private suspend fun CoroutineScope.startAsClient() {
        hudState.statusText = "Choose a game to join..."
        val target = pickJoinTarget() ?: run {
            hudState.statusText = "No game selected - restart the app to try again"
            return
        }

        hudState.statusText = "Connecting to ${target.first}:${target.second}..."
        val connecting = withContext(Dispatchers.Default) {
            NativeBridge.startJoinGame(target.first, target.second)
        }
        if (!connecting) {
            hudState.statusText = "Failed to open a connection to ${target.first}:${target.second}"
            return
        }

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
            hudState.statusText = label.ifEmpty {
                withContext(Dispatchers.Default) { NativeBridge.getGameStateDebugString() }
            }
            // M6: drives the contextual "done buying" button - it only
            // exists during the buying phase, which is the one time it
            // does anything (see ServerPlayedMoveHandler's eFinishedBuy).
            hudState.buyingPhase = label.startsWith("Buying")
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
            // M6: seed the aiming sliders from where the tank is actually
            // pointing, once, as soon as we have a tank - the engine gives
            // every tank a real starting turret rotation, so leaving the
            // sliders at a flat 0 meant the UI disagreed with the tank and
            // the first shot never went where the sliders said.
            if (!aimSeeded) {
                val aim = withContext(Dispatchers.Default) { NativeBridge.getMyAim() }
                if (seedAimFromEngine(aim)) aimSeeded = true
            }
            kotlinx.coroutines.delay(100)
        }
    }

    // The engine's fire angle runs counterclockwise from world +Y, while
    // the angle slider is a clockwise-from-up dial (0=up, 90=right - see
    // fireFromSliders). Mirroring converts between them, and is its own
    // inverse, so the same call works in both directions.
    private fun mirrorAngle(degrees: Float): Float = ((360f - degrees) % 360f + 360f) % 360f

    // M6: applies "angleDegrees|elevationDegrees|powerFraction" from
    // NativeBridge.getMyAim() to the sliders. Returns whether it applied -
    // false while there's no tank yet, so the caller can keep trying.
    private fun seedAimFromEngine(raw: String): Boolean {
        val parts = raw.split("|")
        val engineAngle = parts.getOrNull(0)?.toFloatOrNull() ?: return false
        val elevation = parts.getOrNull(1)?.toFloatOrNull() ?: return false
        val power = parts.getOrNull(2)?.toFloatOrNull() ?: return false

        currentAngleDegrees = mirrorAngle(engineAngle)
        currentElevationDegrees = elevation.coerceIn(0f, 90f)
        currentPowerFraction = power.coerceIn(0f, 1f)
        hudState.angleDegrees = currentAngleDegrees
        hudState.elevationDegrees = currentElevationDegrees
        hudState.powerFraction = currentPowerFraction
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
    // this surface as a result; NativeBridge.handleTap() itself is left in
    // place native-side in case a ray-cast-based revival happens later.
    //
    // ScaleGestureDetector owns pinch-zoom; a plain last-position diff
    // drives orbit drag, suppressed while a scale gesture is in progress
    // (or just ended) so a two-finger pinch doesn't also register as a
    // one-finger drag on whichever pointer stayed down.
    @SuppressLint("ClickableViewAccessibility")
    private fun setUpCameraControls(surface: GLSurfaceView, renderer: GameRenderer) {
        var lastX = 0f
        var lastY = 0f
        var dragging = false

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
                }
                MotionEvent.ACTION_POINTER_DOWN -> {
                    // A second finger just went down - a pinch is starting,
                    // not a drag; stop treating pointer 0's movement as one.
                    dragging = false
                }
                MotionEvent.ACTION_MOVE -> {
                    if (dragging && event.pointerCount == 1 && !scaleDetector.isInProgress) {
                        val dx = event.x - lastX
                        val dy = event.y - lastY
                        renderer.nativeCameraDrag(dx, dy)
                    }
                    lastX = event.x
                    lastY = event.y
                }
                MotionEvent.ACTION_POINTER_UP -> {
                    // One finger lifted out of a multi-touch gesture - resume
                    // dragging from whichever pointer remains, next MOVE.
                    dragging = false
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    dragging = false
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
    // clock/compass-face reading), but the engine's own fire angle rotates
    // the other way (counterclockwise from world +Y - see the comment on
    // handleTap() in engine_jni.cpp: vx=-sin(angle), vy=cos(angle), so
    // increasing angle sweeps from +Y toward -X, not +X). mirrorAngle()
    // converts the player-facing dial into the engine's convention without
    // changing what the slider itself displays.
    private fun fireFromSliders() {
        val engineAngle = mirrorAngle(currentAngleDegrees)
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
                hudState.statusText = "Fired!\n${hudState.statusText}"
            }
        }
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
                NativeBridge.setAim(currentAngleDegrees, currentElevationDegrees, currentPowerFraction)
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
    private fun showActionsMenu() {
        // Resigning ends your round, so it keeps a confirmation step rather
        // than firing off a single tap.
        val entries = listOf<Pair<String, () -> Unit>>(
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

            val labels = weapons.map { weapon ->
                val marker = if (weapon.isCurrentWeapon) "> " else "  "
                val kind = if (weapon.isWeapon) "" else " [${weapon.type}]"
                "$marker${weapon.name}$kind - \$${weapon.price} (owned ${weapon.ownedLabel})"
            }

            hudState.dialog = HudDialog.ListChoice(
                title = "Shop - \$$money",
                items = labels,
                cancelLabel = "Close",
                onSelect = { index ->
                    val weapon = weapons[index]
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
                                withContext(Dispatchers.Default) { NativeBridge.buyAccessory(weapon.accessoryId, true) }
                                hudState.dialog = HudDialog.None
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

    // M4 tutorial: ScorchDroid's own onboarding content. Upstream's
    // data/tutorial.xml is entirely keyed to its GLW desktop-dialog widget
    // tree (step conditions like "WindowVisible" naming windows/controls
    // such as "Rules"/"Team"/"Ok" - see that file) - meaningless without
    // rebuilding those legacy dialogs, which the architecture explicitly
    // rules out (this port fully rewrites the UI layer - see the porting
    // plan). So this describes the controls actually built in this file
    // instead of trying to adapt that data.
    private data class TutorialStep(val title: String, val message: String)

    private val tutorialSteps = listOf(
        TutorialStep(
            "Welcome",
            "Welcome to ScorchDroid! This is the same Scorched3D game logic as the PC version, with touch controls built for Android.",
        ),
        TutorialStep(
            "Tap to fire",
            "Tap anywhere on the battlefield to fire your tank aimed at that point.",
        ),
        TutorialStep(
            "Drag to aim and set power",
            "Press and drag like a slingshot: your tank fires in the direction opposite the drag, and further drags fire with more power.",
        ),
        TutorialStep(
            "Elevation",
            "Use the vertical slider on the left edge of the screen to set your shot's elevation angle.",
        ),
        TutorialStep(
            "Shop",
            "Tap Shop to spend your money on new weapons.",
        ),
        TutorialStep(
            "Weapon",
            "The button at the bottom shows your current weapon. Tap it to switch between weapons you already own.",
        ),
        TutorialStep(
            "You're ready",
            "That's everything you need to play. Tap the ? button any time to see this again.",
        ),
    )

    private fun showTutorial(stepIndex: Int = 0) {
        val step = tutorialSteps[stepIndex]
        val isLast = stepIndex == tutorialSteps.lastIndex
        hudState.dialog = HudDialog.TutorialStep(
            title = "${step.title} (${stepIndex + 1}/${tutorialSteps.size})",
            message = step.message,
            isLast = isLast,
            onNext = {
                if (isLast) {
                    markTutorialSeen()
                    hudState.dialog = HudDialog.None
                } else {
                    showTutorial(stepIndex + 1)
                }
            },
            onSkip = if (isLast) null else {
                { markTutorialSeen(); hudState.dialog = HudDialog.None }
            },
        )
    }

    private fun hasSeenTutorial(): Boolean =
        getPreferences(MODE_PRIVATE).getBoolean(PREF_TUTORIAL_SEEN, false)

    private fun markTutorialSeen() {
        getPreferences(MODE_PRIVATE).edit().putBoolean(PREF_TUTORIAL_SEEN, true).apply()
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
        if (::gameSurface.isInitialized) gameSurface.onResume()
    }

    override fun onPause() {
        super.onPause()
        if (::gameSurface.isInitialized) gameSurface.onPause()
    }

    override fun onDestroy() {
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
        const val PREF_TUTORIAL_SEEN = "tutorial_seen"
    }
}
