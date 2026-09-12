package com.rm.scorchdroid

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/**
 * M12: the tutorial's coach marks.
 *
 * Written rather than ported, and not for want of trying. Upstream ships
 * `data/tutorial.xml`, but its runner (`client/graph/TutorialFile`,
 * `client/dialogs/TutorialDialog`) is in the excluded client layer, and its
 * steps advance on conditions naming upstream's own windows -
 * `<condition type="WindowVisible"><window>Rules</window></condition>` - which
 * do not exist in this port and never will. Its *game* configuration is
 * reusable and is reused; the words are not.
 *
 * The steps below deliberately teach only what a player cannot discover by
 * pressing things: which slider is which, that holding the fire bar picks the
 * weapon, that wind moves the shot, and that targets are what the practice
 * landscape is full of. Everything else - the shop, the camera, the menus -
 * is left to be found, because a tutorial that explains the whole interface
 * is one people skip.
 *
 * The hold is the clearest case for any of this existing: a long press
 * announces itself to nobody, and it is the only way to change weapon
 * without going through the shop.
 */
data class TutorialStep(
    val text: String,
    /**
     * When non-null, the step also clears itself once this becomes true, so a
     * player who has already worked it out is not made to tap through. The
     * step still has a Got it button; this is in addition.
     */
    val advanceWhen: ((GameHudState) -> Boolean)? = null,
)

val TUTORIAL_STEPS: List<TutorialStep> = listOf(
    TutorialStep(
        "That red tank in the middle is yours. Drag anywhere on the battlefield to " +
            "swing the camera around it, and pinch to zoom."
    ),
    TutorialStep(
        // Named by neither edge nor readout, and both exclusions were learned
        // the hard way. Not by edge, because left-hand mode swaps the two and
        // a tutorial that says "left" to a player who has swapped them is
        // worse than saying nothing. Not by readout either, though that was
        // the first fix attempted: FadingReadout only shows a value *while*
        // it changes, so at the moment this card is read neither slider reads
        // anything - and two of the three sliders read degrees anyway (the
        // turn slider in the next step is the other). So it says how to tell
        // them apart instead: drag one and watch. That costs nothing, commits
        // nothing, and the barrel moving is its own label.
        "A slider sits at each edge. Drag either one to see what it does - one tilts " +
            "the barrel up and down, the other sets how hard you fire."
    ),
    TutorialStep(
        // "The bar along the bottom" used to be unambiguous. There are two
        // down there now, so each is named by what it does.
        "The slider just above the buttons turns the tank to face left and right.\n\n" +
            "Set an angle you like, then tap the wide bar below it to fire.",
        // The shot itself is the lesson; no need to make them tap Got it too.
        advanceWhen = { it.shotLocked },
    ),
    TutorialStep(
        // The one genuinely hidden control in the game, and therefore the one
        // this tutorial most has to exist for - a long press announces itself
        // to nobody. Taught after the first shot rather than before it, so it
        // is read while watching that shot land.
        "That bar always names the weapon it will send. Hold it to pick a different " +
            "one.\n\nIt turns red while your shot is queued, and goes back to normal when " +
            "the round plays out."
    ),
    TutorialStep(
        // Deliberately says this practice game has *no* wind. Upstream's
        // tutorial config sets WindForce to WindNone, and the status line
        // reads "Wind: none" - so promising that wind pushes the shot here
        // would be contradicted on screen by the line above the card.
        "Watch where it lands, then adjust and fire again. Ranging in like this is the " +
            "whole game.\n\nThere is no wind in this practice game, so your shots go " +
            "where you point them. In a real one the wind at the top left pushes every " +
            "shot, and you have to allow for it."
    ),
    TutorialStep(
        "The other tanks are practice targets - they sit still and never fire back. " +
            "There is no clock here either, so take as long as you like.\n\nWhen you want " +
            "a real game, quit to the menu with a long press on the undo button."
    ),
)

/**
 * Tracks which step is showing. Kept out of GameHudState because it is a
 * property of one particular game rather than of the HUD, and because a
 * tutorial that survived into the next game would be a bug.
 */
class TutorialState {
    var stepIndex by mutableIntStateOf(0)
        private set
    var finished by mutableStateOf(false)
        private set

    val step: TutorialStep? get() = if (finished) null else TUTORIAL_STEPS.getOrNull(stepIndex)

    fun next() {
        if (stepIndex + 1 >= TUTORIAL_STEPS.size) finished = true else stepIndex++
    }

    fun skip() {
        finished = true
    }

    /** Called each tick; advances a step that has taught itself. */
    fun observe(hud: GameHudState) {
        val current = step ?: return
        val condition = current.advanceWhen ?: return
        if (condition(hud)) next()
    }
}

/**
 * The card itself. Sits at the top, under the status text, because the bottom
 * half of the screen is where every control the steps talk about lives - a
 * panel there would cover the thing it was describing.
 */
@Composable
fun TutorialOverlay(state: TutorialState, onSkip: () -> Unit) {
    val step = state.step ?: return
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.TopCenter) {
        Surface(
            shape = RoundedCornerShape(12.dp),
            color = Color(0xE6161E33),
            modifier = Modifier
                .widthIn(max = 420.dp)
                .fillMaxWidth()
                .padding(horizontal = 12.dp)
                .padding(top = 132.dp),
        ) {
            Column(modifier = Modifier.padding(16.dp)) {
                Text(
                    text = "Tutorial  ${state.stepIndex + 1}/${TUTORIAL_STEPS.size}",
                    color = Color(0xFFB39DFF),
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(6.dp))
                Text(
                    text = step.text,
                    color = Color.White,
                    style = MaterialTheme.typography.bodyMedium,
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    TextButton(onClick = onSkip) {
                        Text("Skip", color = Color.White.copy(alpha = 0.6f))
                    }
                    TextButton(onClick = { state.next() }) {
                        Text(
                            if (state.stepIndex + 1 >= TUTORIAL_STEPS.size) "Done" else "Got it",
                            color = Color(0xFFB39DFF),
                        )
                    }
                }
            }
        }
    }
}
