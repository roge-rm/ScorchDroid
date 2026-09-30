package com.rm.scorchdroid

import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.isAltPressed
import androidx.compose.ui.input.key.isCtrlPressed
import androidx.compose.ui.input.key.isShiftPressed
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlin.concurrent.Volatile
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The game, as the player drives it: the menus, starting and joining games,
 * the tick loop, aiming and firing, and every dialog the HUD opens. It was
 * MainActivity until the browser build needed the same thing (see
 * docs/web-port-plan.md), so it's the phone's code, moved.
 *
 * What only one platform has stays with that platform, in a subclass: the
 * GL surface and touch handling, and on a phone the radios - Wi-Fi Direct,
 * Bluetooth and LAN discovery, with the permissions and system prompts they
 * need. Those are the abstract and open members here.
 */
abstract class GameController(
    val settings: GameSettings,
    protected val sound: SoundEffects,
) {
    /** Where the engine's calls are made from: the UI thread on both platforms. */
    protected val scope = CoroutineScope(Dispatchers.Main)

    // Which top-level screen is showing. The game is one of these now.
    var appScreen by mutableStateOf(AppScreen.SPLASH)
    var splashStatus by mutableStateOf("Starting...")
    var licenseText by mutableStateOf("")

    // M15: created once the data root exists; state-driven from the tick.
    var music: MusicOutput? = null
    // M21: the landscape's own atmosphere. Reloaded when the landscape
    // changes, which is every round.
    var ambient: AmbientOutput? = null
    protected var lastLandscapeTex = ""
    // The running game's tick loop, so quit-to-menu can stop it. Non-null
    // exactly while a game is running.
    protected var gameJob: Job? = null

    // Whether there is a game surface to ask the renderer about. The renderer
    // is there from the first frame of a game to its end.
    protected var surfaceAttached = false

    // M10: the game-setup screen's state. The options come from the engine
    // (upstream's own entries, ranges and descriptions) rather than being
    // declared here - see GameSetup.h.
    protected var setupOptions by mutableStateOf<List<SetupOption>>(emptyList())
    protected var setupTitle by mutableStateOf("New Game")
    protected var availableMods by mutableStateOf<List<String>>(emptyList())
    // M18: the bots the chosen mod offers, and which one fills the slots.
    // Re-read whenever the mod changes - a mod brings its own AIs.
    protected var availableBots by mutableStateOf<List<BotOption>>(emptyList())
    protected var selectedBots by mutableStateOf<List<String>>(emptyList())
    // M19: the mod's landscapes, and which of them a game may use. An empty
    // selection is upstream's own "all of them".
    protected var availableLandscapes by mutableStateOf<List<String>>(emptyList())
    protected var selectedLandscapes by mutableStateOf<List<String>>(emptyList())
    // M12: non-null exactly while a tutorial game is running.
    protected var tutorial by mutableStateOf<TutorialState?>(null)
    protected var selectedMod by mutableStateOf("none")
    // M14: the ready-made games the installed mods describe in their own
    // modinfo.xml. Read once, after the engine has a data root - the list
    // cannot change while the app is running.
    protected var presets by mutableStateOf<List<GamePreset>>(emptyList())
    // The saves on disk, refreshed on the way into Single Player rather than
    // once at startup: one is written mid-game, and the menu is the next
    // thing the player sees afterwards.
    protected var savedGames by mutableStateOf<List<SavedGame>>(emptyList())
    // M16: where the extracted data lives, so the settings screen can show
    // the avatar images and the score table can show them again.
    protected var dataRootPath by mutableStateOf("")

    // M4: the real Compose HUD's mutable state (see GameHud.kt) - written
    // to directly from the tick loop, touch handlers, and dialogs below,
    // all plain (non-Composable) Kotlin code, so a plain mutable holder is
    // simpler here than threading Compose State through every function
    // that used to take a `statusText: TextView` parameter.
    val hudState = GameHudState()

    // M4: touch-controllable elevation, in degrees, shared by both fire
    // gestures below - mirrors hudState.elevationDegrees (the Slider's
    // displayed value) but kept as a separate @Volatile field since it's
    // read from a background coroutine dispatcher when firing, and Compose
    // State reads/writes are only safe on the main thread.
    @Volatile
    protected var currentElevationDegrees = 45f

    // Slider-based aiming (see the porting plan's "aiming controls
    // direction" note) - angle/power set via the sliders in GameHud.kt,
    // fired explicitly via the Fire button (fireFromSliders()) rather than
    // on gesture release like the battlefield tap/drag. Same
    // mirrors-Compose-state-into-a-@Volatile-field pattern as
    // currentElevationDegrees above, for the same reason.
    @Volatile
    protected var currentAngleDegrees = 0f

    @Volatile
    protected var currentPowerFraction = DEFAULT_POWER_FRACTION

    // M6 parity: upstream's UNDO_MOVE ("Revert to last angles") - the
    // angle/elevation/power of the last shot actually fired, so a player
    // can get back to it after nudging the sliders around. Null until
    // something has been fired this session.
    protected var lastFiredAim: Triple<Float, Float, Float>? = null

    // Whether the game being started hosts over Bluetooth instead of the
    // network. Not a setting: the engine has one network interface, so this
    // is chosen on the way in and cannot change while a game is running.
    protected var hostOverBluetooth = false

    // Whether this game is meant for anyone else to join.
    //
    // Every game on this port is a hosted game - there is one engine and it
    // always runs the server - but that is an implementation fact, not
    // something a solo player has any use for. With this false nothing is
    // published: no NSD registration, no Wi-Fi Direct group, no line of HUD
    // naming an address, and none of the toasts that report how those two
    // went. The server socket is still open (stopping it would mean a
    // second startup path through the engine, for no gain), so a game can
    // still be joined by someone told the address by hand - it just isn't
    // announced to the room.
    //
    // Chosen on the way in like [hostOverBluetooth], and false unless one of
    // the two Multiplayer host entries set it.
    protected var hostForOthers = false

    // The move id this turn's committed move was submitted against, or 0 if
    // nothing is committed. The Fire button's locked state hangs off it - see
    // the tick loop, which cannot use "there is a move id" on its own.
    // Which save the next started game comes from, or null for a fresh one.
    // Read by startAsHost, which is the one place either kind of game begins.
    protected var savedGameToLoad: String? = null

    // Which screen the save picker was opened from, which is also the
    // question of intent: a game loaded from Single Player publishes
    // nothing, one loaded from Multiplayer hosts like any other host.
    protected var loadGameForOthers = false
    protected var lockedMoveId = 0
    // Skip All Moves: which move the countdown belongs to, and when it ends.
    // See GameHudState.skipAllMoves for why the mode itself is not engine state.
    // A1: which projectile engine loops this side has started, so the ones
    // that land can be stopped again. Keys are the renderer's.
    protected val projectileLoopKeys = HashSet<String>()

    // The doppler rate each of those was last given, for the line that says
    // what it ended on - see updateProjectileLoops.
    protected val projectileLoopRates = HashMap<String, Float>()
    protected var skipAllMoveId = 0
    protected var skipAllDeadlineMs = 0L

    // M6: whether the aiming sliders have been seeded from the tank's real
    // starting turret rotation yet (see the tick loop). One-shot, so it
    // never fights the player's own adjustments afterwards.
    protected var aimSeeded = false
    // M6 parity: chat polling state. The version is the cheap "did anything
    // arrive" check; the line id is how far the HUD has already been told
    // about, so a message it is already timing is never restarted.
    protected var lastChatVersion = 0
    protected var lastChatLineId = 0
    protected var lastMapLineVersion = 0
    protected var lastMapLineId = 0

    /**
     * M12: starts the tutorial - upstream's own easy-game configuration with
     * this port's own coach marks over it.
     *
     * No setup screen: the whole point is a game that needs no decisions
     * first. The preset is loaded rather than merged, so a player who has been
     * fiddling with rounds and wall types still gets the gentle version.
     */
    protected fun startTutorial() {
        val loaded = NativeBridge.loadSetupPreset("data/singletutorial.xml")
        if (!loaded) {
            // Nothing was changed, so a normal game would start instead - with
            // tutorial text over it, which would be worse than saying so.
            hudState.dialog = HudDialog.Message("Couldn't load the tutorial settings.") {
                hudState.dialog = HudDialog.None
            }
            return
        }
        tutorial = TutorialState()
        startGame()
    }

    /**
     * M18/M19: the parts of the setup screen that are not plain options - the
     * bots and the landscapes. Both come from the chosen mod, so they are
     * re-read whenever the mod changes as well as when the screen opens.
     */
    protected fun readPlayersAndMaps() {
        availableBots = parseBots(NativeBridge.getBots())
        selectedBots = NativeBridge.getBotTypes().toList()
        availableLandscapes = NativeBridge.getLandscapes().toList()
        selectedLandscapes = NativeBridge.getSelectedLandscapes().toList()
    }

    protected fun openQuickGame() {
        appScreen = AppScreen.QUICK_GAME
    }

    /**
     * M14: starts one of the mods' own ready-made games.
     *
     * Like the tutorial, and for the same reason: the preset *replaces* the
     * setup rather than merging into it, so "Easy Game" is upstream's easy
     * game and not upstream's easy game plus whatever was last fiddled with in
     * New Game. The mod comes with it - a mod's preset file names the mod
     * itself, which is why picking an Apocalypse game needs no separate mod
     * choice - and startGame() writes the session config from that, so the
     * server loads the right mod before it reads anything else.
     */
    protected fun startPreset(preset: GamePreset) {
        if (!NativeBridge.loadSetupPreset(preset.gameFile)) {
            hudState.dialog = HudDialog.Message("Couldn't load \"${preset.name}\".") {
                hudState.dialog = HudDialog.None
            }
            return
        }
        startGame()
    }

    /**
     * M10: opens the pre-game setup screen. Both New Game and Host Game land
     * here - they differ in wording, not in what they configure, because a
     * single-player game on this port *is* a hosted game that nobody joined.
     */
    protected fun openSetup(
        title: String,
        overBluetooth: Boolean = false,
        forOthers: Boolean = false,
    ) {
        // Stated here rather than left over from whichever button was last
        // pressed: a player who backed out of a Bluetooth game and then
        // started a solo one would otherwise have hosted it over Bluetooth,
        // or advertised a solo game to the room.
        hostOverBluetooth = overBluetooth
        hostForOthers = forOthers
        resetHostingPrompts()
        // Back to the shipped config: a player who ran the tutorial and then
        // started a real game would otherwise inherit its seven inert targets
        // and its missing shot clock, with the setup screen showing them as
        // though they had chosen them.
        NativeBridge.resetSetupOptions()
        setupTitle = title
        setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
        // The mod reaches the server through the session config, which is the
        // only route that can work: startServerInternal() loads mod files
        // partway through its own startup, so anything applied after
        // startServer() is far too late for it.
        availableMods = NativeBridge.getAvailableMods().toList()
        selectedMod = NativeBridge.getSelectedMod()
        readPlayersAndMaps()
        appScreen = AppScreen.SETUP
    }

    /**
     * Sends one choice to the engine and re-reads the list.
     *
     * Re-read rather than patched locally: the engine is the authority on
     * whether a value was accepted, and on what the option now reads as. A
     * rejected value (out of upstream's range, or not one of an enum's
     * choices) then simply leaves the control where it was, which is the right
     * behaviour and costs no validation logic here.
     */
    protected fun changeSetupOption(option: SetupOption, value: String) {
        NativeBridge.setSetupOption(option.name, value)
        setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
    }

    /**
     * M9: starts a game and switches to the game screen, creating the GL
     * surface as it goes.
     *
     * A fresh GLSurfaceView per game is deliberate. It gives a fresh EGL
     * context, so nativeOnSurfaceCreated runs and the renderer forgets every
     * build-once cache it holds - terrain, ground texture, models, trees,
     * water, sky. That reset already exists and is already correct, because
     * the minimise/resume bug forced it to be; reusing it is much safer than
     * writing a second "forget everything" path that would need to stay in
     * step with the first.
     */
    /**
     * Deleting a save asks first - it is the one action on these menus that
     * destroys something, and there is no undoing it.
     */
    protected fun confirmDeleteSave(save: SavedGame) {
        hudState.dialog = HudDialog.ListChoice(
            title = "Delete this saved game?",
            items = listOf("Yes, delete it"),
            cancelLabel = "Keep it",
            onSelect = {
                val deleted = NativeBridge.deleteSavedGame(save.name)
                if (!deleted) notifyPlayer("Couldn't delete that save")
                onSavesChanged()
                savedGames = parseSavedGames(NativeBridge.listSavedGames())
                hudState.dialog = HudDialog.None
                // Nothing left to show, so this screen has nothing to be.
                if (savedGames.isEmpty()) {
                    appScreen = if (loadGameForOthers) {
                        AppScreen.MULTIPLAYER
                    } else {
                        AppScreen.SINGLE_PLAYER
                    }
                }
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    /**
     * Resume a save: the same path a fresh game takes - the GL surface, then
     * the host coroutine - with the file named for [startAsHost] to load
     * instead of building a new game.
     */
    protected fun startSavedGame(save: SavedGame) {
        if (gameJob != null) return
        if (!loadGameForOthers) {
            // Solo: the game runs a server as every game here does, and
            // publishes nothing - no service record, no Wi-Fi Direct group.
            savedGameToLoad = save.name
            hostForOthers = false
            hostOverBluetooth = false
            startGame()
            return
        }

        startHostedSave(save)
    }

    /**
     * A save loaded from Multiplayer, to host for others. Where a device has
     * more than one way of hosting, this is where it asks which.
     */
    protected open fun startHostedSave(save: SavedGame) {
        startHostedSavedGame(save, overBluetooth = false)
    }

    /** The hosted half of [startSavedGame], once the transport is settled. */
    protected fun startHostedSavedGame(save: SavedGame, overBluetooth: Boolean) {
        if (gameJob != null) return
        savedGameToLoad = save.name
        hostForOthers = true
        hostOverBluetooth = overBluetooth
        resetHostingPrompts()
        startGame()
    }

    protected fun startGame() {
        if (gameJob != null) return

        // Anything the platform has to ask before hosting, like Android's
        // Bluetooth visibility prompt, which comes back here once answered.
        if (holdStartForHosting()) return

        music?.load(NativeBridge.getSelectedMod())
        applySettingsToHud()
        showGameSurface()
        appScreen = AppScreen.GAME
        gameJob = scope.launch { startAsHost() }
    }

    /**
     * M11: copies the display-side settings into the HUD's own state.
     *
     * Copied rather than read through, so a composition never touches
     * preferences: the HUD reads one object, and this is the single place the
     * two are joined. Called when a game starts, which is the only time they
     * can have changed - the settings screen is not reachable mid-game.
     */
    protected fun applySettingsToHud() {
        hudState.showNamePlates = settings.showNamePlates
        hudState.showHealthBars = settings.showHealthBars
        hudState.showTankArrows = settings.showTankArrows
        hudState.chatToastMillis = settings.chatToastSeconds * 1000L
        hudState.leftHandMode = settings.leftHandMode
        hudState.controlOpacity = settings.controlOpacity
    }

    /** Abandons a join that hasn't connected yet and returns to the menu. */
    protected fun cancelJoinFlow() {
        music?.setState(MusicState.WAIT)
        gameJob?.cancel()
        gameJob = null
        hudState.dialog = HudDialog.None
        NativeBridge.stopGame()
        ambient?.stop()
        lastLandscapeTex = ""
        lockedMoveId = 0
        hudState.reset()
        stopNetworkAdvertising()
        appScreen = AppScreen.MULTIPLAYER
    }

    /**
     * Answers whatever the plate pass has asked for since the last tick.
     * Normally nothing: a name is asked for once, the first frame that draws
     * a plate for it, and again only if the GL context and this process ever
     * disagree about what has been handed over.
     */
    protected fun supplyPlateTexts() {
        if (!surfaceAttached) return
        val missing = GameRenderer.nativeGetMissingPlateTexts()
        if (missing.isEmpty()) return

        for (text in missing) {
            if (text.isEmpty()) continue
            val picture = drawPlateText(text) ?: continue
            GameRenderer.nativeSetPlateText(text, picture.width, picture.height, picture.pixels)
        }
    }

    /**
     * M10: quitting is a long press on the undo button rather than an entry in
     * the overflow menu (rm's call). It keeps its confirmation - it abandons
     * the game outright, with no saving or rejoining - and a long press is
     * hard enough to do by accident that the pairing is safe.
     */
    protected fun confirmQuitToMenu() {
        hudState.dialog = HudDialog.ListChoice(
            title = "Leave this game?",
            items = listOf("Yes, quit to menu"),
            cancelLabel = "Cancel",
            onSelect = {
                hudState.dialog = HudDialog.None
                quitToMenu()
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    /**
     * Finishing the shop, which is also where Auto Defense is spent.
     *
     * Upstream puts one step between the shop and the round: finishing the
     * shop stimulates StimAutoDefense, and AutoDefenseDialog::windowInit then
     * either shows a shield/parachute chooser or passes straight through -
     *
     *     if (haveDefense()) displayCurrent(); else finished();
     *
     * - and its OK applies the choices and sends eFinishedBuy. So this is
     * that step, in that place. Hooking it to the Defences button instead
     * would have handed every player the accessory's benefit for free, since
     * that button is reachable whenever you like; the whole of what 3000
     * buys is this window before the round.
     */
    protected fun finishBuying() {
        scope.launch {
            val owned = withContext(Dispatchers.Default) { NativeBridge.hasAutoDefense() }
            if (!owned) {
                submitMoveAsync(MoveType.FINISHED_BUY)
                return@launch
            }
            showAutoDefense()
        }
    }

    /**
     * The pre-round defence chooser. Upstream offers a shield (or "Shields
     * Off") and parachutes on/off; this offers the shields and parachutes
     * actually owned, each raised by the same useDefense path the mid-round
     * Defences menu uses, so there is one way defences go up rather than two.
     *
     * Leaving it without choosing anything still starts the round - the
     * round is not optional, and upstream's cancel does the same.
     */
    protected fun showAutoDefense() {
        scope.launch {
            val defences = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop()).filter {
                    it.isOwned && it.activationChange != null &&
                        (it.type == AccessoryType.SHIELD || it.type == AccessoryType.PARACHUTE)
                }
            }

            if (defences.isEmpty()) {
                // Owning Auto Defense but no shields or parachutes to raise
                // with it. Nothing to choose, so do not stop for it.
                submitMoveAsync(MoveType.FINISHED_BUY)
                return@launch
            }

            val startRound = "Start the round"
            hudState.dialog = HudDialog.ListChoice(
                title = "Before the round",
                items = defences.map { "${it.name} [${it.type}] x${it.ownedLabel}" } + startRound,
                cancelLabel = "Start the round",
                onSelect = { index ->
                    if (index >= defences.size) {
                        hudState.dialog = HudDialog.None
                        submitMoveAsync(MoveType.FINISHED_BUY)
                        return@ListChoice
                    }
                    val item = defences[index]
                    val change = item.activationChange
                    scope.launch {
                        if (change != null) {
                            val used = withContext(Dispatchers.Default) {
                                NativeBridge.useDefense(item.accessoryId, change)
                            }
                            if (!used) notifyPlayer("Couldn't activate ${item.name}")
                        }
                        // Straight back to the list: upstream's dialog lets a
                        // player set a shield *and* parachutes before going
                        // on, so one choice must not end the step.
                        showAutoDefense()
                    }
                },
                onCancel = {
                    hudState.dialog = HudDialog.None
                    submitMoveAsync(MoveType.FINISHED_BUY)
                },
            )
        }
    }

    /**
     * The turret servo sounds, following a drag on one of the aiming
     * controls - upstream's TankKeyboardControlUtil, which starts a one-shot
     * movement.wav plus a looping turn/elevate/power source as a key goes
     * down and stops the loop as it comes up.
     *
     * A drag stands in for the held key, which is the one deliberate
     * difference: there is no key here to hold. Power gets no movement.wav,
     * matching upstream - winding up the power is not the turret moving.
     *
     * Paths and gain come from the engine on every start rather than being
     * cached: the paths go through the mod, and the gain is the live distance
     * from the camera to your own tank, which changes as the camera does.
     */
    protected fun playAimSound(axis: AimAxis, active: Boolean) {
        val key = "aim-$axis"
        if (!active) {
            sound.stopLoop(key)
            return
        }

        val sounds = AimSounds.parse(NativeBridge.getAimSounds()) ?: return
        val loop = when (axis) {
            AimAxis.ANGLE -> sounds.turn
            AimAxis.ELEVATION -> sounds.elevate
            AimAxis.POWER -> sounds.power
        }
        if (axis != AimAxis.POWER) {
            sound.play(sounds.movement, sounds.gain, sounds.priority)
        }
        sound.startLoop(key, loop, sounds.gain, sounds.priority)
    }

    /**
     * The host's admin controls - upstream's AdminDialog, which lived in
     * src/client and so was never ported with the rest of it.
     *
     * Nothing here is new engine work: ServerAdminCommon has run in this
     * build all along (see engine_jni.cpp's adminCommand), there was simply
     * no way to reach it. The commands and the split below are upstream's
     * own - its dialog offers exactly kick, ban, mute, unmute, poor and slap
     * against a chosen player - plus the whole-game ones ServerAdminCommon
     * exposes and upstream drives from its console instead.
     *
     * Two levels rather than one flat list: most of these need a player, and
     * a single list of "kick Bob / kick Alice / ban Bob / ban Alice" grows
     * with the square of the room.
     */
    protected fun showAdminMenu() {
        val gameActions = listOf(
            "New game" to AdminCommand.NEW_GAME,
            "Kill all tanks" to AdminCommand.KILL_ALL,
            "Add a bot" to AdminCommand.ADD_BOT,
        )
        val playerRow = "Player actions..."

        hudState.dialog = HudDialog.ListChoice(
            title = "Admin",
            items = gameActions.map { it.first } + playerRow,
            onSelect = { index ->
                hudState.dialog = HudDialog.None
                if (index < gameActions.size) {
                    runAdminCommand(gameActions[index].second, 0, gameActions[index].first)
                } else {
                    showAdminPlayerPicker()
                }
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    protected fun showAdminPlayerPicker() {
        val players = parsePlayerList(NativeBridge.getPlayerList())
        if (players.isEmpty()) {
            notifyPlayer("Nobody in the game yet")
            return
        }

        hudState.dialog = HudDialog.ListChoice(
            title = "Which player?",
            items = players.map { player ->
                // Which of these is you matters: several commands are
                // perfectly willing to kick or kill the host.
                val tags = listOfNotNull(
                    if (player.isMe) "you" else null,
                    if (player.isBot) "bot" else null,
                    if (!player.alive) "dead" else null,
                )
                if (tags.isEmpty()) player.name else "${player.name} (${tags.joinToString(", ")})"
            },
            onSelect = { index ->
                hudState.dialog = HudDialog.None
                showAdminPlayerActions(players[index])
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    protected fun showAdminPlayerActions(player: PlayerEntry) {
        // Slap is upstream's own 10 life, and Poor takes a player's money -
        // both are its punishments short of removing someone.
        val actions = listOf(
            "Kick" to AdminCommand.KICK,
            "Ban" to AdminCommand.BAN,
            "Mute" to AdminCommand.MUTE,
            "Unmute" to AdminCommand.UNMUTE,
            "Slap (10 life)" to AdminCommand.SLAP,
            "Take their money" to AdminCommand.POOR,
            "Kill" to AdminCommand.KILL,
        )

        hudState.dialog = HudDialog.ListChoice(
            title = player.name,
            items = actions.map { it.first },
            onSelect = { index ->
                hudState.dialog = HudDialog.None
                runAdminCommand(actions[index].second, player.playerId, actions[index].first, player.name)
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    /**
     * Runs the command and says what happened. The answer is worth showing:
     * ServerAdminCommon refuses quietly - a command against a player who has
     * already left just returns false - and these are actions where "did
     * that work?" is a fair question.
     */
    protected fun runAdminCommand(command: Int, playerId: Int, label: String, who: String? = null) {
        val argument = when (command) {
            // Upstream's ban takes a reason, which ends up in the ban list
            // and in the message the banned player sees.
            AdminCommand.BAN -> "Banned by the host"
            // The bot the game was set up with, so one added mid-game plays
            // like the ones already in it rather than at some other skill.
            AdminCommand.ADD_BOT -> selectedBots.firstOrNull() ?: "Moron"
            else -> ""
        }

        scope.launch {
            val accepted = withContext(Dispatchers.Default) {
                NativeBridge.adminCommand(command, playerId, argument)
            }
            val subject = who?.let { "$label - $it" } ?: label
            val message = when {
                accepted -> subject
                // The one refusal with a reason worth giving, because it is
                // a setting rather than a mistake: bot balancing holds the
                // game at a fixed player count and would auto-kick the bot
                // straight back out. See the JNI side.
                command == AdminCommand.ADD_BOT ->
                    "Turn bot balancing off in game setup to add bots"
                else -> "$subject failed"
            }
            notifyPlayer(message)
        }
    }

    /** A message from a menu screen, which has no HUD to put one on. */
    fun showMenuMessage(text: String) {
        hudState.dialog = HudDialog.Message(text) { hudState.dialog = HudDialog.None }
    }

    /**
     * A brief word to the player during a game, for an action that did not
     * happen.
     *
     * A toast rather than [GameHudState.statusText], which is not a channel
     * at all: the tick loop rewrites it from the engine's own status every
     * hundred milliseconds, so anything put there mid-game is gone before it
     * can be read. Chat send failures were reported that way and were
     * therefore never once seen.
     */
    protected abstract fun notifyPlayer(text: String)

    /**
     * M9: end the game and go back to the menu.
     *
     * Order matters. The tick loop is stopped first so nothing is mid-call
     * into the engine when it goes away; then the engine is torn down (see
     * stopGame in engine_jni.cpp, and testServerRestart for the evidence that
     * a second game really can start afterwards); then the GL surface is
     * destroyed, which is what makes the next game's context - and so the
     * renderer's whole cache - genuinely fresh.
     */
    protected fun quitToMenu() {
        gameJob?.cancel()
        gameJob = null
        NativeBridge.stopGame()
        // M21: the landscape is gone, and so is its atmosphere. Cleared as
        // well as stopped, so the next game reloads rather than assuming the
        // same landscape came back.
        ambient?.stop()
        sound.stopAllLoops()
        projectileLoopKeys.clear()
        projectileLoopRates.clear()
        sound.release()
        lastLandscapeTex = ""
        if (surfaceAttached) {
            surfaceAttached = false
            removeGameSurface()
        }
        hudState.reset()
        lockedMoveId = 0
        savedGameToLoad = null
        tutorial = null
        music?.setState(MusicState.WAIT)
        aimSeeded = false
        lastChatVersion = 0
        lastChatLineId = 0
        stopNetworkAdvertising()
        appScreen = AppScreen.MENU
    }

    protected suspend fun CoroutineScope.startAsHost() {
        val save = savedGameToLoad
        hudState.statusText = if (save == null) "Starting local game..." else "Loading saved game..."
        val gameOk = withContext(Dispatchers.Default) {
            // A loaded game is a hosted game in every other respect - the
            // difference is only where the options, the landscape and the
            // players come from, and that is decided inside the engine.
            if (save == null) {
                NativeBridge.startLocalGame(hostOverBluetooth)
            } else {
                NativeBridge.startLoadedGame(save, hostOverBluetooth)
            }
        }
        if (!gameOk) {
            hudState.statusText = "Couldn't start the game (check logcat)"
            return
        }
        // This device owns the game state now, so the admin controls apply -
        // asked of the engine rather than inferred from having taken the
        // host path, since that is the same question adminCommand answers.
        hudState.isHost = withContext(Dispatchers.Default) { NativeBridge.isGameHost() }
        updateHostingLabel()
        runTickLoop()
    }

    // M5 Phase 2: joining side of the host/join choice above. Reuses the
    // same "Find Games" LAN-discovery dialog as the informational one that
    // already existed (see showFindGames doc comment history) - now tapping
    // a result actually connects, and there's a manual host:port entry too
    // for a PC host (or a device not advertising via NSD).
    /**
     * The second half of joining: the connection is open, so pump the
     * handshake through to sJoined and then play. Split from the finding and
     * connecting half (startJoinFlow) because only this part belongs on the
     * game screen.
     */
    protected suspend fun CoroutineScope.awaitJoinAndPlay() {
        // The handshake itself (connect -> auth -> mod-check -> load-level,
        // see ClientContext.hpp) only advances as tickEngine() pumps the
        // network, same as everything else - so this loop has to run
        // (renamed) tickEngine() from the very start, not just once joined.
        while (isActive) {
            withContext(Dispatchers.Default) { NativeBridge.tickEngine() }
            val state = withContext(Dispatchers.Default) { NativeBridge.getClientJoinState() }
            if (state == ClientJoinState.JOINED) break
            if (state == ClientJoinState.FAILED) {
                val reason = withContext(Dispatchers.Default) { NativeBridge.getClientFailureReason() }
                hudState.statusText = "Join failed: $reason"
                return
            }
            hudState.statusText = "Connecting... (state $state)"
            kotlinx.coroutines.delay(100)
        }

        runTickLoop()
    }

    // Drives the real game simulation forward every 100ms, in either role -
    // tickEngine()/getMyStatusLabel()/getCurrentWeaponName() are all
    // mode-agnostic now (see engine_jni.cpp's activeContext()). Host mode's
    // tickEngine() call is what actually advances ServerState (waiting for
    // players -> new level -> buying -> playing); client mode's just pumps
    // ClientContext::tick(). Rendering happens separately, driven by
    // GLSurfaceView's own thread (GameRenderer).
    protected suspend fun CoroutineScope.runTickLoop() {
        // Decode the aiming servo samples before anyone can drag a slider -
        // see sound.preload for why a loop cannot wait for a decode the
        // way a one-shot can. Both roles come through here, once per game.
        withContext(Dispatchers.Default) {
            AimSounds.parse(NativeBridge.getAimSounds())?.let { sounds ->
                sound.preload(
                    listOf(sounds.movement, sounds.turn, sounds.elevate, sounds.power)
                )
            }
        }

        // What is left on this side of the world-anchored drawing - the
        // floating damage numbers, and the plan view's camera arrow - is
        // published by the renderer as a *screen* position, so it goes stale
        // the moment the camera moves. The tick loop below runs ten times a
        // second, which is right for a status line and hopeless for these:
        // at 60fps the scene moved six frames between updates and they
        // stepped after it in visible jumps.
        //
        // So they read on the frame clock instead: [onFrame], which the game
        // screen calls from Compose's own frame loop (see Content), and which
        // does nothing until this loop is running.
        //
        // The name plates used to be read here too, and a frame of lag was
        // the best this route could do - the plate is placed from the MVP of
        // the frame the GL thread finished last. They are drawn in GL now,
        // in the frame they were projected for, which is why they are gone
        // from this loop.
        frameUpdatesOn = true
        try {
            tickUntilCancelled()
        } finally {
            frameUpdatesOn = false
        }
    }

    // Whether [onFrame] has a running game to read from.
    private var frameUpdatesOn = false

    /** Once a frame, while a game's tick loop is running. */
    fun onFrame() {
        if (!frameUpdatesOn) return
        applyHeldKeys()
        hudState.floatingLabels = parseFloatingLabels(GameRenderer.nativeGetFloatingLabels())
        // A1: the shells in flight hum while they fly, and both how
        // loud and which side change as they travel - so this rides
        // the frame clock with the things that are pinned to the
        // world, not the ten-a-second tick.
        updateProjectileLoops()
        // The plan view's camera arrow turns with the camera, so it
        // belongs here for the same reason the plates do. The tanks
        // on that map do not - they move on a turn, not on a frame -
        // and its picture changes a handful of times a round, so both
        // of those stay on the tick loop below.
        if (hudState.miniMapVisible) {
            hudState.miniMapCamera = GameRenderer.nativeCameraPlanInfo()
        }
    }

    private suspend fun CoroutineScope.tickUntilCancelled() {
        while (isActive) {
            withContext(Dispatchers.Default) {
                NativeBridge.tickEngine()
                // M3: SoundAction events queued this tick (see
                // SoundEventQueue.h) - played via Android's own media
                // stack, not vendored OpenAL/OGG (see the porting plan).
                //
                // "path|gain|priority|pan", where the gain is upstream's own
                // inverse-distance attenuation against the live listener and
                // the batch has already been cut to the channel budget - so
                // this loop plays what won a channel, it does not decide.
                for (event in NativeBridge.pollSoundEvents()) {
                    val parts = event.split('|')
                    if (parts.size != 4) continue
                    sound.play(
                        parts[0],
                        parts[1].toFloatOrNull() ?: 1.0f,
                        parts[2].toIntOrNull() ?: SoundEffects.PRIORITY_ACTION,
                        // A4: where it sits across the stereo field, from the
                        // listener's own right vector - see panForPosition.
                        parts[3].toFloatOrNull() ?: 0f,
                    )
                }
            }
            // M5: a plain-language "what's happening / can I fire" label
            // (see getMyStatusLabel()) - replaces the raw ServerState-enum
            // debug string this used to show, which was never meant as a
            // real HUD and left a real player with no way to tell whether
            // they were in a buying phase, a live round, or waiting -
            // "there's no strict turn order, so how do I know when it's my
            // turn to shoot" was the direct report that prompted this.
            // Falls back to the debug string before a tank of ours exists
            // yet (label is "" during connect/buying-roster setup).
            val label = withContext(Dispatchers.Default) { NativeBridge.getMyStatusLabel() }
            val baseStatus = label.ifEmpty {
                withContext(Dispatchers.Default) { NativeBridge.getGameStateDebugString() }
            }
            // M6: how long is left in the current phase. Both timed phases
            // end on a deadline the player otherwise can't see - a buying
            // phase that closes mid-purchase, or a shot clock that expires
            // while you are still nudging the sliders, both just happen.
            // Prefixed to the status rather than given its own line, so the
            // battlefield keeps the space.
            // M6: a granted move id means it is our turn to act again, so
            // whatever we committed last round has been played out. See
            // GameHudState.shotLocked - the Fire button reads this.
            // The mini-map. Nothing here runs unless it is actually on
            // screen: it is off by default, and a map nobody is looking at
            // should not cost a tank sweep and a JNI string every tick.
            //
            // The picture itself is fetched only when the renderer says it
            // changed - a handful of times a round, against the ten times a
            // second this loop runs - so the usual tick copies nothing.
            // The one thing the plate pass cannot do for itself: a picture
            // of each name. Usually an empty array and nothing more - a name
            // is asked for once, when a player first has a plate drawn.
            supplyPlateTexts()

            if (hudState.miniMapVisible) {
                val version = GameRenderer.nativeMiniMapVersion()
                if (version != hudState.miniMapVersion) {
                    hudState.miniMapVersion = version
                    val pixels = withContext(Dispatchers.Default) {
                        GameRenderer.nativeMiniMapImage()
                    }
                    val side = kotlin.math.sqrt(pixels.size.toDouble()).toInt()
                    hudState.miniMapImage = if (side > 0 && side * side == pixels.size) {
                        argbImage(pixels, side, side)
                    } else {
                        // Empty between landscapes, which is the renderer
                        // saying "no map yet" rather than a failure.
                        null
                    }
                    hudState.miniMapInfo = parseMiniMapInfo(
                        withContext(Dispatchers.Default) { NativeBridge.getMiniMapInfo() }
                    )
                }
                hudState.miniMapTanks = parseMiniMapTanks(
                    withContext(Dispatchers.Default) { NativeBridge.getMiniMapTanks() }
                )

                // Lines other players have drawn. The version check keeps
                // this to one cheap int on the overwhelming majority of
                // ticks, the way the chat poll does.
                val mapLineVersion =
                    withContext(Dispatchers.Default) { NativeBridge.getMapLinesVersion() }
                if (mapLineVersion != lastMapLineVersion) {
                    lastMapLineVersion = mapLineVersion
                    val arena = hudState.miniMapInfo
                    val fresh = withContext(Dispatchers.Default) {
                        parseMapLines(NativeBridge.getMapLines(lastMapLineId))
                    }
                    if (fresh.isNotEmpty() && arena != null) {
                        lastMapLineId = fresh.last().id
                        // Stamped on arrival, which is upstream's own rule
                        // (simulateLine stamps with the receiver's clock):
                        // a line fades from when you saw it, not from when
                        // it was drawn on someone else's phone.
                        val now = nowMillis()
                        hudState.mapLines = hudState.mapLines + fresh.map { line ->
                            val (ax, ay) = planFractionToLandscape(line.ax, line.ay, arena)
                            val (bx, by) = planFractionToLandscape(line.bx, line.by, arena)
                            MapLine(ax, ay, bx, by, line.colorArgb, now)
                        }
                    } else if (fresh.isNotEmpty()) {
                        // No arena to convert against yet; drop them rather
                        // than placing them wrongly, and do not advance the
                        // cursor past what was never shown.
                        lastMapLineVersion = 0
                    }
                }
            }

            // M6 parity: the scoreboard between rounds. Upstream puts it up
            // by itself and holds the game there for RoundScoreTime (5s), or
            // ScoreTime (15s) after the last round; before this the port
            // sat through that pause showing the empty battlefield, and the
            // player had to know to open the table by hand. Opened only
            // over an idle HUD - a dialog the player opened themselves is
            // never yanked away - and closed again only if this is the one
            // that opened it.
            val scoreboard = withContext(Dispatchers.Default) { NativeBridge.getScoreboardState() }
            if (scoreboard != 0 && autoScoreDialog == null &&
                hudState.dialog is HudDialog.None) {
                autoScoreDialog = showScores()
            } else if (scoreboard == 0 && autoScoreDialog != null) {
                if (hudState.dialog === autoScoreDialog) hudState.dialog = HudDialog.None
                autoScoreDialog = null
            }

            tutorial?.observe(hudState)

            val moveId = withContext(Dispatchers.Default) { NativeBridge.getMyMoveId() }
            // A move id *different* from the one we committed against - not
            // merely a non-zero one. The id lives on our own tank, and only
            // the server clears it: it is set locally by
            // TankStartMoveSimAction when the move is granted and cleared by
            // TankStopMoveSimAction when the server has the move, which is a
            // round trip away. Testing for non-zero therefore unlocked the
            // button again on the very next tick after firing, and on a
            // client - where that round trip is a real network - the locked
            // state was visible for a frame or two and then gone.
            if (moveId != 0 && moveId != lockedMoveId) {
                hudState.shotLocked = false
                lockedMoveId = 0
            }

            // Skip All Moves, the countdown half. While the mode is on and a
            // shot move of ours is live and uncommitted, five seconds run and
            // then the move is skipped - upstream's own window
            // (SkipAllDialog::simulate waits 5). Driven from this loop rather
            // than the frame clock because it is a move, not a picture, and
            // everything it needs is already here: the move id says whose
            // turn it is, shotLocked says whether we have already answered,
            // and buyingPhase keeps it away from the shop, which upstream's
            // skip does not touch either.
            if (hudState.skipAllMoves && moveId != 0 && !hudState.shotLocked &&
                !hudState.buyingPhase
            ) {
                if (skipAllMoveId != moveId) {
                    skipAllMoveId = moveId
                    skipAllDeadlineMs = nowMillis() + SKIP_ALL_SECONDS * 1000L
                }
                val remaining = skipAllDeadlineMs - nowMillis()
                if (remaining <= 0) {
                    hudState.skipAllSeconds = -1
                    skipAllMoveId = 0
                    submitMoveAsync(MoveType.SKIP)
                } else {
                    // Rounded up, so a fresh countdown reads "5s" rather than
                    // starting at four.
                    hudState.skipAllSeconds = ((remaining + 999) / 1000).toInt()
                }
            } else if (hudState.skipAllSeconds >= 0) {
                // Not our move any more, or the move has been answered - by
                // the countdown itself, or by the player firing anyway.
                hudState.skipAllSeconds = -1
                skipAllMoveId = 0
            }

            val seconds = withContext(Dispatchers.Default) { NativeBridge.getPhaseSecondsRemaining() }
            hudState.statusText = if (seconds >= 0) "${seconds}s | $baseStatus" else baseStatus
            // M6: drives the contextual "done buying" button - it only
            // exists during the buying phase, which is the one time it
            // does anything (see ServerPlayedMoveHandler's eFinishedBuy).
            hudState.buyingPhase = label.startsWith("Buying")
            // M15: upstream's music follows its client state - buying,
            // playing, a shot in flight, the score screen - and these are the
            // same signals the status line is already built from.
            music?.setState(
                when {
                    scoreboard != 0 -> MusicState.SCORE
                    hudState.buyingPhase -> MusicState.BUYING
                    hudState.shotLocked -> MusicState.SHOT
                    else -> MusicState.PLAYING
                }
            )
            // M21: the landscape brings its own atmosphere with it, and a
            // new one arrives every round. The check is a string compare
            // against a value the engine already holds; the XML behind the
            // sounds is only read when it actually changed.
            val tex = withContext(Dispatchers.Default) { NativeBridge.getLandscapeTex() }
            if (tex != lastLandscapeTex) {
                lastLandscapeTex = tex
                val sounds = withContext(Dispatchers.Default) {
                    parseAmbientSounds(NativeBridge.getAmbientSounds())
                }
                ambient?.apply(sounds)
            }
            // M4: keep the weapon-select button's label in sync with
            // the current weapon, in case it changed via the shop
            // dialog or a fresh round's default selection.
            val weaponName = withContext(Dispatchers.Default) {
                NativeBridge.getCurrentWeaponName()
            }
            hudState.weaponLabel = weaponName.ifEmpty { "Weapon" }
            // M6 parity: wind really does perturb shots (see TankLib's
            // windoffsetFB) but nothing ever showed it - upstream has a
            // wind dialog of its own (SHOW_WIND_DIALOG). Rendered as a
            // compass-style bearing to match the angle slider's
            // "clockwise from up" convention (see fireFromSliders).
            val wind = withContext(Dispatchers.Default) { NativeBridge.getWindInfo() }
            hudState.windLabel = formatWindLabel(wind)
            // M6 tank movement: Fuel and friends are used by tapping the
            // ground rather than by aiming, so the HUD needs to know which
            // mode the battlefield tap is in - see handleBattlefieldTap.
            val positionSelect = withContext(Dispatchers.Default) {
                NativeBridge.getPositionSelect()
            }
            hudState.positionSelectWeapon =
                positionSelect.split("|").getOrNull(1).orEmpty()
            // Development perf readout - see GameHudState.perfLabel. Debug
            // builds only: it is a diagnostic that has earned its keep
            // several times over (it is what found the trees not drawing),
            // but it has no business on screen in a release.
            if (isDebugBuild) {
                val stats = GameRenderer.nativeGetFrameStats().split("|")
                val fps = stats.getOrNull(0).orEmpty()
                val calls = stats.getOrNull(1).orEmpty()
                val targets = stats.getOrNull(2).orEmpty()
                hudState.perfLabel = if (fps.isEmpty()) {
                    ""
                } else {
                    "$fps fps | $calls draws | $targets targets"
                }
            }
            // M6: seed the aiming sliders from where the tank is actually
            // pointing, once, as soon as we have a tank - the engine gives
            // every tank a real starting turret rotation, so leaving the
            // sliders at a flat 0 meant the UI disagreed with the tank and
            // the first shot never went where the sliders said.
            if (!aimSeeded) {
                val aim = withContext(Dispatchers.Default) { NativeBridge.getMyAim() }
                if (seedAimFromEngine(aim)) aimSeeded = true
            }
            // M6 parity: the simulation-speed multiplier, shown only when
            // it is not 1x - upstream's SpeedChange draws it on the same
            // condition.
            val speed = withContext(Dispatchers.Default) { NativeBridge.getSimulationSpeed() }
            val speedParts = speed.split("|")
            val num = speedParts.getOrNull(0)?.toIntOrNull() ?: 1
            val den = speedParts.getOrNull(1)?.toIntOrNull() ?: 1
            hudState.speedLabel = when {
                num == den -> ""
                den == 1 -> "Speed: ${num}x"
                else -> "Speed: 1/${den}x"
            }
            // M6 parity: new chat. The version check keeps this to one cheap
            // int most ticks - the strings are only crossed over the JNI
            // boundary when something was actually said.
            val chatVersion = withContext(Dispatchers.Default) { NativeBridge.getChatVersion() }
            if (chatVersion != lastChatVersion) {
                lastChatVersion = chatVersion
                val fresh = withContext(Dispatchers.Default) {
                    parseChatLines(NativeBridge.getChatLines(lastChatLineId))
                }
                if (fresh.isNotEmpty()) {
                    lastChatLineId = fresh.last().id
                    // Each gets its own arrival stamp here, which is what
                    // lets the HUD expire them independently.
                    //
                    // Trimmed to the newest MAX_CHAT_TOASTS, which drops the
                    // oldest the moment the limit is passed rather than
                    // waiting out its timer. takeLast rather than a check on
                    // the existing stack, because one poll can carry a whole
                    // burst on its own - a Death's Head announces every tank
                    // it killed at once.
                    val now = nowMillis()
                    hudState.chatToasts =
                        (hudState.chatToasts + fresh.map { ChatToast(it, now) })
                            .takeLast(MAX_CHAT_TOASTS)
                }
            }
            kotlinx.coroutines.delay(100)
        }
    }

    /**
     * The player-facing compass dial (0 up/north, 90 right/east, clockwise)
     * converted to the bearing the engine takes, which turns the other way:
     * `TankLib::getVelocityVector` fires along `(-sin(xy), cos(xy))`, so
     * engine 0 is north and engine 90 is *west*. A compass and a
     * counter-clockwise bearing are mirror images, hence `360 - d`.
     *
     * This was an identity mapping for a while, and it genuinely measured
     * correct at the time - because the renderer was drawing the whole world
     * mirrored (see worldZFromEngineY in renderer_jni.cpp), which reversed
     * the apparent sweep on screen and cancelled this one. Correcting the
     * renderer's handedness uncovered it: the dial started sweeping the
     * barrel backwards again. Two mirrors cancelling is exactly the trap
     * this port kept falling into, so: this one is derived, and the sweep
     * was then checked on a top-down view.
     */
    protected fun engineAngleFromDial(dialDegrees: Float): Float =
        ((360f - dialDegrees) % 360f + 360f) % 360f

    /** Inverse of [engineAngleFromDial] - a mirror is its own inverse. */
    protected fun dialAngleFromEngine(engineDegrees: Float): Float =
        engineAngleFromDial(engineDegrees)

    // M6: applies "angleDegrees|elevationDegrees|powerFraction" from
    // NativeBridge.getMyAim() to the sliders. Returns whether it applied -
    // false while there's no tank yet, so the caller can keep trying.
    //
    // Angle and elevation are taken from the tank; power is not. The engine
    // starts every tank at full power, which is a poor opening shot and an
    // awkward slider position to nudge down from, so the round opens at
    // DEFAULT_POWER_FRACTION instead - and is pushed straight back to the
    // engine, because the gun and aim sight read TanketShotInfo, not the
    // sliders. Setting the slider alone would put the UI back to claiming a
    // power the tank does not have, which is the exact bug that seeding was
    // introduced to fix.
    protected fun seedAimFromEngine(raw: String): Boolean {
        val parts = raw.split("|")
        val engineAngle = parts.getOrNull(0)?.toFloatOrNull() ?: return false
        val elevation = parts.getOrNull(1)?.toFloatOrNull() ?: return false

        currentAngleDegrees = dialAngleFromEngine(engineAngle)
        currentElevationDegrees = elevation.coerceIn(0f, 90f)
        currentPowerFraction = DEFAULT_POWER_FRACTION
        hudState.angleDegrees = currentAngleDegrees
        hudState.elevationDegrees = currentElevationDegrees
        hudState.powerFraction = currentPowerFraction
        pushAimToEngine()
        return true
    }

    // M6 parity: turns NativeBridge.getWindInfo()'s "speed|angle" into a
    // HUD line with a direction arrow.
    //
    // Wind's own angle convention is already the player-friendly one and,
    // conveniently, the same as the angle slider's: Wind.cpp builds its
    // direction as (sin(angle), cos(angle)), so 0 = up and 90 = right
    // (clockwise from up). That's the opposite rotation direction from the
    // engine's *fire* angle (vx = -sin, vy = cos - see fireFromSliders),
    // so unlike the fire angle this needs no mirroring to display.
    protected fun formatWindLabel(raw: String): String {
        val parts = raw.split("|")
        val speed = parts.getOrNull(0)?.toFloatOrNull() ?: return ""
        val angle = parts.getOrNull(1)?.toFloatOrNull() ?: return ""
        if (speed <= 0.01f) return "Wind: none"
        val arrows = listOf("↑", "↗", "→", "↘", "↓", "↙", "←", "↖")
        // Round to the nearest 45-degree bucket rather than truncating, so
        // e.g. 169 degrees reads as "down" instead of "down-right".
        val arrow = arrows[(((angle + 22.5f) / 45f).toInt() % 8 + 8) % 8]
        return "Wind: ${formatFixed(speed, 1)} $arrow ${formatFixed(angle, 0)}°"
    }

    // Slider-based aiming (see GameHud.kt's angle/power sliders and the
    // Fire button) - fires directly with the slider-set angle/elevation/
    // power, the same NativeBridge.fireWeapon() call the drag gesture uses,
    // just triggered explicitly instead of on gesture release. This is the
    // precise/repeatable path for small between-round adjustments; the
    // battlefield tap/drag gestures above remain as the quick/casual one.
    //
    // The angle slider is deliberately a "clockwise from up" dial for the
    // player (0=forward/up, 90=right, 180=back, 270=left - the usual
    // clock/compass-face reading), while the engine's bearing runs
    // counter-clockwise - see engineAngleFromDial for the conversion.
    protected fun fireFromSliders() {
        // The fire button used to be disabled for these weapons - choosing a
        // spot on the ground *is* the shot, and upstream refuses its fire key
        // for the same reason. It cannot be disabled now that the same button
        // is the only way to reach the weapon list, so the refusal moves here
        // and says so instead of doing nothing.
        if (hudState.positionSelectWeapon.isNotEmpty()) {
            notifyPlayer("Tap the ground to use ${hudState.positionSelectWeapon}")
            return
        }

        val engineAngle = engineAngleFromDial(currentAngleDegrees)
        scope.launch {
            // Read before firing: this is the id the shot is submitted
            // against, and the tick loop needs it to tell "still waiting on
            // this move" from "granted a new one".
            val movedId = withContext(Dispatchers.Default) { NativeBridge.getMyMoveId() }
            val myTankId = withContext(Dispatchers.Default) { NativeBridge.getMyTankId() }
            val fired = myTankId != 0 && withContext(Dispatchers.Default) {
                NativeBridge.fireWeapon(myTankId, engineAngle, currentElevationDegrees, currentPowerFraction)
            }
            if (fired) {
                lockedMoveId = movedId
                // Remember what we actually fired so "revert to last
                // angles" can restore it (see showActionsMenu).
                lastFiredAim = Triple(currentAngleDegrees, currentElevationDegrees, currentPowerFraction)
                // The shot is committed but nothing flies until every player
                // has committed too - the tick loop clears this once the
                // server grants the next move.
                hudState.shotLocked = true
            } else {
                // The most-pressed button in the game, and refusing was
                // indistinguishable from the tap not registering. The engine
                // answers only yes or no (see fireWeapon in engine_jni.cpp),
                // so the reason is worked out from the same two things it
                // checks: whether there is a tank of ours at all, and what
                // the current accessory is.
                val weapon = withContext(Dispatchers.Default) { NativeBridge.getCurrentWeaponName() }
                notifyPlayer(
                    when {
                        myTankId == 0 -> "No tank yet, wait for the round to start"
                        weapon.isEmpty() -> "No weapon selected, pick one from the Shop"
                        else -> "Can't fire $weapon right now"
                    }
                )
            }
        }
    }

    // M6 tap-to-aim - upstream's AUTO_AIM ("Aim at point"), which the
    // control-parity audit flagged as a real binding rather than a
    // convenience. Tapping the ground casts a ray against the terrain (see
    // GameRenderer.nativePickTerrain) and swings the turret to face the
    // hit point, leaving elevation and power alone: it aims *at* a
    // direction, it does not solve the shot for you.
    //
    // The battlefield tap was free - since the 3D camera landed, dragging
    // orbits and a plain tap did nothing at all.
    /**
     * Acknowledges a purchase at once, then reconciles it with the engine.
     *
     * A buy is a queued simulator action, not an immediate one:
     * ServerSimulator only promotes it at a send boundary a couple of
     * fixed-seconds out, so the real count can take several seconds to
     * appear. The shop used to read straight back after sending and so
     * showed the *old* row - during a ~20 second buying phase that reads as
     * "the tap did nothing", and there is no time to close and reopen the
     * shop to find out otherwise.
     *
     * So the row goes to "buying..." and flashes immediately (the money is
     * deducted locally too), and this then polls until the engine confirms.
     * Whatever the engine reports wins in the end, so a purchase the server
     * rejects corrects itself rather than leaving a lie on screen.
     */
    protected suspend fun awaitPurchase(shop: HudDialog.Shop, weapon: WeaponShopEntry) {
        shop.markPending(weapon.accessoryId, weapon.price)

        // ~4s at 120ms. Longer than the couple of seconds a send boundary
        // needs, short enough that a rejected buy doesn't sit as
        // "buying..." for the rest of the phase.
        repeat(34) {
            delay(120)
            val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
            val entries = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
            }
            val updated = entries.firstOrNull { it.accessoryId == weapon.accessoryId }
            if (updated != null && updated.ownedCount != weapon.ownedCount) {
                shop.settle(weapon.accessoryId, money, entries)
                return
            }
        }

        // Never confirmed - show whatever is actually true now.
        val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
        val entries = withContext(Dispatchers.Default) {
            parseWeaponShop(NativeBridge.getWeaponShop())
        }
        shop.settle(weapon.accessoryId, money, entries)
    }

    /**
     * Starts, moves and stops the engine loops for the shells in the air.
     *
     * Upstream holds a looping source per shot and lets OpenAL follow it
     * (MissileActionRenderer). The port's player is keyed rather than
     * handle-based, so the same thing here is a reconciliation: whatever the
     * renderer says is flying gets started or moved, and every loop it no
     * longer names is stopped. Only keys the renderer owns are touched - the
     * aiming servo keeps its own.
     */
    protected fun updateProjectileLoops() {
        val rows = GameRenderer.nativeGetSoundLoops()
        val live = HashSet<String>(rows.size)
        for (row in rows) {
            // The file is a path and can hold anything, so it is parsed from
            // the ends in: key first, then gain, pan and rate off the back.
            val first = row.indexOf('|')
            val rateAt = row.lastIndexOf('|')
            val panAt = if (rateAt > 0) row.lastIndexOf('|', rateAt - 1) else -1
            val gainAt = if (panAt > 0) row.lastIndexOf('|', panAt - 1) else -1
            if (first <= 0 || gainAt <= first) continue

            val key = row.substring(0, first)
            val file = row.substring(first + 1, gainAt)
            val gain = row.substring(gainAt + 1, panAt).toFloatOrNull() ?: continue
            val pan = row.substring(panAt + 1, rateAt).toFloatOrNull() ?: 0f
            val rate = row.substring(rateAt + 1).toFloatOrNull() ?: 1f

            live.add(key)
            projectileLoopRates[key] = rate
            if (key in projectileLoopKeys) {
                sound.updateLoop(key, gain, pan, rate)
            } else {
                sound.startLoop(key, file, gain, SoundEffects.PRIORITY_MISSILE, pan, rate)
                projectileLoopKeys.add(key)
                // Same reason the one-shots log: "is that sound wired" should
                // be answerable from logcat rather than by listening.
                logInfo(
                    "ScorchDroidEngine",
                    "Engine loop: $key ${file.substringAfterLast('/')} rate=$rate",
                )
            }
        }

        if (projectileLoopKeys.isNotEmpty()) {
            val iterator = projectileLoopKeys.iterator()
            while (iterator.hasNext()) {
                val key = iterator.next()
                if (key !in live) {
                    sound.stopLoop(key)
                    // The rate it ended on, beside the one it started at:
                    // two numbers are what show the doppler shift moving
                    // rather than merely being computed once.
                    logInfo(
                        "ScorchDroidEngine",
                        "Engine loop ends: $key rate=${projectileLoopRates.remove(key)}",
                    )
                    iterator.remove()
                }
            }
        }
    }

    /**
     * Upstream's tank tooltip, as a card. The lines and the conditions are
     * TankTip::populate's: the shield only while one is up, the state only
     * when it is not the ordinary one, skill and rank only when the game
     * keeps them. Nothing here is computed on this side - the engine
     * answers, and this lays it out.
     */
    protected fun showTankInfo(playerId: Int) {
        scope.launch {
            val info = withContext(Dispatchers.Default) {
                parseTankInfo(NativeBridge.getTankInfo(playerId))
            } ?: return@launch

            val lines = buildList {
                add("Life: ${info.life}/${info.maxLife}")
                if (info.maxShield > 0) add("Shield: ${info.shield}/${info.maxShield}")
                if (info.state.isNotEmpty()) add("State: ${info.state}")
                add("Lives: ${info.lives}/${info.maxLives}")
                add("Score: ${info.score}")
                if (info.skill > 0) add("Skill: ${info.skill} (${info.startSkill})")
                if (info.rank > 0) add("Rank: ${info.rank}")
            }
            hudState.dialog = HudDialog.ListChoice(
                title = info.name,
                items = lines,
                cancelLabel = "Close",
                // Nothing here is a choice - the rows are the card. Tapping
                // one closes it, which is what a player does to a card they
                // have finished reading.
                onSelect = { hudState.dialog = HudDialog.None },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    /**
     * A tap on the battlefield: normally aims, but with a position-select
     * weapon current (Fuel, Rocket Fuel, Teleport) it *uses* that weapon on
     * the tapped square instead - which for the fuel weapons means driving
     * the tank there. Upstream splits the same way, in
     * TargetCamera::landIntersect.
     *
     * The pick is done once here and handed to whichever branch, rather
     * than in each, so a tap can't ray-cast twice.
     */
    protected fun handleBattlefieldTap(screenX: Float, screenY: Float) {
        // M11: with tap-to-aim off, a tap on the battlefield does nothing and
        // the sliders are the only way to aim. Position-selecting weapons are
        // the exception - Fuel and Teleport have no other way to choose a
        // square, so turning aiming off must not take them with it.
        if (!settings.tapToAim && hudState.positionSelectWeapon.isEmpty()) return
        scope.launch {
            val hit = withContext(Dispatchers.Default) {
                GameRenderer.nativePickTerrain(screenX, screenY)
            }
            val parts = hit.split("|")
            val landscapeX = parts.getOrNull(0)?.toFloatOrNull() ?: return@launch
            val landscapeY = parts.getOrNull(1)?.toFloatOrNull() ?: return@launch

            if (hudState.positionSelectWeapon.isNotEmpty()) {
                val used = withContext(Dispatchers.Default) {
                    NativeBridge.firePositionSelect(landscapeX, landscapeY)
                }
                // Upstream's click handler simply returns when the square is
                // out of reach, which on a touch screen is indistinguishable
                // from a missed tap - so say it instead. The reachable area
                // is painted on the ground either way.
                if (!used) {
                    notifyPlayer("Out of range for ${hudState.positionSelectWeapon}")
                }
                return@launch
            }

            aimAtLandscapePoint(landscapeX, landscapeY)
        }
    }

    /** Swings the turret to face an already-picked landscape point. */
    protected suspend fun aimAtLandscapePoint(landscapeX: Float, landscapeY: Float) {
        val angle = withContext(Dispatchers.Default) {
            NativeBridge.aimAtPoint(landscapeX, landscapeY)
        }
        if (angle < 0f) {
            // Only happens when there is no tank of ours to turn, which from
            // the player's side is a tap on the ground doing nothing.
            notifyPlayer("No tank to aim yet")
            return
        }

        // aimAtPoint has already moved the real turret; this just keeps
        // the dial showing what the tank is actually doing.
        currentAngleDegrees = dialAngleFromEngine(angle)
        hudState.angleDegrees = currentAngleDegrees
    }

    // M6: pushes the current slider values onto "my tank"'s real turret so
    // the rendered gun and the aim sight follow the player's aim live.
    // Without this the sliders were Kotlin-only state handed over at fire
    // time, so the turret (and therefore the sight, which reads
    // TanketShotInfo) never moved until a shot was actually fired - which
    // is exactly how it looked: adjusting angle or elevation changed
    // nothing on screen.
    protected fun pushAimToEngine() {
        scope.launch {
            withContext(Dispatchers.Default) {
                NativeBridge.setAim(
                    engineAngleFromDial(currentAngleDegrees),
                    currentElevationDegrees,
                    currentPowerFraction,
                )
            }
        }
    }

    // M6 parity: fire-and-forget submission of a non-shot move type (see
    // NativeBridge.submitMove). Skip and done-buying are common enough to
    // be their own HUD buttons rather than menu entries.
    protected fun submitMoveAsync(moveType: Int) {
        scope.launch {
            val movedId = withContext(Dispatchers.Default) { NativeBridge.getMyMoveId() }
            val submitted = withContext(Dispatchers.Default) { NativeBridge.submitMove(moveType) }
            // A skip is a committed move for this turn like any other: the
            // player is done, and the round is now waiting on everyone else.
            // The button said so for a shot and not for a skip, which left
            // the one move that looks like nothing happening also looking
            // like it had not registered.
            if (submitted && moveType == MoveType.SKIP) {
                lockedMoveId = movedId
                hudState.shotLocked = true
            } else if (!submitted) {
                // Skip, Resign and done-buying all arrive here, and all three
                // were buttons that could do nothing without saying so.
                notifyPlayer(
                    when (moveType) {
                        MoveType.SKIP -> "Couldn't skip, it might not be your turn"
                        MoveType.RESIGN -> "Couldn't resign right now"
                        else -> "Couldn't finish buying, buying time might be over"
                    }
                )
            }
        }
    }

    // M6 parity: upstream's UNDO_MOVE - puts the sliders back to the aim of
    // the last shot actually fired, for the usual fire/nudge/fire-again
    // loop. No-ops (with a nudge) before anything has been fired.
    protected fun revertToLastAim() {
        val aim = lastFiredAim
        if (aim == null) {
            hudState.statusText = "Nothing to revert yet"
            return
        }
        currentAngleDegrees = aim.first
        currentElevationDegrees = aim.second
        currentPowerFraction = aim.third
        hudState.angleDegrees = aim.first
        hudState.elevationDegrees = aim.second
        hudState.powerFraction = aim.third
        pushAimToEngine()
    }

    // M6 parity: the overflow menu - deliberately only holds things that
    // are genuinely rare. Everything a player reaches for regularly (fire,
    // undo, skip, done-buying, defences, shop, weapon) is a direct button
    // on the HUD instead, so common actions never cost an extra tap.
    // M6 parity: the score / player list (upstream's SHOW_SCORE_DIALOG),
    // with the chat history under it. Every number is read straight off
    // TankScore, which this build already keeps - nothing here is simulated
    // or estimated. Refreshed while open so it tracks the round rather than
    // freezing at the moment it was opened.
    protected fun showScores(): HudDialog.Scores {
        val dialog = HudDialog.Scores(
            entries = emptyList(),
            roundInfo = "",
            chat = emptyList(),
            dataRoot = dataRootPath,
            onCancel = { hudState.dialog = HudDialog.None },
        )
        hudState.dialog = dialog

        scope.launch {
            while (hudState.dialog === dialog) {
                val players = withContext(Dispatchers.Default) {
                    parsePlayerList(NativeBridge.getPlayerList())
                }
                val info = withContext(Dispatchers.Default) { NativeBridge.getRoundInfo() }
                // afterId 0 = the whole log the native store still holds
                // (bounded at 100 lines, see ChatStore.cpp).
                val chat = withContext(Dispatchers.Default) {
                    parseChatLines(NativeBridge.getChatLines(0))
                }
                dialog.entries = players
                dialog.roundInfo = info
                dialog.chat = chat
                delay(1000)
            }
        }
        return dialog
    }

    // Set while the between-rounds scoreboard is on screen because the
    // engine asked for it, so the tick below knows to take it down again -
    // and knows not to touch a score dialog the player opened themselves.
    protected var autoScoreDialog: HudDialog.Scores? = null

    // M6 parity: simulation speed (upstream's SIMULATION_SPEED_* keys).
    // The seven upstream offers, no more: this is upstream's own
    // Simulator::setFast, so the set of speeds is its set, not a range
    // invented here.
    protected fun showSimulationSpeed() {
        val speeds = listOf(
            "1/8 speed" to (1 to 8),
            "1/4 speed" to (1 to 4),
            "1/2 speed" to (1 to 2),
            "Normal speed" to (1 to 1),
            "2x speed" to (2 to 1),
            "4x speed" to (4 to 1),
            "8x speed" to (8 to 1),
        )
        hudState.dialog = HudDialog.ListChoice(
            title = "Game speed",
            items = speeds.map { it.first },
            cancelLabel = "Cancel",
            onSelect = { index ->
                val (numerator, denominator) = speeds[index].second
                scope.launch {
                    val ok = withContext(Dispatchers.Default) {
                        NativeBridge.setSimulationSpeed(numerator, denominator)
                    }
                    // Joined clients follow the host's pace - say so rather
                    // than letting the tap look like it did nothing.
                    if (!ok) hudState.statusText = "Only the host can change the game speed"
                }
                hudState.dialog = HudDialog.None
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // M6 parity: upstream's camera presets (TargetCamera::CamType). The
    // camera *button* stays the quick free/follow toggle - it is the one
    // control reached mid-aim - so the fixed framings live here instead of
    // crowding it. Selecting one is a framing, not a mode lock: a drag drops
    // straight back out of it (see nativeCameraDrag).
    protected fun showCameraPresets() {
        val presets = CameraPreset.entries
        hudState.dialog = HudDialog.ListChoice(
            title = "Camera view",
            items = presets.map { it.label },
            cancelLabel = "Cancel",
            onSelect = { index ->
                GameRenderer.nativeSetCameraPreset(index)
                // Keep the camera button's icon honest - selecting Free or
                // Follow moves the toggle it shows.
                hudState.cameraFollow = presets[index] == CameraPreset.FOLLOW
                hudState.dialog = HudDialog.None
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // Chat send. Off the main thread because it takes the engine mutex, which
    // the simulation tick holds for the duration of a step.
    // Failure is deliberately silent, unlike chat's. There is nothing the
    // player can do about it, the line is already on their own map, and a
    // solo game - where there is nobody to send to at all - would otherwise
    // complain every time anyone drew anything.
    protected fun sendMapLineAsync(ax: Float, ay: Float, bx: Float, by: Float) {
        scope.launch {
            withContext(Dispatchers.Default) { NativeBridge.sendMapLine(ax, ay, bx, by) }
        }
    }

    protected fun sendChatAsync(channel: String, text: String) {
        scope.launch {
            val sent = withContext(Dispatchers.Default) { NativeBridge.sendChat(channel, text) }
            if (!sent) notifyPlayer("Could not send that message")
        }
    }

    protected fun showActionsMenu() {
        // Resigning ends your round, so it keeps a confirmation step rather
        // than firing off a single tap.
        val entries = listOf<Pair<String, () -> Unit>>(
            // Scores/chat, the camera views and the game speed used to be
            // here; each now hangs off a long press on the button it
            // extends - message, camera, and skip-turn respectively. Only
            // genuinely rare things are left behind the overflow.
            (if (hudState.hudHidden) "Show HUD" else "Hide HUD") to {
                hudState.hudHidden = !hudState.hudHidden
                hudState.dialog = HudDialog.None
            },
            // Upstream's third button in the same dialog as Resign
            // (SkipDialog): it skips this move and every later one of yours
            // until it is cancelled. Reversible in one tap from the
            // countdown banner, so unlike Resign it needs no confirmation
            // of its own - but it does skip the move you are on, which is
            // upstream's behaviour and worth being asked about first.
            (if (hudState.skipAllMoves) "Stop skipping turns" else "Skip all turns...") to {
                if (hudState.skipAllMoves) {
                    hudState.skipAllMoves = false
                    hudState.skipAllSeconds = -1
                    hudState.dialog = HudDialog.None
                } else {
                    hudState.dialog = HudDialog.ListChoice(
                        title = "Skip this turn and all the rest?",
                        items = listOf("Yes, skip them"),
                        cancelLabel = "Cancel",
                        onSelect = {
                            hudState.skipAllMoves = true
                            // Upstream skips the current move as it sets the
                            // flag (SkipDialog::buttonDown falls through to
                            // skipShot for both buttons).
                            submitMoveAsync(MoveType.SKIP)
                            hudState.dialog = HudDialog.None
                        },
                        onCancel = { hudState.dialog = HudDialog.None },
                    )
                }
            },
            "Resign round..." to {
                hudState.dialog = HudDialog.ListChoice(
                    title = "Resign this round?",
                    items = listOf("Yes, resign"),
                    cancelLabel = "Cancel",
                    onSelect = {
                        submitMoveAsync(MoveType.RESIGN)
                        hudState.dialog = HudDialog.None
                    },
                    onCancel = { hudState.dialog = HudDialog.None },
                )
            },
        ) + if (!hudState.isHost) {
            // A game you joined cannot be saved: the save is the server's own
            // level message, and the server is someone else's process. That
            // is upstream's rule too - its SaveDialog is only registered when
            // this client is not connected to a server.
            emptyList()
        } else {
            listOf<Pair<String, () -> Unit>>(
                "Save game" to {
                    scope.launch {
                        val saved = withContext(Dispatchers.Default) { NativeBridge.saveGame() }
                        onSavesChanged()
                        notifyPlayer(
                            if (saved.isEmpty()) {
                                "Nothing to save until a round starts"
                            } else {
                                "Saved as $saved"
                            },
                        )
                    }
                    hudState.dialog = HudDialog.None
                },
            )
        }

        hudState.dialog = HudDialog.ListChoice(
            title = "More actions",
            items = entries.map { it.first },
            cancelLabel = "Close",
            onSelect = { index -> entries[index].second() },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    /**
     * Upstream's GiftMoneyDialog: pick someone, pick an amount. Both lists are
     * upstream's own - the recipients are whoever the engine says may be given
     * money (see getGiftTargets), and the amounts are the ladder
     * GiftMoneyDialog offers, cut to what this player actually holds.
     *
     * Whether the gift lands is not decided here: TankGiftSimAction checks the
     * teams, the phase and the money again when it runs, so this is a request,
     * not a transfer.
     */
    protected fun showGiftMoney(targets: List<GiftTarget>, money: Int) {
        hudState.dialog = HudDialog.ListChoice(
            title = "Give money to",
            items = targets.map { "${it.name}  (has \$${it.money})" },
            cancelLabel = "Cancel",
            onSelect = { index ->
                val target = targets[index]
                val amounts = GIFT_AMOUNTS.filter { it <= money }
                if (amounts.isEmpty()) {
                    notifyPlayer("You have nothing to give")
                    hudState.dialog = HudDialog.None
                    return@ListChoice
                }
                hudState.dialog = HudDialog.ListChoice(
                    title = "Give how much to ${target.name}?",
                    items = amounts.map { "\$$it" },
                    cancelLabel = "Cancel",
                    onSelect = { amountIndex ->
                        val amount = amounts[amountIndex]
                        scope.launch {
                            val before = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
                            val sent = withContext(Dispatchers.Default) {
                                NativeBridge.giftMoney(target.playerId, amount)
                            }
                            if (!sent) notifyPlayer("Couldn't send that money")
                            // Back to the shop, which is where this came from,
                            // straight away rather than after the wait below.
                            showWeaponShop()
                            if (!sent) return@launch
                            notifyPlayer("Sent \$$amount to ${target.name}")

                            // A gift is a queued simulator action, like a
                            // purchase: the money does not move on this tick,
                            // and a shop reopened before it lands shows the
                            // player what they had before they gave it away.
                            // Same wait, and the same reason, as awaitPurchase.
                            repeat(34) {
                                delay(120)
                                val now = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
                                if (now != before) {
                                    val entries = withContext(Dispatchers.Default) {
                                        parseWeaponShop(NativeBridge.getWeaponShop())
                                    }
                                    (hudState.dialog as? HudDialog.Shop)?.settle(0, now, entries)
                                    return@launch
                                }
                            }
                        }
                    },
                    onCancel = { hudState.dialog = HudDialog.None },
                )
            },
            onCancel = { hudState.dialog = HudDialog.None },
        )
    }

    // M4 economy, M6 parity: exercises the
    // getMyMoney/getWeaponShop/buyAccessory/selectWeapon JNI surface (see
    // engine_jni.cpp) - lets the human player buy accessories with real
    // AccessoryStore/TankScore state and switch between owned ones.
    // Tapping a row buys one unit if unowned; if already owned, a weapon
    // becomes the current weapon and a defence accessory is activated
    // (see showDefenses() - the Shop is also a reasonable place to use one
    // you just bought). Renders via HudDialog.ListChoice (see
    // HudDialogs.kt).
    //
    // M6: the list now covers all five upstream accessory types, not just
    // weapons - shields/parachutes/batteries were previously unbuyable and
    // unusable (see the getWeaponShop() comment in engine_jni.cpp).
    protected fun showWeaponShop() {
        scope.launch {
            val money = withContext(Dispatchers.Default) { NativeBridge.getMyMoney() }
            val weapons = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
            }
            // Upstream's gift button lives in this dialog and nowhere else -
            // the buying phase is the only time a gift is accepted. An empty
            // list is the engine saying "not now, or nobody", and the button
            // simply does not appear.
            val giftTargets = withContext(Dispatchers.Default) {
                parseGiftTargets(NativeBridge.getGiftTargets())
            }

            hudState.dialog = HudDialog.Shop(
                money = money,
                entries = weapons,
                onGift = if (giftTargets.isEmpty()) {
                    null
                } else {
                    { showGiftMoney(giftTargets, money) }
                },
                onSelect = { weapon ->
                    scope.launch {
                        if (weapon.isOwned) {
                            val change = weapon.activationChange
                            when {
                                // Tapping an owned weapon picks it to fire.
                                weapon.isWeapon -> {
                                    val picked = withContext(Dispatchers.Default) {
                                        NativeBridge.selectWeapon(weapon.accessoryId)
                                    }
                                    if (!picked) notifyPlayer("Couldn't select ${weapon.name}")
                                    hudState.dialog = HudDialog.None
                                }
                                // A shield, parachute or battery goes up.
                                change != null -> {
                                    val used = withContext(Dispatchers.Default) {
                                        NativeBridge.useDefense(weapon.accessoryId, change)
                                    }
                                    if (!used) notifyPlayer("Couldn't activate ${weapon.name}")
                                    hudState.dialog = HudDialog.None
                                }
                                // Owned, but neither fired nor raised - Auto
                                // Defense is the one. This used to fall into
                                // selectWeapon, which set it as the current
                                // weapon and took the game down on the next
                                // Fire; the engine refuses that now, but there
                                // is still nothing useful to do here, so say
                                // what the thing is for instead of going quiet.
                                else -> notifyPlayer("${weapon.name} can't be fired, it works on its own")
                            }
                        } else {
                            // The server only accepts a buy during the
                            // Buying phase (see ServerBuyAccessoryHandler.cpp) -
                            // outside that window it silently no-ops with
                            // nothing but a server-console log line the
                            // player never sees, which looked exactly like
                            // "tapping doesn't do anything". Check first so
                            // there's at least a visible reason why.
                            val status = withContext(Dispatchers.Default) { NativeBridge.getMyStatusLabel() }
                            if (status.startsWith("Buying")) {
                                val sent = withContext(Dispatchers.Default) {
                                    NativeBridge.buyAccessory(weapon.accessoryId, true)
                                }
                                // Stay open and refresh rather than close:
                                // the buying phase is for kitting out, and
                                // reopening the shop after every purchase
                                // (then finding your place in the list
                                // again) was needless work. money and
                                // entries are dialog state precisely so this
                                // can update in place.
                                val shop = hudState.dialog
                                if (sent && shop is HudDialog.Shop) {
                                    awaitPurchase(shop, weapon)
                                }
                            } else {
                                hudState.dialog = HudDialog.Message(
                                    text = "You can only buy weapons between rounds.",
                                    onDismiss = { hudState.dialog = HudDialog.None },
                                )
                            }
                        }
                    }
                },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    // M4: in-game weapon quick-switch, separate from the Shop dialog above
    // (buying vs. selecting are different actions upstream too - see
    // TankAccessorySimAction vs. TanketWeapon::setWeapon). Lists only
    // already-owned weapons (isOwned - a starting weapon is commonly
    // unlimited, ownedCount == -1, not "> 0"; filtering on ">0" excluded it
    // entirely, making even a fresh tank's own default weapon
    // unreachable here), reusing the same getWeaponShop() JNI surface as
    // the shop.
    //
    // M6: filters to weapons specifically now that getWeaponShop() also
    // returns shields/parachutes/batteries - selecting one of those as a
    // "current weapon" is meaningless (they're activated via useDefense -
    // see showDefenses below).
    protected fun showWeaponQuickSelect() {
        scope.launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop()).filter { it.isOwned && it.isWeapon }
            }

            if (owned.isEmpty()) {
                hudState.dialog = HudDialog.Message(
                    text = "No weapons yet, buy some in the Shop first.",
                    onDismiss = { hudState.dialog = HudDialog.None },
                )
                return@launch
            }

            val labels = owned.map { weapon ->
                val marker = if (weapon.isCurrentWeapon) "> " else "  "
                "$marker${weapon.name} (${weapon.ownedLabel})"
            }

            hudState.dialog = HudDialog.ListChoice(
                title = "Select weapon",
                items = labels,
                cancelLabel = "Cancel",
                onSelect = { index ->
                    val weapon = owned[index]
                    scope.launch {
                        val picked = withContext(Dispatchers.Default) {
                            NativeBridge.selectWeapon(weapon.accessoryId)
                        }
                        // The weapon button keeps its old name when this
                        // fails, which is easy to miss in the moment.
                        if (!picked) notifyPlayer("Couldn't select ${weapon.name}")
                    }
                    hudState.dialog = HudDialog.None
                },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    // M6 parity: the defence panel - raise/lower shields, enable/disable
    // parachutes, use a battery to repair. Upstream binds these to keys
    // (ENABLE_SHIELDS/ENABLE_PARACHUTES/USE_BATTERY in data/keys.xml) and
    // routes them through ComsDefenseMessage; ScorchDroid had no path to
    // any of it before M6 (see NativeBridge.useDefense). Lists owned
    // shields/parachutes/batteries plus explicit "down" entries for
    // whatever is currently up.
    protected fun showDefenses() {
        scope.launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
                    .filter { it.isOwned && it.activationChange != null }
            }
            val active = withContext(Dispatchers.Default) { NativeBridge.getActiveDefenses() }
            val activeParts = active.split("|")
            val activeShield = activeParts.getOrNull(0).orEmpty()
            val activeParachute = activeParts.getOrNull(1).orEmpty()

            // Each entry is a label plus the action to run when tapped, so
            // the "turn it off" rows can sit in the same list as the owned
            // accessories without a parallel index-mapping to get wrong.
            val entries = mutableListOf<Pair<String, () -> Unit>>()
            for (item in owned) {
                val change = item.activationChange ?: continue
                val activeMarker = when {
                    item.type == AccessoryType.SHIELD && item.name == activeShield -> "> "
                    item.type == AccessoryType.PARACHUTE && item.name == activeParachute -> "> "
                    else -> "  "
                }
                val verb = if (item.type == AccessoryType.BATTERY) "use" else "activate"
                entries += "$activeMarker${item.name} [${item.type}] x${item.ownedLabel} - $verb" to {
                    scope.launch {
                        // The dialog closes either way, so a refusal used to
                        // look exactly like a shield going up.
                        val used = withContext(Dispatchers.Default) {
                            NativeBridge.useDefense(item.accessoryId, change)
                        }
                        if (!used) notifyPlayer("Couldn't $verb ${item.name}")
                    }
                    hudState.dialog = HudDialog.None
                }
            }
            if (activeShield.isNotEmpty()) {
                entries += "Lower shield ($activeShield)" to {
                    scope.launch {
                        val used = withContext(Dispatchers.Default) {
                            NativeBridge.useDefense(0, DefenseChange.SHIELD_DOWN)
                        }
                        if (!used) notifyPlayer("Couldn't lower the shield")
                    }
                    hudState.dialog = HudDialog.None
                }
            }
            if (activeParachute.isNotEmpty()) {
                entries += "Disable parachutes ($activeParachute)" to {
                    scope.launch {
                        val used = withContext(Dispatchers.Default) {
                            NativeBridge.useDefense(0, DefenseChange.PARACHUTES_DOWN)
                        }
                        if (!used) notifyPlayer("Couldn't disable parachutes")
                    }
                    hudState.dialog = HudDialog.None
                }
            }

            if (entries.isEmpty()) {
                hudState.dialog = HudDialog.Message(
                    text = "No defences yet, buy shields, parachutes or batteries in the Shop.",
                    onDismiss = { hudState.dialog = HudDialog.None },
                )
                return@launch
            }

            hudState.dialog = HudDialog.ListChoice(
                title = "Defences",
                items = entries.map { it.first },
                cancelLabel = "Close",
                onSelect = { index -> entries[index].second() },
                onCancel = { hudState.dialog = HudDialog.None },
            )
        }
    }

    // --- Keyboard ----------------------------------------------------------------
    //
    // Scorched3D's own keys (data/keys.xml) for a browser or a phone with a
    // keyboard, which the player can change (KeyBindings, Settings > Controls). The aiming keys work as TankKeyboardControlUtil does: held,
    // they move the gun 45 degrees and the power 250 (of 1000) a second,
    // four times as fast with Ctrl, a quarter with Shift and a twentieth with
    // both. Everything else is one action per press.

    // Which keys are down, so a held key's repeats don't act twice.
    private val keysDown = HashSet<Key>()
    private var shiftDown = false
    private var ctrlDown = false
    private var lastKeyFrameMillis = 0L
    private val keyAimAxes = HashSet<AimAxis>()

    /**
     * Where the pointer is over the battlefield, in the renderer's pixels, if
     * there's a pointer at all. Upstream's "Aim at point" key aims there.
     */
    protected open val hoverPoint: Pair<Float, Float>? get() = null

    /** A key event on the game screen. True if the game used it. */
    fun onGameKey(event: KeyEvent): Boolean {
        shiftDown = event.isShiftPressed
        ctrlDown = event.isCtrlPressed
        // Typing a chat message, or a dialog is up: the keys are theirs.
        if (hudState.chatComposing || hudState.dialog !is HudDialog.None) {
            keysDown.clear()
            return false
        }
        val keys = settings.keys
        val key = event.key
        if (event.type == KeyEventType.KeyUp) {
            return keysDown.remove(key)
        }
        if (event.type != KeyEventType.KeyDown) return false
        if (keys.isHeldKey(key)) {
            if (keysDown.add(key)) lastKeyFrameMillis = nowMillis()
            return true
        }
        val action = keys.pressAction(key, event.isShiftPressed, event.isCtrlPressed, event.isAltPressed)
            ?: return false
        // Held down, a key repeats. These act once per press, as upstream's do.
        if (!keysDown.add(key)) return true
        onKeyAction(action)
        return true
    }

    private fun onKeyAction(action: KeyAction) {
        when (action) {
            KeyAction.FIRE -> fireFromSliders()
            KeyAction.NEXT_WEAPON -> cycleWeapon(1)
            KeyAction.PREVIOUS_WEAPON -> cycleWeapon(-1)
            KeyAction.UNDO -> revertToLastAim()
            KeyAction.AIM_AT_POINTER -> hoverPoint?.let { (x, y) -> handleBattlefieldTap(x, y) }
            KeyAction.SCORES -> showScores()
            KeyAction.SHIELD -> keyDefense(AccessoryType.SHIELD)
            KeyAction.PARACHUTES -> keyDefense(AccessoryType.PARACHUTE)
            KeyAction.BATTERY -> keyDefense(AccessoryType.BATTERY)
            KeyAction.CAMERA_MENU -> showCameraPresets()
            KeyAction.ACTIONS -> showActionsMenu()
            KeyAction.LEAVE -> confirmQuitToMenu()
            KeyAction.CHAT -> openChat("general")
            KeyAction.TEAM_CHAT -> openChat("team")
            KeyAction.CAMERA_TOP -> keyCamera(CameraPreset.TOP)
            KeyAction.CAMERA_BEHIND -> keyCamera(CameraPreset.BEHIND)
            KeyAction.CAMERA_TANK -> keyCamera(CameraPreset.TANK)
            KeyAction.CAMERA_SHOT -> keyCamera(CameraPreset.SHOT)
            KeyAction.CAMERA_ACTION -> keyCamera(CameraPreset.ACTION)
            KeyAction.SPEED_1 -> keySpeed(1)
            KeyAction.SPEED_2 -> keySpeed(2)
            KeyAction.SPEED_3 -> keySpeed(3)
            KeyAction.SPEED_4 -> keySpeed(4)
            // Held actions are applyHeldKeys'.
            else -> Unit
        }
    }

    /** Moves the gun and the power, and the camera, for the keys being held. */
    private fun applyHeldKeys() {
        if (keysDown.isEmpty() || hudState.chatComposing || hudState.dialog !is HudDialog.None) {
            if (keysDown.isNotEmpty()) keysDown.clear()
            stopKeyAimSounds(emptySet())
            return
        }
        val now = nowMillis()
        // Capped so a long stall (a tab in the background) is not one big jump.
        val seconds = ((now - lastKeyFrameMillis).coerceIn(0L, 250L)) / 1000f
        lastKeyFrameMillis = now
        val rate = when {
            shiftDown && ctrlDown -> 0.05f
            ctrlDown -> 4f
            shiftDown -> 0.25f
            else -> 1f
        } * seconds

        fun held(action: KeyAction) = settings.keys.isHeld(action, keysDown)
        val moving = HashSet<AimAxis>()
        var changed = false

        // Left turns the gun anticlockwise, which is the engine's own
        // direction, and the dial runs the other way (engineAngleFromDial).
        val turn = (if (held(KeyAction.TURN_LEFT)) -1 else 0) + (if (held(KeyAction.TURN_RIGHT)) 1 else 0)
        if (turn != 0) {
            currentAngleDegrees = ((currentAngleDegrees + turn * TURN_PER_SECOND * rate) % 360f + 360f) % 360f
            hudState.angleDegrees = currentAngleDegrees
            moving += AimAxis.ANGLE
            changed = true
        }
        var raise = (if (held(KeyAction.RAISE)) 1 else 0) - (if (held(KeyAction.LOWER)) 1 else 0)
        if (settings.invertUpDownKeys) raise = -raise
        if (raise != 0) {
            currentElevationDegrees = (currentElevationDegrees + raise * TURN_PER_SECOND * rate).coerceIn(0f, 90f)
            hudState.elevationDegrees = currentElevationDegrees
            moving += AimAxis.ELEVATION
            changed = true
        }
        val power = (if (held(KeyAction.POWER_UP)) 1 else 0) -
            (if (held(KeyAction.POWER_DOWN)) 1 else 0)
        if (power != 0) {
            currentPowerFraction = (currentPowerFraction + power * POWER_PER_SECOND * rate).coerceIn(0f, 1f)
            hudState.powerFraction = currentPowerFraction
            moving += AimAxis.POWER
            changed = true
        }
        if (changed) pushAimToEngine()
        for (axis in moving) if (keyAimAxes.add(axis)) playAimSound(axis, true)
        stopKeyAimSounds(moving)

        // The number pad moves the camera, by default, as upstream's does.
        if (surfaceAttached) {
            val dx = (if (held(KeyAction.CAMERA_RIGHT)) 1 else 0) - (if (held(KeyAction.CAMERA_LEFT)) 1 else 0)
            val dy = (if (held(KeyAction.CAMERA_DOWN)) 1 else 0) - (if (held(KeyAction.CAMERA_UP)) 1 else 0)
            if (dx != 0 || dy != 0) {
                GameRenderer.nativeCameraDrag(dx * CAMERA_PIXELS_PER_SECOND * seconds, dy * CAMERA_PIXELS_PER_SECOND * seconds)
            }
            val zoom = (if (held(KeyAction.ZOOM_IN)) 1 else 0) - (if (held(KeyAction.ZOOM_OUT)) 1 else 0)
            if (zoom != 0) GameRenderer.nativeCameraZoom(1f + zoom * seconds)
        }
    }

    private fun stopKeyAimSounds(still: Set<AimAxis>) {
        val stopped = keyAimAxes.filter { it !in still }
        for (axis in stopped) {
            keyAimAxes.remove(axis)
            playAimSound(axis, false)
        }
    }

    /**
     * Next or previous weapon among the ones owned, as upstream's Tab and
     * Shift+Tab do.
     */
    private fun cycleWeapon(step: Int) {
        scope.launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop()).filter { it.isOwned && it.isWeapon }
            }
            if (owned.isEmpty()) return@launch
            val current = owned.indexOfFirst { it.isCurrentWeapon }
            val next = owned[((if (current < 0) 0 else current + step) % owned.size + owned.size) % owned.size]
            val picked = withContext(Dispatchers.Default) { NativeBridge.selectWeapon(next.accessoryId) }
            if (!picked) notifyPlayer("Couldn't select ${next.name}")
        }
    }

    /**
     * Upstream's shield, parachute and battery keys. Each only acts when
     * there's no choice to make: one kind of shield or parachute owned, and
     * a battery only when there's damage for it to repair.
     */
    private fun keyDefense(type: String) {
        scope.launch {
            val owned = withContext(Dispatchers.Default) {
                parseWeaponShop(NativeBridge.getWeaponShop())
                    .filter { it.isOwned && it.type == type && it.activationChange != null }
            }
            val item = when (type) {
                AccessoryType.BATTERY -> {
                    val myId = withContext(Dispatchers.Default) { NativeBridge.getMyTankId() }
                    val info = withContext(Dispatchers.Default) { parseTankInfo(NativeBridge.getTankInfo(myId)) }
                    if (info == null || info.life >= info.maxLife) return@launch
                    owned.firstOrNull()
                }
                else -> owned.singleOrNull()
            } ?: return@launch
            val change = item.activationChange ?: return@launch
            val used = withContext(Dispatchers.Default) { NativeBridge.useDefense(item.accessoryId, change) }
            if (!used) notifyPlayer("Couldn't use ${item.name}")
        }
    }

    private fun keyCamera(preset: CameraPreset) {
        if (!surfaceAttached) return
        GameRenderer.nativeSetCameraPreset(preset.ordinal)
        hudState.cameraFollow = preset == CameraPreset.FOLLOW
    }

    private fun keySpeed(times: Int) {
        scope.launch {
            val ok = withContext(Dispatchers.Default) { NativeBridge.setSimulationSpeed(times, 1) }
            if (!ok) notifyPlayer("Only the host can change the game speed")
        }
    }

    private fun openChat(channel: String) {
        hudState.chatChannel = channel
        hudState.chatComposing = true
    }

    // --- What each platform brings -------------------------------------------

    /** The app's version, and the upstream commit the engine was built from. */
    protected abstract val versionName: String
    protected abstract val upstreamCommit: String

    /** A debug build: shows the frame-rate readout. */
    protected abstract val isDebugBuild: Boolean

    /**
     * Puts a fresh game surface up. A fresh one per game is deliberate: it
     * gives a fresh GL context, so nativeOnSurfaceCreated runs and the
     * renderer forgets every build-once cache it holds - terrain, ground
     * texture, models, trees, water, sky.
     */
    protected abstract fun attachGameSurface()

    /** Takes the game surface down again, at the end of a game. */
    protected abstract fun removeGameSurface()

    private fun showGameSurface() {
        attachGameSurface()
        surfaceAttached = true
    }

    /** The joining half of [showGameSurface], for a platform's join flow. */
    protected fun showJoinedGame() {
        hudState.isHost = false
        applySettingsToHud()
        showGameSurface()
        appScreen = AppScreen.GAME
    }

    /**
     * One name's picture for its plate, white on nothing, as ARGB pixels.
     * The renderer has no font, so the platform draws the text.
     */
    protected abstract fun drawPlateText(text: String): PlateText?

    /** Finds and connects to a game, from the Multiplayer menu. */
    protected abstract fun startJoinFlow(overBluetooth: Boolean = false)

    /** The HUD's "find games" button, mid-game. */
    protected abstract fun onFindGames()

    /** The Multiplayer menu. What it offers depends on what the device can do. */
    @Composable
    protected abstract fun MultiplayerMenu()

    /**
     * What goes under the HUD on the game screen, over the game itself. On a
     * phone that's nothing, since the GL surface takes its own touches; in a
     * browser it's where the pointer is read.
     */
    @Composable
    protected open fun Battlefield() {}

    /** On the way into the Multiplayer menu. */
    protected open fun onEnterMultiplayer() {}

    /** A new hosted game is being set up; any one-shot hosting prompts may ask again. */
    protected open fun resetHostingPrompts() {}

    /**
     * Anything to ask before hosting starts. True means it's asking, and will
     * call [startGame] again once it has an answer.
     */
    protected open fun holdStartForHosting(): Boolean = false

    /** A save was written or deleted. A browser keeps them in its own storage. */
    protected open fun onSavesChanged() {}

    /** Stops telling other devices about a game that has ended. */
    protected open fun stopNetworkAdvertising() {}

    /**
     * The HUD's hosting line, and telling other devices about the game. A solo
     * game announces nothing and says nothing about announcing.
     */
    protected open fun updateHostingLabel() {
        hudState.hostingLabel = ""
    }

    // --- Startup ----------------------------------------------------------------

    /** Once the engine has its data root: the menu is ready to show. */
    fun onEngineReady(dataRoot: String, music: MusicOutput?, ambient: AmbientOutput?) {
        dataRootPath = dataRoot
        this.music = music?.also {
            it.load(NativeBridge.getSelectedMod())
            settings.music = it
        }
        this.ambient = ambient?.also { settings.ambient = it }
        settings.applyAll()
        presets = parsePresets(NativeBridge.getPresets())
        // Upstream's menu music is its "wait" loop.
        music?.setState(MusicState.WAIT)
        appScreen = AppScreen.MENU
    }

    // Whether Settings is showing its key list rather than its tabs.
    private var editingKeys by mutableStateOf(false)

    /** Whether going back means anything on this screen. */
    val backEnabled: Boolean get() = appScreen != AppScreen.MENU

    /**
     * M9: the system back gesture walks the menu back up a level. In a game
     * it does nothing: quitting is a confirmed action behind the overflow
     * menu, and a stray back swipe mid-round should never throw the game away.
     */
    fun onBack() {
        when (appScreen) {
            AppScreen.GAME -> Unit
            // The key list is a page inside Settings; back leaves it first.
            AppScreen.SETTINGS -> if (editingKeys) editingKeys = false else appScreen = AppScreen.MENU
            // The only screen two levels down; back should undo one
            // step, not both.
            AppScreen.QUICK_GAME -> appScreen = AppScreen.SINGLE_PLAYER
            AppScreen.LOAD_GAME -> appScreen =
                if (loadGameForOthers) AppScreen.MULTIPLAYER else AppScreen.SINGLE_PLAYER
            else -> appScreen = AppScreen.MENU
        }
    }

    // --- The screens ------------------------------------------------------------

    @Composable
    fun Content() {
        when (appScreen) {
            AppScreen.SPLASH -> SplashScreen(splashStatus, null)
            AppScreen.MENU -> MainMenuScreen(
                onSinglePlayer = {
                    savedGames = parseSavedGames(NativeBridge.listSavedGames())
                    appScreen = AppScreen.SINGLE_PLAYER
                },
                onMultiplayer = {
                    onEnterMultiplayer()
                    // Read on the way in, not on the tap: whether Load
                    // Game is even offered depends on there being a
                    // save, and a game saved a minute ago was written
                    // after this list was last read.
                    savedGames = parseSavedGames(NativeBridge.listSavedGames())
                    appScreen = AppScreen.MULTIPLAYER
                },
                onSettings = { appScreen = AppScreen.SETTINGS },
                onAbout = { appScreen = AppScreen.ABOUT },
            )
            AppScreen.SINGLE_PLAYER -> SinglePlayerScreen(
                onQuickGame = { openQuickGame() },
                quickGameEnabled = presets.isNotEmpty(),
                onNewGame = { openSetup("New Game") },
                onLoadGame = {
                    loadGameForOthers = false
                    appScreen = AppScreen.LOAD_GAME
                },
                loadGameEnabled = savedGames.isNotEmpty(),
                onTutorial = { startTutorial() },
                tutorialEnabled = true,
                onBack = { appScreen = AppScreen.MENU },
            )
            AppScreen.LOAD_GAME -> LoadGameScreen(
                saves = savedGames,
                onPick = { startSavedGame(it) },
                onDelete = { confirmDeleteSave(it) },
                onBack = {
                    appScreen = if (loadGameForOthers) {
                        AppScreen.MULTIPLAYER
                    } else {
                        AppScreen.SINGLE_PLAYER
                    }
                },
            )
            AppScreen.QUICK_GAME -> QuickGameScreen(
                presets = presets,
                onPick = { startPreset(it) },
                onBack = { appScreen = AppScreen.SINGLE_PLAYER },
            )
            AppScreen.MULTIPLAYER -> MultiplayerMenu()
            AppScreen.SETUP -> GameSetupScreen(
                title = setupTitle,
                options = setupOptions,
                mods = availableMods,
                selectedMod = selectedMod,
                onModChange = {
                    NativeBridge.setSelectedMod(it)
                    selectedMod = NativeBridge.getSelectedMod()
                    // The new mod's bots, which are rarely the same ones.
                    readPlayersAndMaps()
                },
                bots = availableBots,
                selectedBots = selectedBots,
                onBotsChange = {
                    NativeBridge.setBotTypes(it.toTypedArray())
                    selectedBots = NativeBridge.getBotTypes().toList()
                },
                landscapes = availableLandscapes,
                selectedLandscapes = selectedLandscapes,
                onLandscapesChange = {
                    NativeBridge.setLandscapes(it.toTypedArray())
                    selectedLandscapes = NativeBridge.getSelectedLandscapes().toList()
                },
                onChange = { option, value -> changeSetupOption(option, value) },
                onReset = {
                    NativeBridge.resetSetupOptions()
                    setupOptions = parseSetupOptions(NativeBridge.getSetupOptions())
                    selectedMod = NativeBridge.getSelectedMod()
                    readPlayersAndMaps()
                },
                onStart = { startGame() },
                onBack = { appScreen = AppScreen.MENU },
            )
            AppScreen.JOINING -> JoiningScreen(
                status = hudState.statusText,
                dialog = hudState.dialog,
                onBack = { cancelJoinFlow() },
            )
            AppScreen.SETTINGS -> SettingsScreen(
                settings = settings,
                dataRoot = dataRootPath,
                onBack = { appScreen = AppScreen.MENU },
                editingKeys = editingKeys,
                onEditKeys = { editingKeys = it },
            )
            AppScreen.ABOUT -> AboutScreen(
                versionName = versionName,
                upstreamCommit = upstreamCommit,
                licenseText = licenseText,
                onBack = { appScreen = AppScreen.MENU },
            )
            AppScreen.GAME -> {
                // The frame clock the world-anchored bits of the HUD ride on
                // (see onFrame), which a composition always has.
                LaunchedEffect(Unit) {
                    while (true) {
                        withFrameNanos { }
                        onFrame()
                    }
                }
                // The keys go to the game screen as a whole, ahead of whatever
                // button or slider was last touched, and it takes focus back
                // whenever a dialog or the chat box lets go of it.
                val keyFocus = remember { FocusRequester() }
                LaunchedEffect(hudState.dialog, hudState.chatComposing) {
                    if (hudState.dialog is HudDialog.None && !hudState.chatComposing) {
                        runCatching { keyFocus.requestFocus() }
                    }
                }
                Box(
                    Modifier.fillMaxSize()
                        .onPreviewKeyEvent { onGameKey(it) }
                        .focusRequester(keyFocus)
                        .focusable(),
                ) {
                Battlefield()
                GameHud(
                    state = hudState,
                    onFindGames = { onFindGames() },
                    onShop = { showWeaponShop() },
                    onWeapon = { showWeaponQuickSelect() },
                    onElevationChange = { degrees ->
                        currentElevationDegrees = degrees
                        hudState.elevationDegrees = degrees
                        pushAimToEngine()
                    },
                    onAngleChange = { degrees ->
                        currentAngleDegrees = degrees
                        hudState.angleDegrees = degrees
                        pushAimToEngine()
                    },
                    onPowerChange = { power ->
                        currentPowerFraction = power
                        hudState.powerFraction = power
                        pushAimToEngine()
                    },
                    onFire = { fireFromSliders() },
                    onToggleCamera = { hudState.cameraFollow = GameRenderer.nativeToggleCameraMode() },
                    onDefenses = { showDefenses() },
                    onActions = { showActionsMenu() },
                    onUndo = { revertToLastAim() },
                    onQuitToMenu = { confirmQuitToMenu() },
                    onSkip = { submitMoveAsync(MoveType.SKIP) },
                    onDoneBuying = { finishBuying() },
                    onScores = { showScores() },
                    onCameraPresets = { showCameraPresets() },
                    onSimulationSpeed = { showSimulationSpeed() },
                    onAdmin = { showAdminMenu() },
                    onAimGesture = { axis, active -> playAimSound(axis, active) },
                    onSendChat = { text -> sendChatAsync(hudState.chatChannel, text) },
                    onLookAt = { x, y ->
                        if (surfaceAttached) GameRenderer.nativeCameraLookAt(x, y)
                    },
                    // Onto the wire as upstream's ComsLinesMessage. The engine
                    // decides who may see it - the sender's team, which in a
                    // free-for-all is everyone - and a solo game simply has
                    // nobody to send to, so this costs a no-op there.
                    onDrawLine = { line ->
                        val arena = hudState.miniMapInfo
                        if (arena != null) {
                            val (ax, ay) = landscapeToPlanFraction(line.ax, line.ay, arena)
                            val (bx, by) = landscapeToPlanFraction(line.bx, line.by, arena)
                            sendMapLineAsync(ax, ay, bx, by)
                        }
                    },
                )
                }
            }
        }
        // A game that fails to load has to be able to say so from the
        // screen it was picked on. HudDialogHost is drawn by the game and
        // joining screens; these three raise dialogs without being any of
        // them - and a dialog raised on a screen that does not draw one is
        // invisible, which is how "Host over Bluetooth" came to do nothing
        // at all when the radio was switched off.
        if (appScreen == AppScreen.SINGLE_PLAYER || appScreen == AppScreen.QUICK_GAME ||
            appScreen == AppScreen.MULTIPLAYER || appScreen == AppScreen.LOAD_GAME
        ) {
            HudDialogHost(hudState.dialog)
        }
        // M12: over the HUD, and only during a tutorial game.
        tutorial?.let { active ->
            if (appScreen == AppScreen.GAME) {
                TutorialOverlay(active) { active.skip() }
            }
        }
    }

    protected companion object {
        // The amounts upstream's GiftMoneyDialog offers, in its own order.
        // Not this port's numbers to pick: they are what a Scorched3D player
        // sees in that dialog.
        val GIFT_AMOUNTS = listOf(1000, 2500, 5000, 10000, 15000, 20000, 25000, 50000, 100000)

        // How long Skip All Moves waits before passing a move, which is
        // upstream's own five seconds (SkipAllDialog::simulate).
        const val SKIP_ALL_SECONDS = 5

        // TankKeyboardControlUtil's rates: 45 degrees and 250 of 1000 power a second.
        const val TURN_PER_SECOND = 45f
        const val POWER_PER_SECOND = 0.25f
        // The number pad's camera, in the same pixels a drag moves it by.
        const val CAMERA_PIXELS_PER_SECOND = 300f

    }
}

/** A plate's text as the renderer takes it: ARGB pixels, one Int each. */
class PlateText(val width: Int, val height: Int, val pixels: IntArray)
