package com.rm.scorchdroid

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AdminPanelSettings
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.DoneAll
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.GpsFixed
import androidx.compose.material.icons.filled.HourglassTop
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Public
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.ShoppingCart
import androidx.compose.material.icons.filled.TouchApp
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.Undo
import androidx.compose.material.icons.filled.Whatshot
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Surface
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Box
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import kotlinx.coroutines.delay
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt
import kotlinx.coroutines.delay

/**
 * M4: the real in-game HUD (see the porting plan) - Compose overlaid on the
 * GLSurfaceView, replacing the plain XML TextViews/Buttons/SeekBar that
 * shipped as an M4 stopgap while M5 multiplayer work took priority.
 *
 * A plain mutable holder rather than Compose `State` hoisted all the way up
 * through MainActivity: the tick loop, touch handlers, and native-bridge
 * polling in MainActivity are ordinary (non-Composable) Kotlin code that
 * needs a plain reference to write into every ~100ms/on every touch event,
 * not a @Composable function parameter - GameHud just observes whatever is
 * currently here and recomposes when it changes.
 */
/**
 * Firing power every round opens on, for both the slider and the tank
 * itself (MainActivity.seedAimFromEngine pushes it to the engine). The
 * engine's own starting value is full power, which is a poor opening shot
 * and an awkward slider position to nudge down from; half puts the thumb
 * mid-track with both directions equally reachable.
 *
 * Deliberately one constant shared by the HUD state and MainActivity: two
 * literals that had to agree is precisely how the slider once came to
 * claim a power the tank did not have.
 */
const val DEFAULT_POWER_FRACTION = 0.5f

/**
 * One tank's on-screen plate. Mirrors what upstream draws above each tank
 * (TargetRendererImplTank::drawNames/drawLife): the player name in that
 * player's colour, a health bar, and a shield bar when a shield is up.
 */
data class TankOverlay(
    val screenX: Float,
    val screenY: Float,
    val onScreen: Boolean,
    val alive: Boolean,
    val mine: Boolean,
    val life: Float,
    val shield: Float,
    val color: Color,
    val name: String,
)

fun parseTankOverlays(rows: Array<String>): List<TankOverlay> = rows.mapNotNull { row ->
    // The name is last and may itself contain the separator, so split with a
    // limit rather than assuming it doesn't.
    val p = row.split("|", limit = 11)
    if (p.size != 11) return@mapNotNull null
    TankOverlay(
        screenX = p[0].toFloatOrNull() ?: return@mapNotNull null,
        screenY = p[1].toFloatOrNull() ?: return@mapNotNull null,
        onScreen = p[2] == "1",
        alive = p[3] == "1",
        mine = p[4] == "1",
        life = p[5].toFloatOrNull() ?: 0f,
        shield = p[6].toFloatOrNull() ?: 0f,
        color = Color(
            (p[7].toFloatOrNull() ?: 1f).coerceIn(0f, 1f),
            (p[8].toFloatOrNull() ?: 1f).coerceIn(0f, 1f),
            (p[9].toFloatOrNull() ?: 1f).coerceIn(0f, 1f),
        ),
        name = p[10],
    )
}

class GameHudState {
    var statusText by mutableStateOf("")
    var hostingLabel by mutableStateOf("")
    var weaponLabel by mutableStateOf("Weapon")
    var elevationDegrees by mutableFloatStateOf(45f)
    // Slider-based aiming (see the porting plan's "aiming controls
    // direction" note): the drag-slingshot gesture on the battlefield
    // can't be repeated precisely between rounds, so angle/power get their
    // own sliders here, fired via an explicit Fire button - the drag and
    // tap gestures on the battlefield still work too, as a quick/casual
    // alternative, not replaced.
    var angleDegrees by mutableFloatStateOf(0f)
    var powerFraction by mutableFloatStateOf(DEFAULT_POWER_FRACTION)
    // M6: which camera mode the battlefield touch/pinch gestures currently
    // control - see renderer_jni.cpp's OrbitCamera. The actual mode lives
    // native-side; this mirrors it so the camera button can show the
    // matching icon (free-fly vs. locked to your tank).
    var cameraFollow by mutableStateOf(false)
    // M6: whether the round is currently in its buying phase - drives the
    // contextual "done buying" button, which is meaningless (and so hidden)
    // the rest of the time. Kept in sync from the tick loop's status poll.
    var buyingPhase by mutableStateOf(false)
    // M6: this round's shot is submitted and the round is waiting on the
    // other players. Simultaneous turns mean firing produces no immediate
    // feedback - nothing flies until everyone has committed - so without
    // this the Fire button looked like it had done nothing.
    var shotLocked by mutableStateOf(false)
    // M6: name plates, refreshed from the renderer's projection every tick.
    var tankOverlays by mutableStateOf<List<TankOverlay>>(emptyList())
    // M6: floating damage numbers and speech bubbles, world-anchored and
    // already projected by the renderer - it has no font, so they are drawn
    // here alongside the name plates.
    var floatingLabels by mutableStateOf<List<FloatingLabel>>(emptyList())
    // M6 parity: current wind (speed + direction) - it really does perturb
    // shots, and nothing showed it before. "" while there's no game yet.
    var windLabel by mutableStateOf("")
    // M6 tank movement: the name of the current weapon when it is used by
    // choosing a spot on the ground (Fuel, Rocket Fuel, Teleport) rather
    // than by aiming, else "". While it is set, a battlefield tap picks a
    // destination instead of aiming and the Fire button is inert - the same
    // split upstream makes (TankKeyboardControlUtil refuses the fire key
    // for a position-select weapon; the click handler does the work).
    var positionSelectWeapon by mutableStateOf("")
    // Whether this device is hosting, which is what decides if the admin
    // button is there at all. A joined client has no authority over anyone
    // else in the game, so it gets no button rather than a button that
    // fails - see NativeBridge.isGameHost.
    var isHost by mutableStateOf(false)
    // Development performance readout (frame rate / draw calls / targets).
    // Empty hides it. Deliberately plain text in the existing status
    // column rather than an overlay of its own - it is a temporary aid
    // while the renderer grows, and should be gated off before a release.
    var perfLabel by mutableStateOf("")
    // M4 dialog conversion (see HudDialogs.kt) - the currently-shown modal,
    // if any. A plain mutable field like the rest of this state holder,
    // since it's written from ordinary (non-Composable) Kotlin in
    // MainActivity.
    var dialog: HudDialog by mutableStateOf(HudDialog.None)

    // M6 parity: in-game chat. Messages appear briefly in the top-right,
    // under the session icons, newest at the top, each fading on its own
    // timer rather than the whole stack clearing at once - so a burst of
    // traffic doesn't wipe a line you were halfway through reading.
    var chatToasts by mutableStateOf<List<ChatToast>>(emptyList())
    // Whether the inline compose box is open. Deliberately not a HudDialog:
    // those are modal AlertDialogs that cover the battlefield, and typing a
    // message is something you do *while* watching the game.
    var chatComposing by mutableStateOf(false)
    // "general" or "team" - the two channels upstream lets a player speak
    // on. The rest are read-only or need admin authentication.
    var chatChannel by mutableStateOf("general")

    // M6 parity: upstream's HUD_ITEMS key, which toggles its HUD panel off.
    // Here that means everything except a single button to bring it back -
    // without one there would be no way to undo it on a touch screen, which
    // has no spare key to bind.
    var hudHidden by mutableStateOf(false)

    // M6 parity: the simulation-speed multiplier, shown only when it is not
    // 1x - upstream does the same (SpeedChange::draw prints "8.0X" only when
    // speed != 1.0), because a permanent "1x" would be noise.
    var speedLabel by mutableStateOf("")

    // M11 settings, mirrored here so the HUD reads one object rather than
    // reaching for preferences mid-composition. Set from GameSettings.
    var showNamePlates by mutableStateOf(true)
    var showHealthBars by mutableStateOf(true)
    var chatToastMillis by mutableStateOf(CHAT_TOAST_MILLIS)
    var leftHandMode by mutableStateOf(false)
    var controlOpacity by mutableFloatStateOf(1.0f)

    /**
     * M9: back to a fresh game's state, for quit-to-menu.
     *
     * The HUD state outlives a single game (it belongs to the Activity, not
     * the game), so without this the next game opens showing the last one's
     * status line, name plates, chat toasts and wind - which reads as the new
     * game being broken rather than the old one lingering.
     */
    fun reset() {
        statusText = ""
        hostingLabel = ""
        weaponLabel = "Weapon"
        elevationDegrees = 45f
        angleDegrees = 0f
        powerFraction = DEFAULT_POWER_FRACTION
        cameraFollow = false
        buyingPhase = false
        shotLocked = false
        tankOverlays = emptyList()
        floatingLabels = emptyList()
        windLabel = ""
        positionSelectWeapon = ""
        isHost = false
        perfLabel = ""
        dialog = HudDialog.None
        chatToasts = emptyList()
        chatComposing = false
        chatChannel = "general"
        hudHidden = false
        speedLabel = ""
    }
}

/** A chat line currently on screen, with the moment it arrived. */
data class ChatToast(val line: ChatLine, val shownAtMillis: Long)

/** How long each message stays on screen before it fades out. */
const val CHAT_TOAST_MILLIS = 5_000L

/**
 * How many messages may be on screen at once. The oldest goes as soon as
 * the limit is passed, without waiting out [CHAT_TOAST_MILLIS].
 *
 * A timer alone was enough while these were only what players typed. It
 * stopped being enough when the game's own announcements joined them: the
 * start of a match, and any weapon that kills several tanks at once, produce
 * a burst faster than five seconds can drain, and the stack grew until it
 * covered the screen. A message pushed off early is no loss - the score
 * dialog keeps the full history.
 */
const val MAX_CHAT_TOASTS = 20

@Composable
fun GameHud(
    state: GameHudState,
    onFindGames: () -> Unit,
    onShop: () -> Unit,
    onWeapon: () -> Unit,
    onElevationChange: (Float) -> Unit,
    onAngleChange: (Float) -> Unit,
    onPowerChange: (Float) -> Unit,
    onFire: () -> Unit,
    onToggleCamera: () -> Unit,
    onDefenses: () -> Unit,
    onActions: () -> Unit,
    onUndo: () -> Unit,
    onQuitToMenu: () -> Unit,
    onSkip: () -> Unit,
    onDoneBuying: () -> Unit,
    onScores: () -> Unit,
    onCameraPresets: () -> Unit,
    onSimulationSpeed: () -> Unit,
    onAdmin: () -> Unit,
    onAimGesture: (AimAxis, Boolean) -> Unit,
    onSendChat: (String) -> Unit,
) {
    // M6 parity: upstream's HUD_ITEMS toggle. Everything goes except one
    // button to bring it back - a keyboard can rebind the same key to
    // restore the HUD, a touch screen has nothing to press.
    if (state.hudHidden) {
        Box(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .windowInsetsPadding(WindowInsets.displayCutout)
                    .padding(12.dp),
            ) {
                HudIconButton(
                    icon = Icons.Filled.Visibility,
                    description = "Show the HUD again",
                    onClick = { state.hudHidden = false },
                )
            }
        }
        return
    }

    Box(modifier = Modifier.fillMaxSize()) {
        // The top edge: status text on the left, session controls on the
        // right, laid out as one row so that they cannot overlap.
        //
        // They used to be two independently aligned children of this box,
        // which meant the text ran the full width and straight under the
        // buttons - and the longest lines are exactly the ones that matter,
        // a Bluetooth hosting name among them. Giving the text whatever
        // width the buttons leave, rather than a guessed fraction of the
        // screen, stays right on any screen shape and with any number of
        // buttons - which varies already, since the admin one is the host's
        // alone. windowInsetsPadding(displayCutout) keeps the whole row
        // clear of a camera cutout.
        Row(
            modifier = Modifier
                .align(Alignment.TopStart)
                .fillMaxWidth()
                .windowInsetsPadding(WindowInsets.displayCutout),
        ) {
            // Status line (phase/turn/aim feedback - see
            // NativeBridge.getMyStatusLabel()) plus the hosting address once
            // bound.
            Column(
                modifier = Modifier
                    .weight(1f)
                    .padding(16.dp),
            ) {
                if (state.statusText.isNotEmpty()) HudText(state.statusText)
                if (state.hostingLabel.isNotEmpty()) HudText(state.hostingLabel)
                if (state.windLabel.isNotEmpty()) HudText(state.windLabel)
                if (state.speedLabel.isNotEmpty()) HudText(state.speedLabel)
                // Upstream shows "Click ground to activate {0}" as a banner the
                // moment such a weapon is selected; this is the same prompt in
                // the place this HUD already puts status.
                if (state.positionSelectWeapon.isNotEmpty()) {
                    HudText("Tap the ground to use ${state.positionSelectWeapon}")
                }
                if (state.perfLabel.isNotEmpty()) HudText(state.perfLabel)
            }

            // Top-right: "session / view" controls - things about this session
            // or how you're looking at it, not about the fight itself. Icon-only
            // and tightly stacked; a wall of word-labelled buttons was eating
            // the screen and reading as noise.
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(12.dp),
            ) {
                // Host only. Everything behind it acts on other people in the
                // game - kicking, banning, muting - and a joined client has no
                // authority to do any of it, so it gets no button rather than
                // one that quietly does nothing.
                if (state.isHost) {
                    HudIconButton(Icons.Filled.AdminPanelSettings, "Admin", onAdmin)
                }
                HudIconButton(Icons.Filled.Search, "Find LAN games", onFindGames)
                // Outermost, in the corner: the camera toggle is the one control
                // here reached mid-aim, so it gets the position the thumb finds
                // without looking. The icon carries its own state (globe =
                // free-fly over the whole map, focus reticle = locked to your
                // tank), so it needs no caption.
                //
                // The tutorial button that used to sit here is gone: its content
                // described the drag-to-fire gesture and the old plain-view
                // controls, none of which exist any more, so it was actively
                // misleading. Worth rewriting once the controls settle rather
                // than keeping a stale one on screen.
                HudIconButton(
                    icon = Icons.AutoMirrored.Filled.Chat,
                    description = if (state.chatComposing) {
                        "Close the message box (hold for scores and chat history)"
                    } else {
                        "Send a message (hold for scores and chat history)"
                    },
                    // A toggle, not a one-way open: the button is the obvious
                    // thing to press to get rid of the box again, and the
                    // keyboard covers the Close link when it is up.
                    onClick = { state.chatComposing = !state.chatComposing },
                    onLongClick = onScores,
                )
                HudIconButton(
                    icon = if (state.cameraFollow) Icons.Filled.CenterFocusStrong else Icons.Filled.Public,
                    description = if (state.cameraFollow) {
                        "Camera: following your tank (hold for more views)"
                    } else {
                        "Camera: free-fly (hold for more views)"
                    },
                    onClick = onToggleCamera,
                    onLongClick = onCameraPresets,
                )
            }
        }

        // Chat, directly under those icons. Transient by design: it is a
        // glance, not a log - the full history is in the score dialog's
        // sibling, and anything important repeats.
        ChatOverlay(
            state = state,
            onSend = onSendChat,
            modifier = Modifier
                .align(Alignment.TopEnd)
                .windowInsetsPadding(WindowInsets.displayCutout)
                .padding(horizontal = 12.dp)
                // Clear of the icon row above.
                .padding(top = 72.dp),
        )

        // Everything else lives in a stacked strip along the bottom edge.
        // Portrait has scarce width but plenty of height, so stacking two
        // shallow rows here buys room for every common control as its own
        // button - rather than cramming one row and pushing things into
        // menus - while still leaving the whole middle of the screen clear
        // for the battlefield. Order within each row groups by purpose.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                // Keep the strip clear of the gesture-navigation area -
                // system bars are hidden, but a swipe from the very bottom
                // edge still belongs to the system, so buttons sitting in
                // that band would fight it.
                .windowInsetsPadding(WindowInsets.navigationBars)
                // M11: the whole bottom control strip fades together, so a
                // player who wants more battlefield gets it without losing
                // any control.
                .alpha(state.controlOpacity)
                .padding(bottom = 10.dp),
        ) {
            // Row 1 is one button that both picks the weapon and fires it:
            // a tap queues the shot, a hold opens the weapon list.
            //
            // One hit target, which is dan's call after seeing the two-target
            // version. It puts the irreversible action on the *short* press,
            // so the tradeoff is worth stating: a stray tap commits the turn.
            // What makes that liveable is that the label always names the
            // weapon that tap will send, and that resting a thumb produces a
            // hold, which only opens a list.
            //
            // Full width, so the one control anyone reaches for without
            // looking is the whole bottom of the screen and its centre never
            // moves - it used to slide sideways with the length of the
            // weapon's name, which is what made it feel off to the side.
            FirePill(
                state = state,
                onFire = onFire,
                onWeapon = onWeapon,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp)
                    .height(48.dp),
            )

            Spacer(Modifier.height(6.dp))

            // Row 2: the overflow sits in the middle of the pack, with a
            // gap either side of it - undo and skip on the left, defences and
            // the shop on the right - so the two groups read as groups and
            // the thing that opens a menu is not mistaken for one of them.
            //
            // A Box rather than a Row so the cluster stays centred whatever
            // else appears: "Done buying" is pinned to the far right and
            // therefore cannot shove the other five sideways when the buying
            // phase starts and ends. Same reason the fire button above has a
            // fixed centre.
            Box(modifier = Modifier.fillMaxWidth()) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.align(Alignment.Center),
                ) {
                    HudIconButton(
                        icon = Icons.Filled.Undo,
                        description = "Revert to last angles (hold to quit to menu)",
                        onClick = onUndo,
                        onLongClick = onQuitToMenu,
                    )
                    HudIconButton(
                        icon = Icons.Filled.SkipNext,
                        description = "Skip turn (hold for game speed)",
                        onClick = onSkip,
                        onLongClick = onSimulationSpeed,
                    )
                    Spacer(Modifier.width(10.dp))
                    HudIconButton(Icons.Filled.MoreVert, "More actions", onActions)
                    Spacer(Modifier.width(10.dp))
                    HudIconButton(Icons.Filled.Shield, "Defenses", onDefenses)
                    HudIconButton(Icons.Filled.ShoppingCart, "Shop", onShop)
                }
                // Only during the buying phase - the one time it does
                // anything - and out on its own, away from the controls that
                // are always there.
                if (state.buyingPhase) {
                    HudIconButton(
                        icon = Icons.Filled.DoneAll,
                        description = "Done buying",
                        onClick = onDoneBuying,
                        modifier = Modifier.align(Alignment.CenterEnd),
                    )
                }
            }
        }

        // M4: touch-controllable elevation. The top-down 2D renderer has no
        // on-screen vertical axis to repurpose the way the fire-drag
        // gesture repurposes screen X/Y for azimuth+power (see
        // MainActivity.setUpTouchToFire), so elevation gets its own
        // control. A rotated Material3 Slider was tried first (the same
        // trick the previous XML SeekBar used, since Compose has no
        // built-in vertical slider) but its hit target stayed pinned to
        // the Slider's pre-rotation touch height (48dp) - a small, awkward
        // target regardless of how big the surrounding Box was made. This
        // is a purpose-built drag control instead (see AxisSlider below) -
        // the entire visible track is the live touch/drag area.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            // M11: left-hand mode swaps which edge each slider sits on.
            // Elevation is the one adjusted most while aiming, so it belongs
            // under the thumb doing the work; power is the coarser control.
            modifier = if (state.leftHandMode) {
                Modifier.align(Alignment.CenterEnd).padding(end = 4.dp)
            } else {
                Modifier.align(Alignment.CenterStart).padding(start = 4.dp)
            }.alpha(state.controlOpacity),
        ) {
            FadingReadout("${state.elevationDegrees.toInt()}°", state.elevationDegrees)
            // Up increases, down decreases - the buttons bracket a vertical
            // track, so they follow the track's own direction rather than
            // the left/right convention the angle dial uses.
            NudgeButton("+") { onElevationChange((state.elevationDegrees + it).coerceIn(0f, 90f)) }
            AxisSlider(
                value = state.elevationDegrees,
                valueRange = 0f..90f,
                orientation = SliderOrientation.Vertical,
                onValueChange = onElevationChange,
                modifier = Modifier.size(width = 56.dp, height = 160.dp),
                onDragActive = { active -> onAimGesture(AimAxis.ELEVATION, active) },
            )
            NudgeButton("−") { onElevationChange((state.elevationDegrees - it).coerceIn(0f, 90f)) }
        }

        // Power - mirrors the elevation slider on the opposite edge.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = if (state.leftHandMode) {
                Modifier.align(Alignment.CenterStart).padding(start = 4.dp)
            } else {
                Modifier.align(Alignment.CenterEnd).padding(end = 4.dp)
            }.alpha(state.controlOpacity),
        ) {
            FadingReadout("${(state.powerFraction * 100).toInt()}%", state.powerFraction)
            // A step here is one percentage point, so the button matches
            // what the readout shows rather than nudging by a raw 1.0.
            NudgeButton("+") { onPowerChange((state.powerFraction + it / 100f).coerceIn(0f, 1f)) }
            AxisSlider(
                value = state.powerFraction,
                valueRange = 0f..1f,
                orientation = SliderOrientation.Vertical,
                onValueChange = onPowerChange,
                modifier = Modifier.size(width = 56.dp, height = 160.dp),
                onDragActive = { active -> onAimGesture(AimAxis.POWER, active) },
            )
            NudgeButton("−") { onPowerChange((state.powerFraction - it / 100f).coerceIn(0f, 1f)) }
        }

        // Angle - horizontal, sitting just above the two-row control strip
        // below it (hence the larger bottom offset than the other sliders).
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            // Measured from the same baseline as the strip it has to clear,
            // which is the navigation bar and not the screen edge. Without
            // that inset this offset was relative to the screen while the
            // strip was relative to the bar, so on a device with three-button
            // navigation - 48dp of it - the strip sat that much higher and
            // the slider landed on top of the fire button. Gesture
            // navigation insets almost nothing, which is why the phones this
            // is usually tested on never showed it.
            //
            // 138 is the two rows plus their padding. It still has to track
            // the strip's height by hand, and nothing enforces that: it went
            // up by 8 when the fire pill became 48dp tall.
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(bottom = 138.dp),
        ) {
            FadingReadout("${state.angleDegrees.toInt()}°", state.angleDegrees)
            // Nudge buttons flank the slider. 220dp of track covering 360
            // degrees is about 1.6 degrees per dp, so a single degree is
            // less than a pixel of travel - unhittable by dragging, however
            // steady your thumb. These give exact single-degree steps for
            // the final adjustment while the slider still does the coarse
            // sweep.
            Row(verticalAlignment = Alignment.CenterVertically) {
                NudgeButton("−") { onAngleChange(wrapDegrees(state.angleDegrees - it)) }
                AxisSlider(
                    value = state.angleDegrees,
                    valueRange = 0f..360f,
                    orientation = SliderOrientation.Horizontal,
                    onValueChange = onAngleChange,
                    modifier = Modifier.size(width = 220.dp, height = 40.dp),
                    // A compass has no ends: dragging off either side keeps
                    // turning the turret and the value wraps, so the whole 360
                    // is reachable in one continuous swipe instead of having to
                    // lift off and restart from the far side of the track.
                    wrapAround = true,
                    onDragActive = { active -> onAimGesture(AimAxis.ANGLE, active) },
                )
                NudgeButton("+") { onAngleChange(wrapDegrees(state.angleDegrees + it)) }
            }
        }

        // Name plates and health bars, positioned from the renderer's own
        // projection. Drawn before the dialog host so a modal covers them.
        TankPlates(state.tankOverlays, state.showNamePlates, state.showHealthBars)
        FloatingLabels(state.floatingLabels)

        HudDialogHost(state.dialog)
    }
}

private enum class SliderOrientation { Vertical, Horizontal }

/**
 * A purpose-built drag/tap slider along one axis - see the elevation-slider
 * doc comment in [GameHud] above for why this exists instead of a rotated
 * Material3 `Slider` (its touch target was too small and didn't scale with
 * the surrounding layout). Used for elevation and power (vertical) and
 * angle (horizontal).
 */
private fun wrapDegrees(degrees: Float): Float = ((degrees % 360f) + 360f) % 360f

/**
 * A small step-by-one control beside a slider. Tap for a single step; hold
 * to repeat, accelerating, so a long correction doesn't need dozens of taps
 * but a short one stays exact.
 *
 * Deliberately understated: it sits over the battlefield, and the slider
 * next to it is the primary control. [onNudge] is handed the step count
 * rather than a value, so wrapping and clamping stay with the caller that
 * knows which axis this is - the button drives a compass, an elevation and
 * a power bar without knowing the difference.
 */

/**
 * Name plates over the battlefield - this port's answer to upstream's
 * TargetRendererImplTank::drawNames/drawLife, which draw a name billboard
 * and life bars in world space using its own GL font atlas.
 *
 * Rendered as Compose instead: there is no font renderer here and the whole
 * UI layer is Compose by design, so the renderer hands over projected screen
 * positions and the text is ordinary Android text - which also means it
 * stays legible at any distance rather than shrinking into the terrain.
 *
 * A destroyed tank has no plate at all - the renderer stops publishing an
 * overlay for it, matching upstream's getVisible() guard (see the overlay
 * loop in renderer_jni.cpp). What arrives here with alive=false is a tank
 * that is alive but in the buying phase: upstream draws its name and no
 * life bar, so that is what this does.
 */
@Composable
private fun TankPlates(overlays: List<TankOverlay>, showNames: Boolean, showHealth: Boolean) {
    if (overlays.isEmpty() || (!showNames && !showHealth)) return
    val density = LocalDensity.current

    Box(modifier = Modifier.fillMaxSize()) {
        for (overlay in overlays) {
            if (!overlay.onScreen) continue
            val xDp = with(density) { overlay.screenX.toDp() }
            val yDp = with(density) { overlay.screenY.toDp() }

            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier
                    // Centre the plate on the tank and sit it just above.
                    .offset(x = xDp - 60.dp, y = yDp - 28.dp)
                    .width(120.dp),
            ) {
                if (showNames) {
                    Text(
                        text = overlay.name,
                        // Full colour either way: the only non-sNormal tank
                        // that reaches here is one still buying, and upstream
                        // draws its name in the player's own colour like any
                        // other.
                        color = overlay.color,
                        style = MaterialTheme.typography.labelMedium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                // Bars only while alive - a destroyed tank has no health to
                // report, and upstream likewise draws life only for a
                // playing tank.
                if (overlay.alive && showHealth) {
                    Spacer(Modifier.height(2.dp))
                    StatBar(overlay.life, Color(0xFF4CAF50))
                    // Second bar only when a shield is actually up, matching
                    // upstream's own "zero unless raised" behaviour.
                    if (overlay.shield > 0f) {
                        Spacer(Modifier.height(2.dp))
                        StatBar(overlay.shield, Color(0xFF4FC3F7))
                    }
                }
            }
        }
    }
}

/** One thin filled bar, dark behind so it reads against any terrain. */
@Composable
private fun StatBar(fraction: Float, color: Color) {
    Box(
        modifier = Modifier
            .width(52.dp)
            .height(4.dp)
            .background(Color.Black.copy(alpha = 0.5f), RoundedCornerShape(2.dp)),
    ) {
        Box(
            modifier = Modifier
                .fillMaxHeight()
                .width(52.dp * fraction.coerceIn(0f, 1f))
                .background(color, RoundedCornerShape(2.dp)),
        )
    }
}

@Composable
private fun NudgeButton(label: String, onNudge: (Float) -> Unit) {
    var pressed by remember { mutableStateOf(false) }
    val nudge by rememberUpdatedState(onNudge)

    // The repeat only - the first step is fired synchronously on press
    // below. Doing it here instead raced: a quick tap can flip `pressed`
    // true and false within a single composition, and LaunchedEffect keyed
    // on it then never runs the true branch at all, so short taps silently
    // did nothing.
    LaunchedEffect(pressed) {
        if (!pressed) return@LaunchedEffect
        delay(400)  // hold threshold, so a tap is exactly one step
        var interval = 140L
        while (pressed) {
            nudge(1f)
            delay(interval)
            // Ease into a faster repeat, floored so it stays controllable.
            interval = (interval * 4 / 5).coerceAtLeast(30L)
        }
    }

    Box(
        contentAlignment = Alignment.Center,
        modifier = Modifier
            .size(30.dp)
            .background(Color.White.copy(alpha = 0.18f), CircleShape)
            .pointerInput(Unit) {
                detectTapGestures(
                    onPress = {
                        // Exactly one step per press, however brief.
                        nudge(1f)
                        pressed = true
                        tryAwaitRelease()
                        pressed = false
                    },
                )
            },
    ) {
        Text(
            text = label,
            color = Color.White.copy(alpha = 0.75f),
            style = MaterialTheme.typography.titleMedium,
        )
    }
}

@Composable
private fun AxisSlider(
    value: Float,
    valueRange: ClosedFloatingPointRange<Float>,
    orientation: SliderOrientation,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    wrapAround: Boolean = false,
    // Whether a drag is in progress, which is what the aiming sounds follow -
    // upstream starts and stops them on the key going down and up, and a drag
    // is the nearest thing this port has to a held key.
    onDragActive: (Boolean) -> Unit = {},
) {
    val isVertical = orientation == SliderOrientation.Vertical
    val span = valueRange.endInclusive - valueRange.start
    BoxWithConstraints(modifier = modifier) {
        val lengthPx = if (isVertical) constraints.maxHeight.toFloat() else constraints.maxWidth.toFloat()

        fun fractionFromOffset(pos: Float): Float =
            if (isVertical) (1f - (pos / lengthPx)).coerceIn(0f, 1f) else (pos / lengthPx).coerceIn(0f, 1f)

        fun applyOffset(pos: Float) {
            val fraction = fractionFromOffset(pos)
            onValueChange(valueRange.start + fraction * span)
        }

        // Where the finger went down, and what the value was there. A drag
        // is applied as a delta from this anchor rather than as an absolute
        // position, which is what lets a [wrapAround] slider keep turning
        // past either end: the finger runs off the track but the offset
        // keeps growing, and the value wraps instead of sticking at 0/360.
        //
        // Inside the track this is identical to absolute positioning (the
        // anchor value is itself the absolute value at the anchor point),
        // so nothing changes for the non-wrapping sliders.
        var anchorPos by remember { mutableFloatStateOf(0f) }
        var anchorValue by remember { mutableFloatStateOf(0f) }
        // Read through rememberUpdatedState so onDragStart always sees the
        // latest value without the gesture detector being keyed on it -
        // keying pointerInput on a value the drag itself changes would tear
        // down and restart the detector on every frame of the drag.
        val currentValue by rememberUpdatedState(value)

        fun applyDrag(pos: Float) {
            if (!wrapAround) {
                applyOffset(pos)
                return
            }
            val travelled = (pos - anchorPos) / lengthPx * span
            val raw = anchorValue + if (isVertical) -travelled else travelled
            onValueChange(((raw - valueRange.start) % span + span) % span + valueRange.start)
        }

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(Unit) {
                    detectTapGestures { offset -> applyOffset(if (isVertical) offset.y else offset.x) }
                }
                .pointerInput(wrapAround, lengthPx) {
                    detectDragGestures(
                        onDragStart = { offset ->
                            anchorPos = if (isVertical) offset.y else offset.x
                            // Anchor on the *current* value, not on where the
                            // finger landed, so grabbing the thumb anywhere
                            // nudges from where the turret already points
                            // rather than jumping first.
                            anchorValue = currentValue
                            onDragActive(true)
                        },
                        // Both ends, not just the clean one: a drag cancelled
                        // by another pointer or by the gesture being taken
                        // over would otherwise leave the servo running with
                        // nothing to stop it.
                        onDragEnd = { onDragActive(false) },
                        onDragCancel = { onDragActive(false) },
                    ) { change, _ ->
                        change.consume()
                        applyDrag(if (isVertical) change.position.y else change.position.x)
                    }
                },
        ) {
            // Track.
            Box(
                modifier = Modifier
                    .align(Alignment.Center)
                    .let { if (isVertical) it.width(6.dp).fillMaxHeight() else it.height(6.dp).fillMaxWidth() }
                    .background(Color.White.copy(alpha = 0.35f), RoundedCornerShape(3.dp)),
            )

            // Thumb - a generously sized circle so it's easy to grab, sized
            // independent of the (thin) track.
            val thumbSizeDp = 20.dp
            val thumbSizePx = with(LocalDensity.current) { thumbSizeDp.toPx() }
            val fraction = ((value - valueRange.start) / (valueRange.endInclusive - valueRange.start)).coerceIn(0f, 1f)
            val travelPx = lengthPx - thumbSizePx
            Box(
                modifier = Modifier
                    .align(if (isVertical) Alignment.TopCenter else Alignment.CenterStart)
                    .offset {
                        if (isVertical) {
                            IntOffset(0, ((1f - fraction) * travelPx).roundToInt())
                        } else {
                            IntOffset((fraction * travelPx).roundToInt(), 0)
                        }
                    }
                    .size(thumbSizeDp)
                    .background(Color(0xFFB39DDB), CircleShape),
            )
        }
    }
}

@Composable
private fun HudText(text: String, modifier: Modifier = Modifier) {
    Text(text, color = Color.White, style = MaterialTheme.typography.bodyMedium, modifier = modifier)
}

/**
 * A slider's numeric readout, shown while the value is being changed and
 * faded out a second after the last change. The numbers matter only while
 * you are adjusting - the rest of the time they are three more things
 * competing with the battlefield for attention.
 *
 * Faded via alpha rather than removed from the layout, so the slider
 * underneath doesn't shift up and down as the label comes and goes.
 */
@Composable
private fun FadingReadout(text: String, value: Float) {
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(value) {
        visible = true
        delay(1000)
        visible = false
    }
    val alpha by animateFloatAsState(
        targetValue = if (visible) 1f else 0f,
        animationSpec = tween(durationMillis = if (visible) 0 else 400),
        label = "readout",
    )
    HudText(text, modifier = Modifier.alpha(alpha))
}

/**
 * The fire button, which is also the weapon button: a tap queues the shot, a
 * hold opens the weapon list.
 *
 * A Surface with combinedClickable rather than a Button, because Material's
 * Button takes only an onClick - the same reason HudIconButton's long-press
 * variant is built this way.
 *
 * The icon is repeated at both ends. The middle is the weapon's name, so
 * without them nothing on the control says what pressing it does: reticles
 * while it can fire, hourglasses once the shot is in, and a tap-target while
 * a position weapon wants the map instead of this button.
 */
@Composable
private fun FirePill(
    state: GameHudState,
    onFire: () -> Unit,
    onWeapon: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val icon = when {
        state.positionSelectWeapon.isNotEmpty() -> Icons.Filled.TouchApp
        state.shotLocked -> Icons.Filled.HourglassTop
        else -> Icons.Filled.GpsFixed
    }
    // Red once the shot is committed, back to normal when the round resolves
    // and a new move is granted. Never colour alone: the icons at both ends
    // change with it, so it still reads for anyone who cannot tell the two
    // colours apart.
    Surface(
        color = if (state.shotLocked) {
            Color(0xFFC62828)
        } else {
            MaterialTheme.colorScheme.primary
        },
        contentColor = if (state.shotLocked) {
            Color.White
        } else {
            MaterialTheme.colorScheme.onPrimary
        },
        shape = RoundedCornerShape(24.dp),
        modifier = modifier.combinedClickable(
            onClick = onFire,
            onLongClick = onWeapon,
            role = Role.Button,
            onClickLabel = if (state.shotLocked) "Shot already sent" else "Fire",
            onLongClickLabel = "Change weapon",
        ),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.padding(horizontal = 16.dp),
        ) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
            Text(
                state.weaponLabel,
                maxLines = 1,
                textAlign = TextAlign.Center,
                modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            )
            Icon(icon, contentDescription = null, modifier = Modifier.size(20.dp))
        }
    }
}

/**
 * A compact icon-only HUD button. Filled-tonal rather than a plain
 * IconButton so it still reads as a control against an arbitrary 3D scene
 * behind it, and small enough that several can sit in a tight row without
 * the word-label sprawl the HUD had before.
 */
@Composable
private fun HudIconButton(
    icon: ImageVector,
    description: String,
    onClick: () -> Unit,
    onLongClick: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    // A long press opens the fuller version of whatever the button does -
    // the message button's chat history, the camera button's preset list.
    // Those belong on the icon they extend rather than buried in the
    // overflow menu, and a long press costs the tap nothing.
    if (onLongClick == null) {
        FilledTonalIconButton(
            onClick = onClick,
            modifier = modifier.padding(horizontal = 2.dp).size(44.dp),
        ) {
            Icon(icon, contentDescription = description, modifier = Modifier.size(22.dp))
        }
        return
    }

    // FilledTonalIconButton takes only an onClick, so the long-press variant
    // is the same surface with a combinedClickable of its own.
    Surface(
        color = MaterialTheme.colorScheme.secondaryContainer,
        contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
        shape = CircleShape,
        modifier = modifier
            .padding(horizontal = 2.dp)
            .size(44.dp)
            .combinedClickable(
                onClick = onClick,
                onLongClick = onLongClick,
                role = Role.Button,
            ),
    ) {
        Box(contentAlignment = Alignment.Center) {
            Icon(icon, contentDescription = description, modifier = Modifier.size(22.dp))
        }
    }
}

/**
 * M6 parity: the in-game chat overlay - a transient stack of recent messages
 * plus the inline compose box.
 *
 * Deliberately not a modal dialog. Chat happens *during* the round, often
 * while watching a shot land, so covering the battlefield to type would make
 * it useless for the one thing it is for. The compose box is a single row
 * pinned under the message stack; the system keyboard takes the bottom of the
 * screen and the game stays visible above it.
 *
 * Each message carries its own arrival time and expires on its own timer, so
 * a burst does not wipe a line halfway through being read; new ones push in
 * at the top and the rest slide down.
 */
@Composable
private fun ChatOverlay(
    state: GameHudState,
    onSend: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    // Drop expired messages. Ticking here rather than filtering at the poll
    // site keeps the expiry independent of whether any new chat is arriving -
    // the last message of a conversation has to time out too.
    LaunchedEffect(state.chatToasts.size, state.chatToasts.firstOrNull()?.line?.id) {
        while (state.chatToasts.isNotEmpty()) {
            delay(250)
            val now = System.currentTimeMillis()
            state.chatToasts = state.chatToasts.filter {
                now - it.shownAtMillis < state.chatToastMillis
            }
        }
    }

    Column(
        horizontalAlignment = Alignment.End,
        verticalArrangement = Arrangement.spacedBy(4.dp),
        modifier = modifier.widthIn(max = 260.dp),
    ) {
        // Newest first, so the eye lands on the latest without hunting.
        state.chatToasts.sortedByDescending { it.line.id }.forEach { toast ->
            ChatToastRow(toast, state.chatToastMillis)
        }

        if (state.chatComposing) {
            ChatComposer(
                channel = state.chatChannel,
                onChannelChange = { state.chatChannel = it },
                onSend = { text ->
                    onSend(text)
                    state.chatComposing = false
                },
                onDismiss = { state.chatComposing = false },
            )
        }
    }
}

@Composable
private fun ChatToastRow(toast: ChatToast, toastMillis: Long) {
    // Fade the last second rather than vanishing, so a message leaving does
    // not read as a glitch.
    var visible by remember(toast.line.id) { mutableStateOf(true) }
    val alpha by animateFloatAsState(if (visible) 1f else 0f, label = "chatFade")
    LaunchedEffect(toast.line.id) {
        delay(toastMillis - 800)
        visible = false
    }

    Surface(
        color = Color.Black.copy(alpha = 0.55f * alpha),
        shape = MaterialTheme.shapes.small,
    ) {
        Row(modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp)) {
            if (toast.line.who.isNotEmpty()) {
                Text(
                    text = "${toast.line.who}: ",
                    style = MaterialTheme.typography.bodySmall,
                    fontWeight = FontWeight.Bold,
                    color = channelColor(toast.line.channel).copy(alpha = alpha),
                )
            }
            Text(
                text = toast.line.text,
                style = MaterialTheme.typography.bodySmall,
                color = Color.White.copy(alpha = alpha),
            )
        }
    }
}

/**
 * Upstream colours its channels so the game's own commentary reads
 * differently from a player talking. The exact palette is ours - upstream's
 * lives in its GLW widget set - but the distinction is the point.
 */
private fun channelColor(channel: String): Color = when (channel) {
    "team" -> Color(0xFF7FD8FF)
    "info", "announce", "banner" -> Color(0xFFFFD37F)
    "combat" -> Color(0xFFFF9E80)
    else -> Color(0xFFB6F7A8)
}

@Composable
private fun ChatComposer(
    channel: String,
    onChannelChange: (String) -> Unit,
    onSend: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var text by remember { mutableStateOf("") }
    val focusRequester = remember { FocusRequester() }
    // Open the keyboard as soon as the box appears - the tap on the chat
    // icon was already the "I want to type" gesture, so asking for a second
    // one would be busywork.
    LaunchedEffect(Unit) { focusRequester.requestFocus() }

    Surface(
        color = Color.Black.copy(alpha = 0.75f),
        shape = MaterialTheme.shapes.small,
    ) {
        Column(modifier = Modifier.padding(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                // Channel picker: two options, so a toggle rather than a menu.
                listOf("general", "team").forEach { option ->
                    Text(
                        text = option,
                        style = MaterialTheme.typography.labelSmall,
                        color = if (channel == option) channelColor(option) else Color.White.copy(alpha = 0.5f),
                        modifier = Modifier
                            .clickable { onChannelChange(option) }
                            .padding(horizontal = 6.dp, vertical = 2.dp),
                    )
                }
                Spacer(Modifier.weight(1f))
                Text(
                    text = "Close",
                    style = MaterialTheme.typography.labelSmall,
                    color = Color.White.copy(alpha = 0.7f),
                    modifier = Modifier
                        .clickable(onClick = onDismiss)
                        .padding(horizontal = 6.dp, vertical = 2.dp),
                )
            }
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                singleLine = true,
                textStyle = MaterialTheme.typography.bodySmall,
                placeholder = {
                    Text(
                        "Message",
                        style = MaterialTheme.typography.bodySmall,
                        color = Color.White.copy(alpha = 0.5f),
                    )
                },
                // Explicit colours: the field sits on a near-black surface,
                // and the theme's defaults are dark-on-light, which left the
                // text all but invisible against it.
                colors = OutlinedTextFieldDefaults.colors(
                    focusedTextColor = Color.White,
                    unfocusedTextColor = Color.White,
                    cursorColor = Color.White,
                    focusedBorderColor = Color.White.copy(alpha = 0.7f),
                    unfocusedBorderColor = Color.White.copy(alpha = 0.4f),
                    focusedContainerColor = Color.Transparent,
                    unfocusedContainerColor = Color.Transparent,
                ),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = {
                    if (text.isNotBlank()) onSend(text.trim())
                    text = ""
                }),
                modifier = Modifier
                    .fillMaxWidth()
                    .focusRequester(focusRequester),
            )
        }
    }
}


/**
 * M6: upstream's floating damage numbers and speech bubbles. The renderer
 * projects them; they are drawn here because it has no font, which is the
 * same reason the tank name plates live in Compose.
 */
@Composable
private fun FloatingLabels(labels: List<FloatingLabel>) {
    val density = LocalDensity.current
    labels.forEach { label ->
        if (!label.onScreen) return@forEach
        Text(
            text = label.text,
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
            color = label.color.copy(alpha = label.fade.coerceIn(0f, 1f)),
            modifier = Modifier.offset(
                x = with(density) { label.screenX.toDp() } - 16.dp,
                y = with(density) { label.screenY.toDp() } - 10.dp,
            ),
        )
    }
}
