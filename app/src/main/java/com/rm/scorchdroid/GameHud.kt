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
import androidx.compose.material.icons.filled.CenterFocusStrong
import androidx.compose.material.icons.filled.DoneAll
import androidx.compose.material.icons.filled.GpsFixed
import androidx.compose.material.icons.filled.HourglassTop
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Public
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.ShoppingCart
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.Undo
import androidx.compose.material.icons.filled.Whatshot
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
    // M6 parity: current wind (speed + direction) - it really does perturb
    // shots, and nothing showed it before. "" while there's no game yet.
    var windLabel by mutableStateOf("")
    // M4 dialog conversion (see HudDialogs.kt) - the currently-shown modal,
    // if any. A plain mutable field like the rest of this state holder,
    // since it's written from ordinary (non-Composable) Kotlin in
    // MainActivity.
    var dialog: HudDialog by mutableStateOf(HudDialog.None)
}

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
    onSkip: () -> Unit,
    onDoneBuying: () -> Unit,
) {
    Box(modifier = Modifier.fillMaxSize()) {
        // Top-left: status line (phase/turn/aim feedback - see
        // NativeBridge.getMyStatusLabel()) plus the hosting address once
        // bound. windowInsetsPadding(displayCutout) keeps this clear of a
        // camera cutout - previously handled by hand in MainActivity,
        // Compose does it natively.
        Column(
            modifier = Modifier
                .align(Alignment.TopStart)
                .windowInsetsPadding(WindowInsets.displayCutout)
                .padding(16.dp),
        ) {
            if (state.statusText.isNotEmpty()) HudText(state.statusText)
            if (state.hostingLabel.isNotEmpty()) HudText(state.hostingLabel)
            if (state.windLabel.isNotEmpty()) HudText(state.windLabel)
        }

        // Top-right: "session / view" controls - things about this session
        // or how you're looking at it, not about the fight itself. Icon-only
        // and tightly stacked; a wall of word-labelled buttons was eating
        // the screen and reading as noise.
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .align(Alignment.TopEnd)
                .windowInsetsPadding(WindowInsets.displayCutout)
                .padding(12.dp),
        ) {
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
                icon = if (state.cameraFollow) Icons.Filled.CenterFocusStrong else Icons.Filled.Public,
                description = if (state.cameraFollow) "Camera: following your tank" else "Camera: free-fly",
                onClick = onToggleCamera,
            )
        }

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
                .padding(bottom = 10.dp),
        ) {
            // Row 1: aim + fire. The weapon button keeps its text because,
            // unlike the icon buttons, it isn't a label for a function -
            // it's live data (weapon name + remaining ammo) read before
            // firing. "Done buying" only exists during the buying phase,
            // which is the one time it's ever useful.
            Row(verticalAlignment = Alignment.CenterVertically) {
                Button(onClick = onWeapon, contentPadding = PaddingValues(horizontal = 12.dp)) {
                    Icon(Icons.Filled.Whatshot, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(state.weaponLabel, maxLines = 1)
                }
                Spacer(Modifier.width(8.dp))
                // Red once the shot is committed, back to normal when the
                // round resolves and a new move is granted. The label
                // changes with it - colour alone would leave anyone who
                // can't distinguish it with no feedback at all.
                Button(
                    onClick = onFire,
                    contentPadding = PaddingValues(horizontal = 16.dp),
                    colors = if (state.shotLocked) {
                        ButtonDefaults.buttonColors(
                            containerColor = Color(0xFFC62828),
                            contentColor = Color.White,
                        )
                    } else {
                        ButtonDefaults.buttonColors()
                    },
                ) {
                    Icon(
                        if (state.shotLocked) Icons.Filled.HourglassTop else Icons.Filled.GpsFixed,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp),
                    )
                    Spacer(Modifier.width(6.dp))
                    Text(if (state.shotLocked) "LOCKED" else "FIRE")
                }
                if (state.buyingPhase) {
                    Spacer(Modifier.width(8.dp))
                    HudIconButton(Icons.Filled.DoneAll, "Done buying", onDoneBuying)
                }
            }

            Spacer(Modifier.height(6.dp))

            // Row 2: turn actions then loadout. Undo and skip are common
            // enough (fire, nudge, fire again; pass when you can't reach
            // anyone) that they get their own buttons rather than hiding
            // in a menu - only genuinely rare things like resigning sit
            // behind the overflow.
            Row(verticalAlignment = Alignment.CenterVertically) {
                HudIconButton(Icons.Filled.Undo, "Revert to last angles", onUndo)
                HudIconButton(Icons.Filled.SkipNext, "Skip turn", onSkip)
                Spacer(Modifier.width(10.dp))
                HudIconButton(Icons.Filled.Shield, "Defenses", onDefenses)
                HudIconButton(Icons.Filled.ShoppingCart, "Shop", onShop)
                HudIconButton(Icons.Filled.MoreVert, "More actions", onActions)
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
            modifier = Modifier.align(Alignment.CenterStart).padding(start = 4.dp),
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
            )
            NudgeButton("−") { onElevationChange((state.elevationDegrees - it).coerceIn(0f, 90f)) }
        }

        // Power - mirrors the elevation slider on the opposite edge.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.align(Alignment.CenterEnd).padding(end = 4.dp),
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
            )
            NudgeButton("−") { onPowerChange((state.powerFraction - it / 100f).coerceIn(0f, 1f)) }
        }

        // Angle - horizontal, sitting just above the two-row control strip
        // below it (hence the larger bottom offset than the other sliders).
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 130.dp),
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
                )
                NudgeButton("+") { onAngleChange(wrapDegrees(state.angleDegrees + it)) }
            }
        }

        // Name plates and health bars, positioned from the renderer's own
        // projection. Drawn before the dialog host so a modal covers them.
        TankPlates(state.tankOverlays)

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
 * A dead tank keeps its plate. Upstream does the same (drawParticle falls
 * through to the arrow and names for a non-normal tank), and it is the only
 * thing marking where a destroyed player was now that the model is hidden.
 */
@Composable
private fun TankPlates(overlays: List<TankOverlay>) {
    if (overlays.isEmpty()) return
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
                Text(
                    text = overlay.name,
                    color = if (overlay.alive) overlay.color else overlay.color.copy(alpha = 0.55f),
                    style = MaterialTheme.typography.labelMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                // Bars only while alive - a destroyed tank has no health to
                // report, and upstream likewise draws life only for a
                // playing tank.
                if (overlay.alive) {
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
                        },
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
) {
    FilledTonalIconButton(
        onClick = onClick,
        modifier = Modifier.padding(horizontal = 2.dp).size(44.dp),
    ) {
        Icon(icon, contentDescription = description, modifier = Modifier.size(22.dp))
    }
}
