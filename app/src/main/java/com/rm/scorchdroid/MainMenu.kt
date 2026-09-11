package com.rm.scorchdroid

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
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
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * M9: which of the app's top-level screens is showing.
 *
 * The game itself is one of these rather than the whole app, which is the
 * change M9 is really about: until now the app *was* the game, the host/join
 * choice happened once at launch, and the only way out was to kill it.
 */
enum class AppScreen {
    SPLASH, MENU, SINGLE_PLAYER, QUICK_GAME, MULTIPLAYER, SETUP, JOINING, SETTINGS, ABOUT, GAME
}

/** The palette the menu screens share, so they read as one thing. */
private val MenuTop = Color(0xFF16213A)
private val MenuBottom = Color(0xFF2B1B3D)
private val MenuAccent = Color(0xFFB39DFF)

@Composable
private fun MenuBackdrop(content: @Composable () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(MenuTop, MenuBottom))),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
            modifier = Modifier
                .fillMaxWidth()
                // Scrolls only when it has to: a column shorter than the
                // screen still measures to its content, so the Box above keeps
                // centring it and the short menus look exactly as they did.
                // The quick-game list is the one that can outgrow a landscape
                // phone, and did as soon as a second mod's games appeared in
                // it.
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 32.dp, vertical = 24.dp),
        ) {
            content()
        }
    }
}

@Composable
private fun Title(subtitle: String? = null) {
    Text(
        text = "ScorchDroid",
        color = Color.White,
        fontSize = 40.sp,
        fontWeight = FontWeight.Bold,
        textAlign = TextAlign.Center,
    )
    if (subtitle != null) {
        Spacer(Modifier.height(6.dp))
        Text(
            text = subtitle,
            color = MenuAccent,
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
        )
    }
}

/**
 * Shown while the first run extracts upstream's ~90MB `data/` tree to internal
 * storage and the engine initialises. Not decoration: that work is real, takes
 * a noticeable while on a cold start, and used to happen behind a bare line of
 * status text on the HUD.
 */
@Composable
fun SplashScreen(status: String, progress: Float?) {
    MenuBackdrop {
        Title("An Android port of Scorched3D")
        Spacer(Modifier.height(40.dp))
        if (progress != null && progress in 0f..1f) {
            LinearProgressIndicator(
                progress = { progress },
                color = MenuAccent,
                modifier = Modifier.widthIn(max = 320.dp).fillMaxWidth(),
            )
        } else {
            CircularProgressIndicator(color = MenuAccent)
        }
        Spacer(Modifier.height(16.dp))
        Text(
            text = status,
            color = Color.White.copy(alpha = 0.75f),
            style = MaterialTheme.typography.bodySmall,
            textAlign = TextAlign.Center,
        )
    }
}

@Composable
private fun MenuButton(
    label: String,
    subtitle: String? = null,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Button(
        onClick = onClick,
        enabled = enabled,
        colors = ButtonDefaults.buttonColors(
            containerColor = MenuAccent.copy(alpha = 0.18f),
            contentColor = Color.White,
            disabledContainerColor = Color.White.copy(alpha = 0.06f),
            disabledContentColor = Color.White.copy(alpha = 0.35f),
        ),
        // widthIn *before* fillMaxWidth: the other order lets fillMaxWidth
        // take the whole width first, which on a landscape phone made every
        // button 2340px of purple.
        modifier = Modifier
            .widthIn(max = 340.dp)
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        // Horizontal padding matters as soon as a subtitle is long enough to
        // wrap - the quick-game list carries upstream's own two-sentence
        // descriptions, and without this they ran to the button's edge and
        // were clipped a character in on both sides.
        contentPadding = PaddingValues(horizontal = 20.dp, vertical = 14.dp),
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(label, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.White.copy(alpha = 0.6f),
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
fun MainMenuScreen(
    onSinglePlayer: () -> Unit,
    onMultiplayer: () -> Unit,
    onSettings: () -> Unit,
    onAbout: () -> Unit,
) {
    MenuBackdrop {
        Title()
        Spacer(Modifier.height(36.dp))
        MenuButton("Single Player", onClick = onSinglePlayer)
        MenuButton("Multiplayer", onClick = onMultiplayer)
        MenuButton("Settings", onClick = onSettings)
        MenuButton("About", onClick = onAbout)
    }
}

@Composable
fun SinglePlayerScreen(
    onQuickGame: () -> Unit,
    quickGameEnabled: Boolean,
    onNewGame: () -> Unit,
    onTutorial: () -> Unit,
    tutorialEnabled: Boolean,
    onBack: () -> Unit,
) {
    MenuBackdrop {
        Title("Single Player")
        Spacer(Modifier.height(36.dp))
        // M14: first, and first for a reason - it is the shortest path from
        // here to playing. New Game is the one that asks questions.
        MenuButton(
            "Quick Game",
            if (quickGameEnabled) "Pick a difficulty and play" else "No games found",
            enabled = quickGameEnabled,
            onClick = onQuickGame,
        )
        MenuButton("New Game", "Choose the settings yourself", onClick = onNewGame)
        MenuButton(
            "Tutorial",
            if (tutorialEnabled) "Learn the controls" else "Not built yet",
            enabled = tutorialEnabled,
            onClick = onTutorial,
        )
        Spacer(Modifier.height(12.dp))
        TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
    }
}

/**
 * M14: the difficulty presets, which are upstream's own single-player menu.
 *
 * Nothing here is written by this port. Each mod's modinfo.xml names its
 * games, gives the words to describe them, and points at an options file; this
 * screen lists what the mods say. The base game contributes Target practice,
 * Easy, Normal and Hard; a mod contributes its own, which is why entries are
 * shown under the mod they came from rather than merged into one list - the
 * Apocalypse ones play a different game entirely.
 */
@Composable
fun QuickGameScreen(
    presets: List<GamePreset>,
    onPick: (GamePreset) -> Unit,
    onBack: () -> Unit,
) {
    MenuBackdrop {
        Title("Quick Game")
        Spacer(Modifier.height(24.dp))
        // Grouped only when there is more than one mod to distinguish: with
        // just the base game installed a heading over the only list is noise.
        val byMod = presets.groupBy { it.mod }
        byMod.forEach { (mod, entries) ->
            if (byMod.size > 1) {
                Spacer(Modifier.height(8.dp))
                Text(
                    modLabel(mod),
                    color = MenuAccent,
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                )
            }
            entries.forEach { preset ->
                MenuButton(preset.name, preset.summary) { onPick(preset) }
            }
        }
        Spacer(Modifier.height(12.dp))
        TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
    }
}

@Composable
fun MultiplayerScreen(
    onHost: () -> Unit,
    onHostBluetooth: () -> Unit,
    onJoin: () -> Unit,
    onJoinBluetooth: () -> Unit,
    onBack: () -> Unit,
    bluetoothEnabled: Boolean = true,
) {
    MenuBackdrop {
        Title("Multiplayer")
        Spacer(Modifier.height(36.dp))
        MenuButton("Host Game", "Over Wi-Fi, a hotspot, or Wi-Fi Direct", onClick = onHost)
        // Its own button rather than an option inside hosting, because it is
        // genuinely a different game: the engine has one network interface,
        // so a Bluetooth game is not also a Wi-Fi one and the choice cannot
        // be made after the fact.
        MenuButton(
            "Host over Bluetooth",
            "No Wi-Fi at all - for two devices side by side",
            onClick = onHostBluetooth,
            enabled = bluetoothEnabled,
        )
        MenuButton("Join Game", "Find a game on the network", onClick = onJoin)
        // Its own search, not a row in the other one. A Bluetooth scan
        // cannot be narrowed to devices running the game, so it turns up
        // every speaker, headset and car in range - which buried the two
        // phones it was there for.
        MenuButton(
            "Join over Bluetooth",
            "Pick the device hosting the game",
            onClick = onJoinBluetooth,
            enabled = bluetoothEnabled,
        )
        Spacer(Modifier.height(12.dp))
        TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
    }
}

/**
 * M10: shown while finding and connecting to a game.
 *
 * It exists because the join flow used to run over the *game* screen: picking
 * Join built the GL surface and switched to the HUD immediately, so the
 * discovery dialog appeared on top of aiming sliders and a Fire button
 * belonging to a game that did not exist yet. The surface is now built only
 * once a connection is established.
 *
 * The discovery and manual-address dialogs themselves are unchanged and still
 * come from [HudDialogHost] - they are the same dialogs, just no longer over a
 * pretend battlefield.
 */
@Composable
fun JoiningScreen(status: String, dialog: HudDialog, onBack: () -> Unit) {
    MenuBackdrop {
        Title("Join Game")
        Spacer(Modifier.height(28.dp))
        Text(
            text = status,
            color = Color.White.copy(alpha = 0.75f),
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(20.dp))
        TextButton(onClick = onBack) { Text("Cancel", color = MenuAccent) }
    }
    HudDialogHost(dialog)
}

/**
 * M9: the licence notice, and the reason it exists.
 *
 * ScorchDroid links Scorched3D's GPLv2+ code into the APK, so distributing a
 * build carries an obligation to state that and to offer the corresponding
 * source. Three signed releases went out before this screen existed; it is
 * the first thing M9 puts right.
 *
 * The licence text is the repository's own LICENSE file, staged into the APK
 * at build time (see stageLicense in app/build.gradle.kts) rather than copied
 * into assets by hand, so what the app shows and what the repository carries
 * cannot drift apart.
 */
@Composable
fun AboutScreen(
    versionName: String,
    upstreamCommit: String,
    licenseText: String,
    onBack: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(MenuTop, MenuBottom))),
    ) {
        Column(
            modifier = Modifier
                // Capped for the same reason as the settings screens: a
                // full-width line of licence text on a landscape phone is
                // unreadable.
                .widthIn(max = 560.dp)
                .fillMaxSize()
                .align(Alignment.TopCenter)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 32.dp),
        ) {
            // Back on the title row rather than after the whole GPL text,
            // which is several screens of scrolling away.
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "ScorchDroid $versionName",
                    color = Color.White,
                    fontSize = 26.sp,
                    fontWeight = FontWeight.Bold,
                )
                TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
            }
            Spacer(Modifier.height(10.dp))
            AboutParagraph(
                "An Android port of Scorched3D, the 3D artillery game based on the " +
                    "classic Scorched Earth. The simulation - weapons, physics, terrain " +
                    "destruction, economy and bot AI - is upstream's own C++ code. The " +
                    "renderer and the whole interface are new."
            )
            AboutHeading("Copyright")
            AboutParagraph(
                "Scorched3D is Copyright (C) 2000-2011 Gavin Camp and contributors.\n" +
                    "Android port by roge-rm."
            )
            AboutHeading("Licence")
            AboutParagraph(
                "This program is free software: you can redistribute it and/or modify it " +
                    "under the terms of the GNU General Public License as published by the " +
                    "Free Software Foundation, either version 2 of the License, or (at your " +
                    "option) any later version.\n\n" +
                    "This program is distributed in the hope that it will be useful, but " +
                    "WITHOUT ANY WARRANTY; without even the implied warranty of " +
                    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU " +
                    "General Public License below for more details."
            )
            AboutHeading("Source code")
            AboutParagraph(
                "The complete corresponding source for this build:\n\n" +
                    "github.com/roge-rm/ScorchDroid\n\n" +
                    "It contains this port's own source, the exact upstream commit it is " +
                    "built against ($upstreamCommit, from github.com/bberberov/scorched3d), " +
                    "and every patch applied to that checkout, under patches/."
            )
            AboutHeading("Game data")
            AboutParagraph(
                "The bundled landscapes, models, textures, sounds and language files are " +
                    "upstream's, included unmodified and under the same licence."
            )
            AboutHeading("GNU General Public License, version 2")
            Text(
                text = licenseText,
                color = Color.White.copy(alpha = 0.7f),
                fontSize = 11.sp,
                lineHeight = 15.sp,
                modifier = Modifier.padding(top = 8.dp),
            )
            Spacer(Modifier.height(24.dp))
        }
    }
}

@Composable
private fun AboutHeading(text: String) {
    Text(
        text = text,
        color = MenuAccent,
        style = MaterialTheme.typography.titleSmall,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(top = 20.dp),
    )
}

@Composable
private fun AboutParagraph(text: String) {
    Text(
        text = text,
        color = Color.White.copy(alpha = 0.85f),
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.padding(top = 6.dp),
    )
}
