package com.rm.scorchdroid

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * M4 dialog conversion: every modal in the game (host/join choice, tutorial,
 * shop, weapon quick-select, LAN discovery, manual address entry) used to be
 * a plain platform AlertDialog built directly in MainActivity - the last
 * stopgap left from before the Compose HUD (see GameHud.kt / the porting
 * plan). MainActivity still owns all the actual logic (coroutines, native
 * calls) and now just sets [GameHudState.dialog] to describe what should be
 * showing; this file only renders it.
 *
 * Convention: every callback here (onSelect/onCancel/onDismiss/onNext/...)
 * is responsible for setting `hudState.dialog` itself (to `None` or to the
 * next dialog to show) - the render functions below never touch it after
 * invoking a callback, so a callback can chain straight into a different
 * dialog (e.g. Shop -> a blocked-buy Message) without this file needing to
 * know about that.
 */
sealed class HudDialog {
    object None : HudDialog()

    data class GameModeChoice(
        val onHost: () -> Unit,
        val onJoin: () -> Unit,
    ) : HudDialog()

    data class Message(
        val text: String,
        val onDismiss: () -> Unit,
    ) : HudDialog()

    data class TutorialStep(
        val title: String,
        val message: String,
        val isLast: Boolean,
        val onNext: () -> Unit,
        val onSkip: (() -> Unit)?,
    ) : HudDialog()

    /**
     * A titled, cancelable list of tappable rows - used for the Shop, the
     * weapon quick-select, and LAN game discovery. [title] and [items] are
     * themselves Compose state so a dialog already on screen can be updated
     * live (LAN discovery finding more games over time) without replacing
     * the whole [HudDialog] instance.
     */
    class ListChoice(
        title: String,
        items: List<String>,
        val cancelLabel: String = "Cancel",
        val onSelect: (Int) -> Unit,
        val onCancel: () -> Unit,
    ) : HudDialog() {
        var title by mutableStateOf(title)
        var items by mutableStateOf(items)
    }

    data class ManualAddress(
        val onConnect: (String) -> Unit,
        val onCancel: () -> Unit,
    ) : HudDialog()
}

@Composable
fun HudDialogHost(dialog: HudDialog) {
    when (dialog) {
        is HudDialog.None -> {}

        is HudDialog.GameModeChoice -> AlertDialog(
            onDismissRequest = {},
            title = { Text("ScorchDroid") },
            text = { Text("Host a new game, or join one already running on your LAN?") },
            confirmButton = { TextButton(onClick = dialog.onHost) { Text("Host") } },
            dismissButton = { TextButton(onClick = dialog.onJoin) { Text("Join") } },
        )

        is HudDialog.Message -> AlertDialog(
            onDismissRequest = dialog.onDismiss,
            text = { Text(dialog.text) },
            confirmButton = { TextButton(onClick = dialog.onDismiss) { Text("OK") } },
        )

        is HudDialog.TutorialStep -> AlertDialog(
            onDismissRequest = {},
            title = { Text(dialog.title) },
            text = { Text(dialog.message) },
            confirmButton = {
                TextButton(onClick = dialog.onNext) { Text(if (dialog.isLast) "Got it" else "Next") }
            },
            dismissButton = dialog.onSkip?.let { skip ->
                { TextButton(onClick = skip) { Text("Skip") } }
            },
        )

        is HudDialog.ListChoice -> AlertDialog(
            onDismissRequest = dialog.onCancel,
            title = { Text(dialog.title) },
            text = {
                LazyColumn(modifier = Modifier.heightIn(max = 400.dp)) {
                    itemsIndexed(dialog.items) { index, label ->
                        Text(
                            label,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { dialog.onSelect(index) }
                                .padding(vertical = 12.dp),
                        )
                    }
                }
            },
            confirmButton = {},
            dismissButton = { TextButton(onClick = dialog.onCancel) { Text(dialog.cancelLabel) } },
        )

        is HudDialog.ManualAddress -> {
            var text by remember { mutableStateOf("") }
            AlertDialog(
                onDismissRequest = {},
                title = { Text("Enter game address") },
                text = {
                    OutlinedTextField(
                        value = text,
                        onValueChange = { text = it },
                        placeholder = { Text("192.168.1.42:47376") },
                        singleLine = true,
                    )
                },
                confirmButton = { TextButton(onClick = { dialog.onConnect(text) }) { Text("Connect") } },
                dismissButton = { TextButton(onClick = dialog.onCancel) { Text("Cancel") } },
            )
        }
    }
}
