package com.rm.scorchdroid

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
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
enum class AppScreen { SPLASH, MENU, SINGLE_PLAYER, MULTIPLAYER, SETUP, SETTINGS, ABOUT, GAME }

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
                .padding(horizontal = 32.dp),
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
                modifier = Modifier.fillMaxWidth().widthIn(max = 320.dp),
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
        modifier = Modifier
            .fillMaxWidth()
            .widthIn(max = 340.dp)
            .padding(vertical = 6.dp),
        contentPadding = PaddingValues(vertical = 14.dp),
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(label, style = MaterialTheme.typography.titleMedium)
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = Color.White.copy(alpha = 0.6f),
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
    onNewGame: () -> Unit,
    onTutorial: () -> Unit,
    tutorialEnabled: Boolean,
    onBack: () -> Unit,
) {
    MenuBackdrop {
        Title("Single Player")
        Spacer(Modifier.height(36.dp))
        MenuButton("New Game", "Against bots on this device", onClick = onNewGame)
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

@Composable
fun MultiplayerScreen(
    onHost: () -> Unit,
    onJoin: () -> Unit,
    onBack: () -> Unit,
) {
    MenuBackdrop {
        Title("Multiplayer")
        Spacer(Modifier.height(36.dp))
        MenuButton("Host Game", "Others on your network can join", onClick = onHost)
        MenuButton("Join Game", "Find a game on your network", onClick = onJoin)
        Spacer(Modifier.height(12.dp))
        TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
    }
}

/**
 * M11 builds the real settings screen. Until then the menu entry leads
 * somewhere that says so, rather than to a button that does nothing.
 */
@Composable
fun SettingsPlaceholderScreen(onBack: () -> Unit) {
    MenuBackdrop {
        Title("Settings")
        Spacer(Modifier.height(24.dp))
        Text(
            text = "Not built yet.\n\nSound, graphics detail, HUD and control options " +
                "will live here.",
            color = Color.White.copy(alpha = 0.75f),
            style = MaterialTheme.typography.bodyMedium,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(24.dp))
        TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
    }
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
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp, vertical = 32.dp),
        ) {
            Text(
                "ScorchDroid $versionName",
                color = Color.White,
                fontSize = 26.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(16.dp))
            AboutParagraph(
                "An Android port of Scorched3D, the 3D artillery game based on the " +
                    "classic Scorched Earth. The simulation - weapons, physics, terrain " +
                    "destruction, economy and bot AI - is upstream's own C++ code. The " +
                    "renderer and the whole interface are new."
            )
            AboutHeading("Copyright")
            AboutParagraph(
                "Scorched3D is Copyright (C) 2000-2011 Gavin Camp and contributors.\n" +
                    "Android port by rm."
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
            TextButton(onClick = onBack) { Text("Back", color = MenuAccent) }
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
