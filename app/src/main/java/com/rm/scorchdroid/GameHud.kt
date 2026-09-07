package com.rm.scorchdroid

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
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Public
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Shield
import androidx.compose.material.icons.filled.ShoppingCart
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.Undo
import androidx.compose.material.icons.filled.Whatshot
import androidx.compose.material3.Button
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

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
                Button(onClick = onFire, contentPadding = PaddingValues(horizontal = 16.dp)) {
                    Icon(Icons.Filled.GpsFixed, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("FIRE")
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
            HudText("${state.elevationDegrees.toInt()}°")
            AxisSlider(
                value = state.elevationDegrees,
                valueRange = 0f..90f,
                orientation = SliderOrientation.Vertical,
                onValueChange = onElevationChange,
                modifier = Modifier.size(width = 56.dp, height = 160.dp),
            )
        }

        // Power - mirrors the elevation slider on the opposite edge.
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.align(Alignment.CenterEnd).padding(end = 4.dp),
        ) {
            HudText("${(state.powerFraction * 100).toInt()}%")
            AxisSlider(
                value = state.powerFraction,
                valueRange = 0f..1f,
                orientation = SliderOrientation.Vertical,
                onValueChange = onPowerChange,
                modifier = Modifier.size(width = 56.dp, height = 160.dp),
            )
        }

        // Angle - horizontal, sitting just above the two-row control strip
        // below it (hence the larger bottom offset than the other sliders).
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 130.dp),
        ) {
            HudText("${state.angleDegrees.toInt()}°")
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
        }

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
private fun HudText(text: String) {
    Text(text, color = Color.White, style = MaterialTheme.typography.bodyMedium)
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
