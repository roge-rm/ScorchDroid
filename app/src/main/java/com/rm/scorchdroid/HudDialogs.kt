package com.rm.scorchdroid

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
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
import androidx.compose.material3.HorizontalDivider
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.graphics.Color
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

    /**
     * M6 parity: the score / player list (upstream's SHOW_SCORE_DIALOG).
     * Its own dialog rather than a [ListChoice] because it is a table, not
     * a list of choices - nothing here is tappable, and squeezing six
     * numbers per player into one padded string was unreadable at phone
     * width. [entries] is Compose state so an open dialog keeps up with the
     * round rather than freezing at the moment it was opened.
     */
    class Scores(
        entries: List<PlayerEntry>,
        roundInfo: String,
        chat: List<ChatLine>,
        /** M16: where the avatar images live; see [PlayerEntry.avatar]. */
        val dataRoot: String,
        val onCancel: () -> Unit,
    ) : HudDialog() {
        var entries by mutableStateOf(entries)
        var roundInfo by mutableStateOf(roundInfo)
        var chat by mutableStateOf(chat)
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
 * own, and without one a list that runs past the fold gives no clue there is
 * anything below it - which is exactly how the score table and every one of
 * the choice dialogs read before this was shared with them.
 *
 * Position and size come from the item index rather than pixel offsets. That
 * is exact where every row is the same height, as in the shop and the choice
 * lists, and approximate where a row can wrap to two lines, as a chat line
 * can. Approximate is the right trade for an indicator: it is answering "is
 * there more, and roughly where am I", not driving the scroll.
 *
 * Draws nothing at all when everything already fits, so a short list is not
 * given a full-height thumb to puzzle over.
 *
 * [reverseLayout] must match the list's own, or the thumb runs backwards -
 * the chat log is laid out bottom-up so that it opens on the newest line.
 */
@Composable
private fun ListScrollbar(
    listState: LazyListState,
    modifier: Modifier = Modifier,
    reverseLayout: Boolean = false,
) {
    val info = listState.layoutInfo
    val total = info.totalItemsCount
    val onScreen = info.visibleItemsInfo.size
    if (total == 0 || onScreen == 0 || onScreen >= total) return

    val thumbFraction = onScreen.toFloat() / total.toFloat()
    val maxFirstIndex = (total - onScreen).toFloat()
    val position = if (maxFirstIndex > 0f) listState.firstVisibleItemIndex / maxFirstIndex else 0f
    // firstVisibleItemIndex counts from whichever end the list starts at, so
    // under reverseLayout index 0 is the bottom of the track, not the top.
    val scrolled = if (reverseLayout) 1f - position else position

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

        is HudDialog.Message -> AlertDialog(
            onDismissRequest = dialog.onDismiss,
            text = { Text(dialog.text) },
            confirmButton = { TextButton(onClick = dialog.onDismiss) { Text("OK") } },
        )

        is HudDialog.ListChoice -> AlertDialog(
            onDismissRequest = dialog.onCancel,
            title = { Text(dialog.title) },
            text = {
                val listState = rememberLazyListState()
                Box(Modifier.heightIn(max = 400.dp)) {
                    LazyColumn(state = listState, modifier = Modifier.padding(end = 10.dp)) {
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
                    ListScrollbar(listState, Modifier.align(Alignment.CenterEnd))
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

        is HudDialog.Scores -> AlertDialog(
            onDismissRequest = dialog.onCancel,
            title = { Text(scoresTitle(dialog.roundInfo)) },
            text = { ScoresContent(dialog) },
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

/** "Round 2 of 5 - turn 3 of 10", from getRoundInfo()'s four numbers. */
private fun scoresTitle(roundInfo: String): String {
    val parts = roundInfo.split("|")
    if (parts.size != 4) return "Scores"
    val (round, rounds, turn, turns) = parts
    return "Round $round/$rounds · turn $turn/$turns"
}

/**
 * M17: upstream's team colours and names, from TankColorGenerator. Teams are
 * numbered 1-4 by the engine and it gives each a fixed colour; a tank on a
 * team wears it instead of its own (Tank::getColor), so these are the colours
 * already on the battlefield.
 */
private val TeamNames = listOf("Red", "Blue", "Green", "Yellow")
private val TeamColors = listOf(
    Color(0xFFFF0000), Color(0xFF004DFF), Color(0xFF00FF00), Color(0xFFFFFF00),
)

@Composable
private fun ScoresContent(dialog: HudDialog.Scores) {
    Column(modifier = Modifier.heightIn(max = 460.dp)) {
        // M17: the team totals, which are the score that decides a team game
        // - upstream's own score dialog leads with them for the same reason.
        // Shown only when there are teams: with Teams at 1 every tank is on
        // team 0 and this is an empty row.
        val teamScores = dialog.entries
            .filter { it.team in 1..TeamNames.size }
            .groupBy { it.team }
            .mapValues { (_, players) -> players.sumOf { it.score } }
        if (teamScores.isNotEmpty()) {
            Row(modifier = Modifier.padding(bottom = 6.dp)) {
                teamScores.entries.sortedByDescending { it.value }.forEach { (team, total) ->
                    Text(
                        text = "${TeamNames[team - 1]} $total",
                        color = TeamColors[team - 1],
                        style = MaterialTheme.typography.titleSmall,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier.padding(end = 12.dp),
                    )
                }
            }
            HorizontalDivider()
            Spacer(Modifier.height(4.dp))
        }
        Row(modifier = Modifier.padding(bottom = 4.dp)) {
            ScoreCell("Player", weight = 3f, header = true)
            ScoreCell("Score", weight = 1.2f, header = true)
            ScoreCell("Kills", weight = 1f, header = true)
            ScoreCell("Wins", weight = 1f, header = true)
            ScoreCell("Money", weight = 1.5f, header = true)
        }
        HorizontalDivider()
        val tableState = rememberLazyListState()
        Box(Modifier.weight(1f, fill = false)) {
            LazyColumn(state = tableState, modifier = Modifier.padding(end = 10.dp)) {
                items(dialog.entries, key = { it.playerId }) { entry ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.padding(vertical = 3.dp),
                    ) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.weight(3f).padding(end = 6.dp),
                        ) {
                            // M16: the player's avatar, which is where upstream
                            // shows one too. Every bot has the computer face and
                            // a human has whichever they chose, so the column is
                            // either full or - if a player picked none - has a
                            // gap the colour dot beside it still fills.
                            val avatar = rememberAvatarBitmap(dialog.dataRoot, entry.avatar)
                            if (avatar != null) {
                                Image(
                                    bitmap = avatar,
                                    contentDescription = null,
                                    modifier = Modifier
                                        .size(18.dp)
                                        .clip(RoundedCornerShape(3.dp)),
                                )
                                Spacer(Modifier.width(5.dp))
                            }
                            // The tank's own engine colour, so a row can be
                            // matched to a tank on the battlefield at a glance -
                            // the name plates use the same one.
                            Box(
                                modifier = Modifier
                                    .size(10.dp)
                                    .background(Color(entry.colorArgb), CircleShape),
                            )
                            Spacer(Modifier.width(6.dp))
                            Text(
                                // No "(bot)" suffix: the engine already
                                // prefixes an AI's name with "(Bot) ", so adding
                                // one produced "(Bot) Fred (bot)".
                                text = entry.name,
                                style = MaterialTheme.typography.bodySmall,
                                fontWeight = if (entry.isMe) FontWeight.Bold else FontWeight.Normal,
                                // A dead player is still in the round and still
                                // scores, so they are dimmed rather than hidden.
                                color = if (entry.alive) {
                                    MaterialTheme.colorScheme.onSurface
                                } else {
                                    MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f)
                                },
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        ScoreCell(entry.score.toString(), weight = 1.2f)
                        ScoreCell(entry.kills.toString(), weight = 1f)
                        ScoreCell(entry.wins.toString(), weight = 1f)
                        ScoreCell("$${entry.money}", weight = 1.5f)
                    }
                }
            }
            ListScrollbar(tableState, Modifier.align(Alignment.CenterEnd))
        }

        // The chat history lives here rather than in a dialog of its own:
        // the HUD stack is transient by design, so this is the only place to
        // catch up on what was said, and it is the same "how is the game
        // going" question the scores answer.
        if (dialog.chat.isNotEmpty()) {
            Spacer(Modifier.height(8.dp))
            HorizontalDivider()
            Text(
                text = "Chat",
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
            )
            // Newest at the bottom and pinned there, like any chat log.
            // reverseLayout lays out from the bottom up with index 0 at the
            // bottom, so the list is passed newest-first: that both starts
            // the view on the latest line and keeps it there as lines
            // arrive. Without it the panel opened on the oldest lines it
            // held - four "Game starting in N seconds..." - and clipped
            // everything anyone had actually said.
            val chatState = rememberLazyListState()
            Box(Modifier.heightIn(max = 140.dp)) {
                LazyColumn(
                    state = chatState,
                    modifier = Modifier.padding(end = 10.dp),
                    reverseLayout = true,
                ) {
                    items(dialog.chat.asReversed(), key = { it.id }) { line ->
                        Text(
                            text = if (line.who.isEmpty()) line.text else "${line.who}: ${line.text}",
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.padding(vertical = 1.dp),
                        )
                    }
                }
                // Matches the list's own reverseLayout, or the thumb sits at
                // the top while the view is pinned to the newest line.
                ListScrollbar(chatState, Modifier.align(Alignment.CenterEnd), reverseLayout = true)
            }
        }
    }
}

@Composable
private fun RowScope.ScoreCell(text: String, weight: Float, header: Boolean = false) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodySmall,
        fontWeight = if (header) FontWeight.Bold else FontWeight.Normal,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier.weight(weight),
    )
}
