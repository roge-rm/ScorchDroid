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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
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
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt

private val SettingsTop = Color(0xFF16213A)
private val SettingsBottom = Color(0xFF2B1B3D)
private val SettingsAccent = Color(0xFFB39DFF)

/**
 * M11: the settings screen.
 *
 * Deliberately much smaller than upstream's. `OptionsDisplay` carries around a
 * hundred and twenty entries, but the great majority are legacy GL capability
 * switches - NoVBO, NoROAM, NoGLCubeMap, NoTessalation - that mean nothing to a
 * GLES3 renderer written from scratch. Every item here maps to a switch this
 * port actually has, and anything that changes the *game* rather than the
 * device belongs in the pre-game setup screen instead.
 */
@Composable
fun SettingsScreen(settings: GameSettings, onBack: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(SettingsTop, SettingsBottom))),
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
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 28.dp),
        ) {
            Text("Settings", color = Color.White, fontSize = 26.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(20.dp))

            Group("Player")
            NameRow(settings)

            Group("Sound")
            SwitchRow(
                "Sound effects",
                "Explosions, weapon fire and impacts",
                settings.soundEnabled,
            ) { settings.updateSoundEnabled(it) }
            SwitchRow(
                "Music",
                "Scorched3D's own loops, changing with the state of the game",
                settings.musicEnabled,
            ) { settings.updateMusicEnabled(it) }
            SliderRow(
                "Music volume",
                "${(settings.musicVolume * 100).roundToInt()}%",
                settings.musicVolume,
                0f..1f,
            ) { settings.updateMusicVolume(it) }

            Group("Graphics")
            SwitchRow(
                "Trees",
                "A landscape scatters up to two thousand; turning them off is the " +
                    "single biggest saving on a slow device",
                settings.showTrees,
            ) {
                settings.updateShowTrees(it)
                settings.applyAll()
            }
            SwitchRow(
                "Distance fog",
                "Fades the landscape towards the horizon",
                settings.showFog,
            ) {
                settings.updateShowFog(it)
                settings.applyAll()
            }

            Group("HUD")
            SwitchRow(
                "Name plates",
                "Each tank's name above it",
                settings.showNamePlates,
            ) { settings.updateShowNamePlates(it) }
            SwitchRow(
                "Health bars",
                "The green bar under each name",
                settings.showHealthBars,
            ) { settings.updateShowHealthBars(it) }
            SliderRow(
                "Chat message time",
                "${settings.chatToastSeconds}s",
                settings.chatToastSeconds.toFloat(),
                2f..15f,
            ) { settings.updateChatToastSeconds(it.roundToInt()) }

            Group("Controls")
            SwitchRow(
                "Tap to aim",
                "Tap the ground to aim there; the sliders always work either way",
                settings.tapToAim,
            ) { settings.updateTapToAim(it) }
            SwitchRow(
                "Invert drag",
                "Reverses the up/down direction when swinging the camera",
                settings.invertDrag,
            ) { settings.updateInvertDrag(it) }
            SwitchRow(
                "Left-hand mode",
                "Mirrors the controls for left-handed play",
                settings.leftHandMode,
            ) { settings.updateLeftHandMode(it) }
            SliderRow(
                "Control opacity",
                "${(settings.controlOpacity * 100).roundToInt()}%",
                settings.controlOpacity,
                0.3f..1.0f,
            ) { settings.updateControlOpacity(it) }

            Spacer(Modifier.height(24.dp))
            TextButton(onClick = onBack) { Text("Back", color = SettingsAccent) }
        }
    }
}

@Composable
private fun Group(title: String) {
    Spacer(Modifier.height(18.dp))
    Text(
        title,
        color = SettingsAccent,
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.Bold,
    )
    HorizontalDivider(
        color = Color.White.copy(alpha = 0.12f),
        modifier = Modifier.padding(top = 4.dp, bottom = 8.dp),
    )
}

@Composable
private fun NameRow(settings: GameSettings) {
    // Edited locally and committed when the field loses focus or the player
    // presses done, rather than on every keystroke: the engine refuses an
    // empty name, so writing through mid-edit would fight anyone clearing the
    // field to retype it.
    var draft by remember { mutableStateOf(settings.playerName) }
    OutlinedTextField(
        value = draft,
        onValueChange = { draft = it.take(24) },
        label = { Text("Name", color = Color.White.copy(alpha = 0.7f)) },
        supportingText = {
            Text(
                "Shown over your tank, and to everyone else in a network game",
                color = Color.White.copy(alpha = 0.5f),
                style = MaterialTheme.typography.bodySmall,
            )
        },
        singleLine = true,
        colors = OutlinedTextFieldDefaults.colors(
            focusedTextColor = Color.White,
            unfocusedTextColor = Color.White,
            cursorColor = Color.White,
            focusedBorderColor = SettingsAccent,
            unfocusedBorderColor = Color.White.copy(alpha = 0.35f),
        ),
        modifier = Modifier
            .fillMaxWidth()
            .onFocusChangedCommit { settings.updatePlayerName(draft); draft = settings.playerName },
    )
}

/** Commits when focus leaves the field. */
private fun Modifier.onFocusChangedCommit(commit: () -> Unit): Modifier =
    this.onFocusChanged { state -> if (!state.isFocused) commit() }

@Composable
private fun SwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.fillMaxWidth(0.78f)) {
            Text(title, color = Color.White, style = MaterialTheme.typography.bodyLarge)
            Text(
                subtitle,
                color = Color.White.copy(alpha = 0.55f),
                style = MaterialTheme.typography.bodySmall,
            )
        }
        Switch(
            checked = checked,
            onCheckedChange = onChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = SettingsAccent,
                checkedTrackColor = SettingsAccent.copy(alpha = 0.4f),
            ),
        )
    }
}

@Composable
private fun SliderRow(
    title: String,
    valueLabel: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit,
) {
    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(title, color = Color.White, style = MaterialTheme.typography.bodyLarge)
            Text(valueLabel, color = SettingsAccent, style = MaterialTheme.typography.bodyLarge)
        }
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = range,
            colors = SliderDefaults.colors(
                thumbColor = SettingsAccent,
                activeTrackColor = SettingsAccent.copy(alpha = 0.7f),
                inactiveTrackColor = Color.White.copy(alpha = 0.2f),
            ),
        )
    }
}
