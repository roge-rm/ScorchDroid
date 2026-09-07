package com.rm.scorchdroid

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import kotlinx.coroutines.delay
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
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

    /**
     * The Shop. Its own dialog rather than a [ListChoice] because the shop
     * is the one list long enough to get lost in: every accessory type in
     * the mod, tens of rows, previously rendered as one flat run of
     * identical padded strings with no indication of how much was below the
     * fold. This splits it by type, packs the rows tighter, and shows where
     * you are in the list.
     */
    class Shop(
        money: Int,
        entries: List<WeaponShopEntry>,
        val onSelect: (WeaponShopEntry) -> Unit,
        val onCancel: () -> Unit,
    ) : HudDialog() {
        var money by mutableIntStateOf(money)
        var entries by mutableStateOf(entries)

        /**
         * Accessories bought but not yet confirmed by the engine. A buy is
         * a queued simulator action, and the round it lands on can be a
         * couple of seconds out (ServerSimulator's send boundary), which is
         * a long time against a ~20 second buying phase - long enough that
         * the row still showing a price reads as "the tap did nothing".
         * These rows say so instead, and become a real count when the
         * engine confirms.
         */
        var pending by mutableStateOf(emptySet<Int>())

        /** The row to flash, and a token that changes on every purchase so
         *  buying the same thing twice flashes twice. */
        var flashAccessoryId by mutableIntStateOf(0)
        var flashToken by mutableIntStateOf(0)

        fun markPending(accessoryId: Int, price: Int) {
            pending = pending + accessoryId
            money -= price
            flashAccessoryId = accessoryId
            flashToken++
        }

        fun settle(accessoryId: Int, money: Int, entries: List<WeaponShopEntry>) {
            pending = pending - accessoryId
            this.money = money
            this.entries = entries
        }
    }
}

/**
 * Shop tabs. Two is enough: upstream has five accessory types, but four of
 * them (parachute, shield, autodefense, battery) are all "things that keep
 * you alive" and are already grouped that way in the Defenses panel, so
 * splitting them further would make three of the tabs very short.
 */
private enum class ShopTab(val label: String) {
    WEAPONS("Weapons"),
    DEFENSES("Defenses");

    // Upstream's own <tabgroup>, not the accessory type: Fuel and Rocket
    // Fuel are weapons that upstream files under defense, and they belong
    // there - you buy them for the same reason you buy a shield, and you
    // never pick one when choosing what to shoot with.
    fun matches(entry: WeaponShopEntry): Boolean =
        if (this == WEAPONS) entry.tabGroup == "weapon" else entry.tabGroup != "weapon"
}

@Composable
private fun ShopContent(dialog: HudDialog.Shop) {
    var tab by remember { mutableStateOf(ShopTab.WEAPONS) }
    val visible = dialog.entries.filter { tab.matches(it) }
    val listState = rememberLazyListState()

    Column {
        TabRow(selectedTabIndex = tab.ordinal) {
            ShopTab.entries.forEach { candidate ->
                Tab(
                    selected = tab == candidate,
                    onClick = { tab = candidate },
                    text = { Text(candidate.label) },
                )
            }
        }
        Spacer(Modifier.height(8.dp))

        Box(Modifier.heightIn(max = 400.dp)) {
            LazyColumn(state = listState, modifier = Modifier.padding(end = 10.dp)) {
                items(visible) { entry ->
                    ShopRow(
                        entry = entry,
                        pending = dialog.pending.contains(entry.accessoryId),
                        flashToken = if (dialog.flashAccessoryId == entry.accessoryId) {
                            dialog.flashToken
                        } else {
                            0
                        },
                    ) { dialog.onSelect(entry) }
                }
            }
            ListScrollbar(listState, Modifier.align(Alignment.CenterEnd))
        }
    }
}

/**
 * One accessory. Name on the left, price and owned count right-aligned, so
 * the columns line up down the list instead of the eye having to find the
 * dash in each row - which is what the old single-string rows forced.
 */
@Composable
private fun ShopRow(
    entry: WeaponShopEntry,
    pending: Boolean,
    flashToken: Int,
    onClick: () -> Unit,
) {
    // A purchase needs an acknowledgement the eye catches without being
    // watched for: the row lights up at once and fades over about half a
    // second. Keyed on a token rather than a boolean so buying the same
    // accessory twice in a row flashes twice.
    var lit by remember { mutableStateOf(false) }
    LaunchedEffect(flashToken) {
        if (flashToken != 0) {
            lit = true
            delay(100)
            lit = false
        }
    }
    val flash by animateFloatAsState(
        targetValue = if (lit) 0.5f else 0f,
        animationSpec = tween(durationMillis = if (lit) 0 else 550),
        label = "shopRowFlash",
    )

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.primary.copy(alpha = flash))
            .clickable(onClick = onClick)
            .padding(vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = entry.name,
            style = MaterialTheme.typography.bodyMedium,
            // The current weapon is the one piece of state worth spotting
            // at a glance, so it gets weight rather than a "> " prefix that
            // also shifted every other row's text across.
            fontWeight = if (entry.isCurrentWeapon) FontWeight.Bold else FontWeight.Normal,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f),
        )
        Text(
            // Owned shows the count, unowned shows what it costs - the two
            // never both matter, and showing both is what made the old rows
            // long enough to need eliding. Unlimited is spelled out rather
            // than run through the "x{n}" form, which read as "xunlimited".
            text = when {
                // Says something changed the instant it is tapped, without
                // claiming a count the engine hasn't granted yet.
                pending -> "buying..."
                entry.ownedCount < 0 -> "unlimited"
                entry.ownedCount > 0 -> "x${entry.ownedCount}"
                else -> "$${entry.price}"
            },
            style = MaterialTheme.typography.bodySmall,
            color = if (pending) {
                MaterialTheme.colorScheme.primary
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant
            },
        )
    }
}

/**
 * A minimal scrollbar for a [LazyColumn]. Compose ships no scrollbar of its
 * own, and without one a long shop list gives no clue how much is below the
 * fold. Position and size come from the item index rather than pixel
 * offsets, which is exact here because every row is the same height.
 */
@Composable
private fun ListScrollbar(listState: LazyListState, modifier: Modifier = Modifier) {
    val info = listState.layoutInfo
    val total = info.totalItemsCount
    val onScreen = info.visibleItemsInfo.size
    if (total == 0 || onScreen == 0 || onScreen >= total) return

    val thumbFraction = onScreen.toFloat() / total.toFloat()
    val maxFirstIndex = (total - onScreen).toFloat()
    val scrolled = if (maxFirstIndex > 0f) listState.firstVisibleItemIndex / maxFirstIndex else 0f

    BoxWithConstraints(
        modifier = modifier
            .width(4.dp)
            .fillMaxHeight()
            .clip(RoundedCornerShape(2.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant),
    ) {
        val trackHeight = maxHeight
        val thumbHeight = trackHeight * thumbFraction
        Box(
            Modifier
                .offset(y = (trackHeight - thumbHeight) * scrolled.coerceIn(0f, 1f))
                .width(4.dp)
                .height(thumbHeight)
                .clip(RoundedCornerShape(2.dp))
                .background(MaterialTheme.colorScheme.primary),
        )
    }
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

        is HudDialog.Shop -> AlertDialog(
            onDismissRequest = dialog.onCancel,
            title = { Text("Shop - \$${dialog.money}") },
            text = { ShopContent(dialog) },
            confirmButton = {},
            dismissButton = { TextButton(onClick = dialog.onCancel) { Text("Close") } },
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
