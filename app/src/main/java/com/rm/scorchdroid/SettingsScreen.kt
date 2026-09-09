package com.rm.scorchdroid

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
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
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
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
/**
 * The settings screen's tabs.
 *
 * One page of a dozen rows was too much to take in, and M16's tank, colour
 * and avatar pickers made it worse - the page ran for four screens and the
 * controls at the bottom were effectively hidden. Splitting it also groups
 * the settings by what a player is actually trying to change: who they are,
 * what they hear, what they see, how they play.
 */
private enum class SettingsTab(val label: String) {
    PLAYER("Player"),
    AUDIO("Audio"),
    DISPLAY("Display"),
    CONTROLS("Controls"),
}

@Composable
fun SettingsScreen(settings: GameSettings, dataRoot: String, onBack: () -> Unit) {
    // Remembered across tab switches only, not across visits: coming back to
    // Settings should start where the screen starts.
    var tab by remember { mutableStateOf(SettingsTab.PLAYER) }

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
                .padding(horizontal = 24.dp, vertical = 28.dp),
        ) {
            // Back sits on the title row, not at the foot of the page: a page
            // that scrolls for several times its own height means scrolling
            // back down to leave. The row and the tabs stay put while only
            // the settings under them scroll.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Settings", color = Color.White, fontSize = 26.sp, fontWeight = FontWeight.Bold)
                TextButton(onClick = onBack) { Text("Back", color = SettingsAccent) }
            }

            // Fixed rather than scrollable: a scrolling row sizes each tab
            // to its label and packs them from the left, which left the four
            // of them bunched against one edge with the slack at the other.
            // These four share the width evenly instead, each label centred
            // in its own share of it.
            TabRow(
                selectedTabIndex = tab.ordinal,
                containerColor = Color.Transparent,
                contentColor = SettingsAccent,
                divider = {
                    HorizontalDivider(color = Color.White.copy(alpha = 0.12f))
                },
            ) {
                SettingsTab.entries.forEach { candidate ->
                    Tab(
                        selected = candidate == tab,
                        onClick = { tab = candidate },
                        selectedContentColor = SettingsAccent,
                        unselectedContentColor = Color.White.copy(alpha = 0.55f),
                        text = {
                            Text(candidate.label, style = MaterialTheme.typography.titleSmall)
                        },
                    )
                }
            }

            // Keyed on the tab, so each one starts at its own top rather
            // than inheriting how far the last was scrolled.
            val scroll = remember(tab) { ScrollState(0) }
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(scroll)
                    .padding(top = 8.dp),
            ) {
                when (tab) {
                    SettingsTab.PLAYER -> {
                        NameRow(settings)
                        // M16: the rest of upstream's PlayerDialog. Read from
                        // the shipped data - the mod's tanks.xml, upstream's
                        // colour palette, the avatars it ships - so a mod's
                        // own tanks appear here by itself.
                        TankModelRow(settings)
                        TankColorRow(settings)
                        AvatarRow(settings, dataRoot)
                    }

                    SettingsTab.AUDIO -> {
                        SwitchRow(
                            "Sound effects",
                            "Explosions, weapon fire and impacts",
                            settings.soundEnabled,
                        ) { settings.updateSoundEnabled(it) }
                        SwitchRow(
                            "Ambient sound",
                            "The landscape's own atmosphere - waves, rain, birds in the trees",
                            settings.ambientEnabled,
                        ) { settings.updateAmbientEnabled(it) }
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
                    }

                    SettingsTab.DISPLAY -> {
                        // Two groups under one tab: both are "what is on the
                        // screen", and neither is long enough to be a tab of
                        // its own.
                        Group("Landscape")
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

                        // M23: the terrain's own resolution. The range comes
                        // from the renderer rather than being repeated here.
                        val detailRange = remember {
                            val parts = NativeBridge.getTerrainDetailRange().split("|")
                            val min = parts.getOrNull(0)?.toIntOrNull() ?: 32
                            val max = parts.getOrNull(1)?.toIntOrNull() ?: 256
                            min.toFloat()..max.toFloat()
                        }
                        SliderRow(
                            "Landscape detail",
                            if (settings.terrainDetail >= detailRange.endInclusive.toInt()) {
                                "Full"
                            } else {
                                "${settings.terrainDetail}"
                            },
                            settings.terrainDetail.toFloat(),
                            detailRange,
                        ) {
                            // Rounded to a multiple of 16: the mesh is rebuilt
                            // on every change, and a slider that fired for
                            // each pixel of travel would rebuild it dozens of
                            // times on one drag.
                            settings.updateTerrainDetail((it / 16f).roundToInt() * 16)
                        }
                        Text(
                            "How finely the ground is drawn. Full is the whole heightmap, " +
                                "as Scorched3D draws it; lower is cheaper on a slow device.",
                            color = Color.White.copy(alpha = 0.55f),
                            style = MaterialTheme.typography.bodySmall,
                        )

                        SwitchRow(
                            "Scorched3D's ocean",
                            "Its own wave spectrum, driven by the round's wind, instead of " +
                                "this port's two rolling waves. Costs a little CPU",
                            settings.originalOcean,
                        ) {
                            settings.updateOriginalOcean(it)
                        }

                        // Upstream's own effects detail, with upstream's own
                        // three particle budgets behind it. Normal is its
                        // default and the faithful one; Low exists because a
                        // napalm field is by far the heaviest thing this
                        // renderer draws.
                        SliderRow(
                            "Effects detail",
                            when (settings.effectsDetail) {
                                0 -> "Low"
                                1 -> "Normal"
                                else -> "High"
                            },
                            settings.effectsDetail.toFloat(),
                            0f..2f,
                        ) {
                            settings.updateEffectsDetail(it.roundToInt())
                        }
                        Text(
                            "How many flames, sparks and smoke puffs may be alight at " +
                                "once - 100, 6000 or 10000, which are Scorched3D's own " +
                                "numbers. Fire thins out when the budget runs out.",
                            color = Color.White.copy(alpha = 0.55f),
                            style = MaterialTheme.typography.bodySmall,
                        )

                        // Three positions rather than a switch: the middle one
                        // is a real answer for a device that cannot afford
                        // reflecting everything but can afford the land.
                        SliderRow(
                            "Water reflections",
                            when (settings.reflectionLevel) {
                                0 -> "Sky"
                                1 -> "Sky and land"
                                else -> "Everything"
                            },
                            settings.reflectionLevel.toFloat(),
                            0f..2f,
                        ) {
                            settings.updateReflectionLevel(it.roundToInt())
                        }
                        Text(
                            "Sky is this port's own colour reflection. The other two draw " +
                                "the scene a second time, mirrored in the water, as " +
                                "Scorched3D does - Everything adds the clouds, tanks, " +
                                "scenery, shots and explosions it reflects.",
                            color = Color.White.copy(alpha = 0.55f),
                            style = MaterialTheme.typography.bodySmall,
                        )

                        SwitchRow(
                            "Scorched3D's aim sight",
                            "Its own: a protractor ring around the tank and a separate " +
                                "bearing marker on the ground. Off is this port's single blade",
                            settings.originalSight,
                        ) {
                            settings.updateOriginalSight(it)
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
                    }

                    SettingsTab.CONTROLS -> {
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
                    }
                }

                Spacer(Modifier.height(24.dp))
            }
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

/**
 * M16: the tank model, in a dialog rather than inline.
 *
 * The base game declares a hundred and five of them and a mod may declare
 * more, so this is the one identity choice that cannot be a row of chips. The
 * list is upstream's own order, which groups them roughly by category.
 */
@Composable
private fun TankModelRow(settings: GameSettings) {
    var picking by remember { mutableStateOf(false) }
    // Read once, when the row first appears: the list only changes with the
    // mod, and the mod cannot change while this screen is open.
    val models = remember { NativeBridge.getTankModels().toList() }
    if (models.isEmpty()) return

    PickerRow(
        title = "Tank",
        subtitle = "Which tank you drive",
        value = settings.tankModel.ifEmpty { "Random" },
    ) { picking = true }

    if (picking) {
        AlertDialog(
            onDismissRequest = { picking = false },
            title = { Text("Tank") },
            text = {
                Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                    // "Random" first, and it is not one of upstream's models -
                    // it is the empty choice, which is what the game did
                    // before anyone could pick.
                    ChoiceLine("Random", settings.tankModel.isEmpty()) {
                        settings.updateTankModel("")
                        picking = false
                    }
                    models.forEach { model ->
                        ChoiceLine(model, model == settings.tankModel) {
                            settings.updateTankModel(model)
                            picking = false
                        }
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { picking = false }) { Text("Close") }
            },
        )
    }
}

@Composable
private fun ChoiceLine(label: String, selected: Boolean, onClick: () -> Unit) {
    Text(
        label,
        color = if (selected) SettingsAccent else Color.Unspecified,
        fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp),
    )
}

@Composable
private fun PickerRow(title: String, subtitle: String, value: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.fillMaxWidth(0.6f)) {
            Text(title, color = Color.White, style = MaterialTheme.typography.bodyLarge)
            Text(
                subtitle,
                color = Color.White.copy(alpha = 0.55f),
                style = MaterialTheme.typography.bodySmall,
            )
        }
        Text(value, color = SettingsAccent, style = MaterialTheme.typography.bodyLarge)
    }
}

/**
 * M16: the tank colour, as upstream's own twenty-six swatches.
 *
 * The game still has the last word: TankChangeSimAction ignores a colour
 * another tank in the game already has, and hands you the one you were
 * allocated instead. That is upstream's rule and worth keeping - two tanks the
 * same colour is a real problem in a game about shooting at the right one.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun TankColorRow(settings: GameSettings) {
    val colors = remember { NativeBridge.getTankColors() }
    if (colors.isEmpty()) return

    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text("Colour", color = Color.White, style = MaterialTheme.typography.bodyLarge)
        Text(
            "Your tank's colour, if it is free when the game starts",
            color = Color.White.copy(alpha = 0.55f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(8.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Swatch(
                color = Color.Transparent,
                selected = settings.tankColorIndex < 0,
                label = "Any",
            ) { settings.updateTankColorIndex(-1) }
            colors.forEachIndexed { index, rgb ->
                Swatch(
                    color = Color(0xFF000000.toInt() or rgb),
                    selected = index == settings.tankColorIndex,
                ) { settings.updateTankColorIndex(index) }
            }
        }
    }
}

@Composable
private fun Swatch(
    color: Color,
    selected: Boolean,
    label: String? = null,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(36.dp)
            .clip(CircleShape)
            .background(color)
            .border(
                width = if (selected) 3.dp else 1.dp,
                color = if (selected) Color.White else Color.White.copy(alpha = 0.3f),
                shape = CircleShape,
            )
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        if (label != null) {
            Text(
                label,
                color = Color.White.copy(alpha = 0.8f),
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}

/**
 * M16: the avatar, shown against your name in the score table.
 *
 * Upstream ships nineteen and lets a player send any PNG; these are the ones
 * everyone else already has, so no image has to cross the network for another
 * player to see it. The files are read straight off the extracted data root -
 * they are ordinary PNGs, a few kilobytes each.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun AvatarRow(settings: GameSettings, dataRoot: String) {
    val avatars = remember { NativeBridge.getAvatars().toList() }
    if (avatars.isEmpty()) return

    Column(modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text("Avatar", color = Color.White, style = MaterialTheme.typography.bodyLarge)
        Text(
            "Shown beside your name in the score table",
            color = Color.White.copy(alpha = 0.55f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(8.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(44.dp)
                    .clip(RoundedCornerShape(6.dp))
                    .border(
                        width = if (settings.avatar.isEmpty()) 3.dp else 1.dp,
                        color = if (settings.avatar.isEmpty()) SettingsAccent
                            else Color.White.copy(alpha = 0.3f),
                        shape = RoundedCornerShape(6.dp),
                    )
                    .clickable { settings.updateAvatar("") },
                contentAlignment = Alignment.Center,
            ) {
                Text("None", color = Color.White.copy(alpha = 0.7f),
                    style = MaterialTheme.typography.labelSmall)
            }
            avatars.forEach { path ->
                AvatarTile(path, dataRoot, path == settings.avatar) {
                    settings.updateAvatar(path)
                }
            }
        }
    }
}

@Composable
private fun AvatarTile(path: String, dataRoot: String, selected: Boolean, onClick: () -> Unit) {
    val image = rememberAvatarBitmap(dataRoot, path) ?: return
    Image(
        bitmap = image,
        contentDescription = path.substringAfterLast('/').removeSuffix(".png"),
        modifier = Modifier
            .size(44.dp)
            .clip(RoundedCornerShape(6.dp))
            .border(
                width = if (selected) 3.dp else 1.dp,
                color = if (selected) SettingsAccent else Color.White.copy(alpha = 0.3f),
                shape = RoundedCornerShape(6.dp),
            )
            .clickable(onClick = onClick),
    )
}

/**
 * Decodes one avatar PNG, remembered per path so scrolling the settings screen
 * does not decode nineteen files on every recomposition. Null when the file is
 * missing or is not an image the platform reads, which the callers treat as
 * "show nothing" rather than as an error.
 */
@Composable
fun rememberAvatarBitmap(dataRoot: String, path: String): ImageBitmap? = remember(path) {
    if (path.isEmpty()) return@remember null
    runCatching {
        BitmapFactory.decodeFile(File(dataRoot, path).path)?.asImageBitmap()
    }.getOrNull()
}

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
