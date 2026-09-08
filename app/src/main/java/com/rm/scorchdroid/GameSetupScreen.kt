package com.rm.scorchdroid

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt

private val SetupTop = Color(0xFF16213A)
private val SetupBottom = Color(0xFF2B1B3D)
private val SetupAccent = Color(0xFFB39DFF)

/**
 * M10: the screen shown before a game starts, for both New Game and Host Game.
 *
 * Every control here is drawn from what the engine reports about its own
 * option - the range on a slider, the choices in a row of chips, the text
 * under the label - rather than from anything hardcoded in the UI. That is the
 * point: these are upstream's game rules, so the screen shows what upstream
 * says they are and lets the player pick among them, and adding an option to
 * the exposed list in GameSetup.cpp makes it appear here with no work.
 */
@Composable
fun GameSetupScreen(
    title: String,
    options: List<SetupOption>,
    mods: List<String>,
    selectedMod: String,
    onModChange: (String) -> Unit,
    bots: List<BotOption>,
    selectedBot: String,
    onBotChange: (String) -> Unit,
    onChange: (SetupOption, String) -> Unit,
    onReset: () -> Unit,
    onStart: () -> Unit,
    onBack: () -> Unit,
) {
    // M18: the tabs come from the options themselves - each one says which
    // group it belongs to (see GameSetup.cpp's exposed list), in the order
    // they are declared. So adding an option there puts it on a tab here
    // with nothing to change in this file.
    val groups = options.map { it.group }.distinct()
    var tab by remember(groups) { mutableStateOf(groups.firstOrNull() ?: "") }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(SetupTop, SetupBottom))),
    ) {
        Column(
            modifier = Modifier
                // M11: capped and centred rather than filling the width. A
                // landscape phone is 2340px across, and a slider that wide has
                // absurd travel per step while a line of description becomes
                // hard to track back to its start.
                .widthIn(max = 560.dp)
                .fillMaxSize()
                .align(Alignment.TopCenter)
                .padding(horizontal = 24.dp, vertical = 28.dp),
        ) {
            // Back on the title row, like Settings and About: a page that
            // scrolls for several screens means scrolling back down to leave.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = title,
                    color = Color.White,
                    fontSize = 26.sp,
                    fontWeight = FontWeight.Bold,
                )
                TextButton(onClick = onBack) { Text("Back", color = SetupAccent) }
            }
            Spacer(Modifier.height(10.dp))

            // Start sits at the top, not at the foot. The defaults are a
            // playable game, so the common case is opening this screen and
            // wanting to play - and making that person hunt for the button
            // taxes them for a choice they did not want to make.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Button(
                    onClick = onStart,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = SetupAccent.copy(alpha = 0.25f),
                        contentColor = Color.White,
                    ),
                    modifier = Modifier.widthIn(min = 150.dp),
                ) {
                    Text("Start", style = MaterialTheme.typography.titleMedium)
                }
                TextButton(onClick = onReset) { Text("Reset to defaults", color = SetupAccent) }
            }
            Spacer(Modifier.height(6.dp))

            TabRow(
                selectedTabIndex = groups.indexOf(tab).coerceAtLeast(0),
                containerColor = Color.Transparent,
                contentColor = SetupAccent,
                divider = { HorizontalDivider(color = Color.White.copy(alpha = 0.12f)) },
            ) {
                groups.forEach { group ->
                    Tab(
                        selected = group == tab,
                        onClick = { tab = group },
                        selectedContentColor = SetupAccent,
                        unselectedContentColor = Color.White.copy(alpha = 0.55f),
                        // A shade smaller than the settings tabs, and never
                        // wrapped: four equal shares of a phone's width is
                        // not quite enough for "Weapons" at title size, and a
                        // tab label broken across two lines looks like a
                        // mistake.
                        text = {
                            Text(
                                group,
                                style = MaterialTheme.typography.labelLarge,
                                maxLines = 1,
                            )
                        },
                    )
                }
            }

            // Keyed on the tab so each starts at its own top.
            val scroll = remember(tab) { ScrollState(0) }
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(scroll)
                    .padding(top = 10.dp),
            ) {
                Text(
                    text = "These are Scorched3D's own settings, with its own limits.",
                    color = SetupAccent,
                    style = MaterialTheme.typography.bodySmall,
                )
                Spacer(Modifier.height(14.dp))

                // The mod decides what every other option means - which
                // weapons, tanks, landscapes and bots exist - so it leads the
                // first tab. Only when there is a choice to make: with just
                // upstream's base game installed a one-item picker is noise.
                if (tab == groups.firstOrNull() && mods.size > 1) {
                    ModRow(mods, selectedMod, onModChange)
                }

                options.filter { it.group == tab }.forEach { option ->
                    SetupRow(option, onChange)
                    HorizontalDivider(
                        color = Color.White.copy(alpha = 0.08f),
                        modifier = Modifier.padding(vertical = 10.dp),
                    )
                }

                // M18: who fills the other slots. Beside the player count,
                // because the two are one question - how many opponents, and
                // how good.
                if (tab == "Players" && bots.isNotEmpty()) {
                    BotRow(bots, selectedBot, onBotChange)
                }

                Spacer(Modifier.height(24.dp))
            }
        }
    }
}

/**
 * M18: which AI fills the slots that are not the player's.
 *
 * Upstream has no difficulty setting. It gives each of its twenty-four player
 * slots a PlayerType naming an AI, and the AIs themselves are the difficulty:
 * the base game ships Moron ("a very stupid computer controlled player") up
 * through Shooter, Tosser, Chooser and Shark to Cyborg ("a deadly computer
 * controlled player. Ouch!!"). Those descriptions are upstream's own and are
 * the only place it says how hard a bot is, so they are what this shows.
 *
 * Every slot gets the same choice here, which is what upstream's own
 * single-player files do - singleeasy is two Morons, singlenormal three
 * Choosers and a Random.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun BotRow(bots: List<BotOption>, selected: String, onChange: (String) -> Unit) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text("Bots", color = Color.White, style = MaterialTheme.typography.titleSmall)
        Text(
            text = bots.firstOrNull { it.name == selected }?.description
                ?: "Which computer player fills the other places",
            color = Color.White.copy(alpha = 0.55f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(6.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            bots.forEach { bot ->
                FilterChip(
                    selected = bot.name == selected,
                    onClick = { onChange(bot.name) },
                    label = { Text(bot.name, style = MaterialTheme.typography.bodySmall) },
                    colors = FilterChipDefaults.filterChipColors(
                        containerColor = Color.White.copy(alpha = 0.06f),
                        labelColor = Color.White.copy(alpha = 0.75f),
                        selectedContainerColor = SetupAccent.copy(alpha = 0.3f),
                        selectedLabelColor = Color.White,
                    ),
                )
            }
        }
    }
}

/**
 * Enums whose values form a scale rather than a set, and so read better as a
 * slider than as a row of chips.
 *
 * Upstream declares WindForce as Random, None, 1, 2, 3, 4, 5, Breezy, Gale and
 * WindType as Never, Sometimes, Frequently, Constantly, Always - both in
 * increasing order, so dragging right means more wind and more change. The
 * other enums are not like that: WallType's Concrete, Bouncy, Random and
 * TurnType's several turn orders are unordered alternatives, and putting them
 * on a track would invent an ordering upstream does not have.
 */
private val SLIDER_ENUMS = setOf("WindForce", "WindType")

@Composable
private fun SetupRow(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
    val asSlider = option.kind == SetupKind.ENUM && option.name in SLIDER_ENUMS &&
        option.choices.size > 1
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = option.label,
                color = Color.White,
                style = MaterialTheme.typography.titleSmall,
            )
            val shown = when {
                option.kind == SetupKind.BOUNDED_INT || option.kind == SetupKind.INT -> option.value
                // The choice's presentable name, not its raw identifier: the
                // chips show "Breezy" and so should this.
                asSlider -> option.choices.firstOrNull { it.rawLabel == option.value }?.label
                else -> null
            }
            if (shown != null) {
                Text(
                    text = shown,
                    color = SetupAccent,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
        Text(
            text = option.description,
            color = Color.White.copy(alpha = 0.55f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(6.dp))
        when (option.kind) {
            SetupKind.BOUNDED_INT -> BoundedIntControl(option, onChange)
            SetupKind.ENUM ->
                if (asSlider) EnumSliderControl(option, onChange)
                else EnumControl(option, onChange)
            SetupKind.BOOL -> BoolControl(option, onChange)
            // A plain int has no range to constrain a slider, and a free text
            // field for a game rule invites values upstream would refuse. None
            // of the exposed options is currently one, so it shows as read-only
            // rather than as a control that might mislead.
            SetupKind.INT -> Unit
        }
    }
}

@Composable
private fun BoundedIntControl(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
    val current = option.value.toIntOrNull() ?: option.minValue
    // Stepped to upstream's own step value: MoneyStarting moves in 10000s, not
    // in 1s, and a slider that offered 1s would produce values the engine
    // accepts but no player wants to aim at.
    val stepCount = if (option.stepValue > 0) {
        ((option.maxValue - option.minValue) / option.stepValue - 1).coerceAtLeast(0)
    } else {
        0
    }
    // Ticks only when there are few enough to mean something. Number Of Rounds
    // runs 0-100 in 1s and Money Starting 0-500000 in 10000s, and a track
    // stippled with fifty or a hundred dots reads as decoration rather than as
    // "these are your choices". Snapping still happens either way - it is in
    // onValueChange, not in the tick marks.
    Slider(
        value = current.toFloat(),
        onValueChange = { raw ->
            val snapped = if (option.stepValue > 0) {
                option.minValue +
                    ((raw - option.minValue) / option.stepValue).roundToInt() * option.stepValue
            } else {
                raw.roundToInt()
            }
            onChange(option, snapped.coerceIn(option.minValue, option.maxValue).toString())
        },
        valueRange = option.minValue.toFloat()..option.maxValue.toFloat(),
        steps = if (stepCount in 1..12) stepCount else 0,
        colors = SliderDefaults.colors(
            thumbColor = SetupAccent,
            activeTrackColor = SetupAccent.copy(alpha = 0.7f),
            inactiveTrackColor = Color.White.copy(alpha = 0.2f),
        ),
    )
}

@Composable
private fun EnumSliderControl(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
    // The slider runs over positions in upstream's list, not over the enum's
    // numbers. They happen to agree for both wind options, but nothing
    // guarantees an enum is numbered contiguously from zero - WindForce
    // already skips nothing only by luck - and a gap would leave dead stretches
    // of track that snap to a value the option does not have.
    val index = option.choices.indexOfFirst { it.rawLabel == option.value }.coerceAtLeast(0)
    val last = option.choices.size - 1
    Slider(
        value = index.toFloat(),
        onValueChange = { raw ->
            val picked = option.choices[raw.roundToInt().coerceIn(0, last)]
            if (picked.rawLabel != option.value) onChange(option, picked.rawLabel)
        },
        valueRange = 0f..last.toFloat(),
        // One tick per choice: nine of them at most, and here they do mean
        // "these are your choices" - there is nothing between Breezy and Gale.
        steps = (last - 1).coerceAtLeast(0),
        colors = SliderDefaults.colors(
            thumbColor = SetupAccent,
            activeTrackColor = SetupAccent.copy(alpha = 0.7f),
            inactiveTrackColor = Color.White.copy(alpha = 0.2f),
        ),
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun EnumControl(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
    val current = option.value
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        option.choices.forEach { choice ->
            // Upstream's enums compare by their identifier, not their number:
            // getValueAsString returns "WallConcrete", so that is what a
            // selection has to be tested and sent as.
            val selected = current == choice.rawLabel
            FilterChip(
                selected = selected,
                onClick = { onChange(option, choice.rawLabel) },
                label = { Text(choice.label, style = MaterialTheme.typography.bodySmall) },
                colors = FilterChipDefaults.filterChipColors(
                    containerColor = Color.White.copy(alpha = 0.06f),
                    labelColor = Color.White.copy(alpha = 0.75f),
                    selectedContainerColor = SetupAccent.copy(alpha = 0.3f),
                    selectedLabelColor = Color.White,
                ),
            )
        }
    }
}

@Composable
private fun BoolControl(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
    val on = option.value.equals("true", ignoreCase = true) || option.value == "1"
    Switch(
        checked = on,
        onCheckedChange = { onChange(option, if (it) "true" else "false") },
        colors = SwitchDefaults.colors(
            checkedThumbColor = SetupAccent,
            checkedTrackColor = SetupAccent.copy(alpha = 0.4f),
        ),
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ModRow(mods: List<String>, selected: String, onChange: (String) -> Unit) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text("Mod", color = Color.White, style = MaterialTheme.typography.titleSmall)
        Text(
            text = "Which set of weapons, landscapes and models to play with",
            color = Color.White.copy(alpha = 0.55f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(6.dp))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            mods.forEach { mod ->
                FilterChip(
                    selected = mod == selected,
                    onClick = { onChange(mod) },
                    // "none" is upstream's own name for the base game, which
                    // reads as an absence rather than a choice on a button.
                    label = {
                        Text(
                            if (mod == "none") "Scorched3D" else mod.replaceFirstChar { it.uppercase() },
                            style = MaterialTheme.typography.bodySmall,
                        )
                    },
                    colors = FilterChipDefaults.filterChipColors(
                        containerColor = Color.White.copy(alpha = 0.06f),
                        labelColor = Color.White.copy(alpha = 0.75f),
                        selectedContainerColor = SetupAccent.copy(alpha = 0.3f),
                        selectedLabelColor = Color.White,
                    ),
                )
            }
        }
        HorizontalDivider(
            color = Color.White.copy(alpha = 0.08f),
            modifier = Modifier.padding(vertical = 10.dp),
        )
    }
}
