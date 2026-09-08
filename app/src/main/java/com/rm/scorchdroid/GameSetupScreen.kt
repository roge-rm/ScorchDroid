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
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
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
    onChange: (SetupOption, String) -> Unit,
    onReset: () -> Unit,
    onStart: () -> Unit,
    onBack: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(SetupTop, SetupBottom))),
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 28.dp),
        ) {
            Text(
                text = title,
                color = Color.White,
                fontSize = 26.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = "These are Scorched3D's own settings, with its own limits.",
                color = SetupAccent,
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(20.dp))

            options.forEach { option ->
                SetupRow(option, onChange)
                HorizontalDivider(
                    color = Color.White.copy(alpha = 0.08f),
                    modifier = Modifier.padding(vertical = 10.dp),
                )
            }

            Spacer(Modifier.height(8.dp))
            Button(
                onClick = onStart,
                colors = ButtonDefaults.buttonColors(
                    containerColor = SetupAccent.copy(alpha = 0.25f),
                    contentColor = Color.White,
                ),
                modifier = Modifier.fillMaxWidth().widthIn(max = 340.dp),
            ) {
                Text("Start", style = MaterialTheme.typography.titleMedium)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                TextButton(onClick = onBack) { Text("Back", color = SetupAccent) }
                TextButton(onClick = onReset) { Text("Reset to defaults", color = SetupAccent) }
            }
        }
    }
}

@Composable
private fun SetupRow(option: SetupOption, onChange: (SetupOption, String) -> Unit) {
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
            if (option.kind == SetupKind.BOUNDED_INT || option.kind == SetupKind.INT) {
                Text(
                    text = option.value,
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
            SetupKind.ENUM -> EnumControl(option, onChange)
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
