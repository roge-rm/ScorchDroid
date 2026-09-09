package com.rm.scorchdroid

object NativeBridge {
    init {
        System.loadLibrary("scorchdroid_engine")
    }

    external fun helloFromNative(): String

    /** Points the native engine's cwd/$HOME at the extracted data root (see AssetDataExtractor). */
    external fun initEngine(dataRoot: String): Boolean

    /**
     * M2 vertical slice: boots a real local game via
     * ScorchedServer::startServer. [debugBuild] makes the engine start players rich
     * (100000 rather than the config's 10000) so testing need not grind for
     * a nuke or a shield; pass BuildConfig.DEBUG so release is untouched.
     */
    external fun startLocalGame(debugBuild: Boolean): Boolean

    /** Advances the real game simulation by one step (see ServerSimulator). */
    external fun tickEngine()

    /** Debug-only: current ServerState + tank count. */
    external fun getGameStateDebugString(): String

    /**
     * M4: playerId of the local human-controlled tank added by
     * startLocalGame() (see addHumanTank() in engine_jni.cpp), or 0 if it
     * hasn't been added to the target container yet.
     */
    external fun getMyTankId(): Int

    /**
     * M4 drag-to-aim: directly submits a move for the given tank, exactly as
     * a real client's ComsPlayedMoveMessage would - see engine_jni.cpp.
     * angle/elevation are in degrees, power is 0..1. Used by the drag-based
     * aim gesture (see MainActivity.setUpCameraControls) once the player
     * releases.
     */
    external fun fireWeapon(playerId: Int, angleDegrees: Float, elevationDegrees: Float, power: Float): Boolean

    /**
     * M3: drains sound events queued by SoundAction::simulate() since the
     * last call (see SoundEventQueue.h) - each entry is an absolute path to
     * an already-extracted .ogg file, ready to hand to a MediaPlayer.
     */
    external fun pollSoundEvents(): Array<String>

    /** M4 economy: "my tank"'s current money (see TankScore::getMoney), or -1 if not added yet. */
    external fun getMyMoney(): Int

    /**
     * M4 economy, M6 parity: the purchasable accessory list for "my tank",
     * one pipe-delimited row each:
     * "accessoryId|name|price|ownedCount|isCurrentWeapon(0/1)|type"
     * (see engine_jni.cpp) - parsed into [WeaponShopEntry] by
     * NativeBridge.parseWeaponShop. Covers all five upstream accessory types
     * now, not just weapons - see [AccessoryType].
     */
    external fun getWeaponShop(): Array<String>

    /** M4 economy: buys (buy=true) or sells (buy=false) one unit of an accessory for "my tank". */
    external fun buyAccessory(accessoryId: Int, buy: Boolean): Boolean

    /** M4 economy: selects an already-owned accessory as "my tank"'s current weapon. */
    external fun selectWeapon(accessoryId: Int): Boolean

    /**
     * M4 weapon HUD: "my tank"'s current weapon name plus its remaining
     * ammo count in parentheses (e.g. "Baby Missile (5)"), no parentheses
     * if unlimited - or "" if not set/the tank hasn't been added yet. A
     * lighter-weight query than [getWeaponShop] for polling every tick.
     */
    external fun getCurrentWeaponName(): String

    /**
     * M5: a plain-language status line for "what phase is this round in,
     * and can I act right now" - ScorchDroid's own config has no strict
     * turn order (everyone can fire simultaneously once live), so this
     * answers "can I fire" rather than "whose turn is it". "" if this
     * process has no tank of its own yet.
     */
    external fun getMyStatusLabel(): String

    /**
     * M5: whether startLocalGame() successfully bound a real listening
     * socket (see NetServerTCP3::start() in engine_jni.cpp) - false means
     * this device is only playable solo (e.g. the port was already in use),
     * not joinable over the network.
     */
    external fun isHostingOnNetwork(): Boolean

    /** M5: the port passed to NetInterface::start() - see isHostingOnNetwork. */
    external fun getServerPort(): Int

    /**
     * M5 Phase 2: starts joining a game hosted elsewhere on the LAN instead
     * of hosting one locally - mutually exclusive with [startLocalGame]
     * (the engine picks one role per process, see engine_jni.cpp). Only
     * opens the socket and starts the handshake; poll [getClientJoinState]
     * to see how it's progressing.
     */
    external fun startJoinGame(host: String, port: Int): Boolean

    /**
     * M5 Phase 2: the joining client's handshake progress - one of
     * ClientContext::State's ordinals (see ClientContext.hpp: 0=idle,
     * 1=connecting, 2=waitingAuthChallenge, 3=waitingConnectAccept,
     * 4=waitingLoadLevel, 5=joined, 6=failed), or -1 if this process isn't
     * in client mode at all.
     */
    external fun getClientJoinState(): Int

    /** M5 Phase 2: why the join failed - only meaningful once [getClientJoinState] reports failed (6). */
    external fun getClientFailureReason(): String

    /**
     * M6 parity: activates a defense accessory for "my tank" - see
     * [DefenseChange] for the change codes. [accessoryId] says which
     * shield/parachute/battery to act on (pass 0 for the *_DOWN cases).
     * Returns whether the request was queued/sent; the engine re-validates
     * it when simulated, so an impossible request is a silent no-op.
     */
    external fun useDefense(accessoryId: Int, change: Int): Boolean

    /**
     * M6 parity: "my tank"'s currently-active defenses as
     * "shieldName|parachuteName", either side empty if none is up.
     */
    external fun getActiveDefenses(): String

    /**
     * M6 parity: current wind as "speed|angleDegrees", or "" if there's no
     * running game yet. Wind really does perturb shots (see TankLib's
     * windoffsetFB), so the HUD shows it - upstream has a wind dialog too.
     */
    external fun getWindInfo(): String

    /**
     * M6 parity: submits a non-shot player move - see [MoveType]. Before
     * this, only shots could be submitted, so a player could neither skip,
     * resign, nor finish buying early; they had to wait out the timers.
     */
    external fun submitMove(moveType: Int): Boolean

    /**
     * M6 HUD: the move id the server has granted "my tank", or 0 if none is
     * outstanding. Zero on a live tank means the shot is locked in and the
     * round is waiting on the other players.
     */
    /**
     * M6 tap-to-aim (upstream's AUTO_AIM): swings "my tank"'s turret to
     * face a landscape point - from GameRenderer.nativePickTerrain - and
     * returns the resulting angle in degrees, or -1 if there is no tank to
     * aim. The angle uses upstream's own autoAim arithmetic.
     */
    external fun aimAtPoint(landscapeX: Float, landscapeY: Float): Float

    external fun getMyMoveId(): Int

    /**
     * M6 HUD: seconds left in the current timed phase (buying, or the shot
     * clock), or -1 where a countdown would be meaningless - no game yet,
     * joined as a client (the host owns the clock and doesn't send it), or
     * a phase that ends on an event rather than a deadline.
     */
    external fun getPhaseSecondsRemaining(): Int

    /**
     * M6: "my tank"'s current aim as
     * "angleDegrees|elevationDegrees|powerFraction", or "" if there's no
     * tank yet. The angle is the engine's counter-clockwise bearing; the
     * player-facing compass dial is its mirror (see
     * MainActivity.dialAngleFromEngine).
     */
    external fun getMyAim(): String

    /**
     * M6 tank movement: the current weapon's position-select mode as
     * "type|weaponName|range", or "" when an ordinary weapon is selected.
     *
     * Non-empty means the weapon is used by choosing a spot on the ground
     * rather than by aiming and firing - upstream's Fuel and Rocket Fuel
     * (which move the tank) and Teleport work this way. [type] is
     * upstream's own name: fuel, fuellimit, limit or generic.
     */
    external fun getPositionSelect(): String

    /**
     * M6 tank movement: uses the current position-select weapon on a
     * landscape point - from GameRenderer.nativePickTerrain. Returns false
     * if that point is out of reach, so the HUD can say so instead of
     * leaving the tap looking ignored.
     */
    external fun firePositionSelect(landscapeX: Float, landscapeY: Float): Boolean

    /**
     * M6: applies the aiming sliders to "my tank"'s real turret state, so
     * the rendered gun and the aim sight follow the player's aim instead of
     * only moving when a shot is fired. [angleDegrees] is the engine's own
     * bearing, not the player dial - convert with
     * MainActivity.engineAngleFromDial first; [power] is 0..1.
     */
    external fun setAim(angleDegrees: Float, elevationDegrees: Float, power: Float): Boolean

    /**
     * M6 parity: the score / player list (upstream's SHOW_SCORE_DIALOG), one
     * pipe-delimited row per player, sorted by score descending. See
     * [parsePlayerList].
     */
    external fun getPlayerList(): Array<String>

    /** "round|totalRounds|turn|totalTurns", the score dialog's own heading. */
    external fun getRoundInfo(): String

    /**
     * Whether the engine has the end-of-round scoreboard up: 0 no, 1 the
     * round score, 2 the final score of the match. Upstream raises this by
     * itself at the end of every round and holds play there while it shows
     * (RoundScoreTime / ScoreTime); see patch 0017.
     */
    external fun getScoreboardState(): Int

    /**
     * M9: ends the current game and returns the engine to the state
     * [startLocalGame] and [startJoinGame] will accept. Idempotent, so the
     * menu can call it without tracking whether a game is running.
     */
    external fun stopGame()

    /**
     * M10: the options the game setup screen offers, as rows of
     * "name|kind|value|min|max|step|choices|description" - see
     * [parseSetupOptions]. These are upstream's own OptionsGame entries, with
     * upstream's own ranges and descriptions; nothing here invents a rule.
     */
    external fun getSetupOptions(): Array<String>

    /**
     * Chooses a value for one of them. False when the option isn't offered, or
     * when upstream's own validation rejects the value (out of range, or not
     * one of an enum's choices) - which the UI should report rather than retry.
     */
    external fun setSetupOption(name: String, value: String): Boolean

    /** Back to the values the shipped config file specifies. */
    external fun resetSetupOptions()

    /**
     * The mods that can be chosen: "none" (upstream's base game) first, then
     * whatever sits in data/globalmods. Only names from this list should be
     * passed to [setSelectedMod] - the engine does not validate them, and a
     * name with no directory behind it fails at load time.
     */
    /**
     * M11: the name this player's tank carries, hosting or joining. Returns
     * the name actually in force - the engine refuses an empty one and keeps
     * the previous name, so the caller should store what comes back.
     */
    external fun setPlayerName(name: String): String

    /**
     * M11: renderer options that cost frame rate. Applied on the next frame;
     * no game restart needed.
     */
    external fun setRenderOptions(showTrees: Boolean, showFog: Boolean)

    /**
     * M22: which aim sight to draw - 0 for this port's own blade, 1 for
     * Scorched3D's own arrangement (a protractor ring under the tank, a
     * bearing marker on the ground, a blade along the barrel).
     */
    external fun setSightStyle(style: Int)

    /**
     * M23: how finely the landscape is drawn, as the resolution of the mesh
     * grid. The default is the heightmap's own, which is what upstream draws;
     * lower is cheaper. Takes effect on the next landscape.
     */
    external fun setTerrainDetail(grid: Int)

    /**
     * W4: which sea to draw - 0 for this port's two sine waves, 1 for
     * Scorched3D's own Tessendorf spectrum, generated on a worker thread.
     */
    external fun setOceanStyle(style: Int)

    /** M23: the range [setTerrainDetail] accepts, as "min|max". */
    external fun getTerrainDetailRange(): String

    /**
     * M16: the tank models the selected mod offers, by name, from its own
     * tanks.xml. Does not include upstream's "Random" entry - that is the
     * empty choice, not a model.
     */
    external fun getTankModels(): Array<String>

    /** M16: upstream's own tank palette, 0xRRGGBB each, in its own order. */
    external fun getTankColors(): IntArray

    /** M16: the avatars upstream ships, as paths relative to the data root. */
    external fun getAvatars(): Array<String>

    /**
     * M16: the tank this player wears. An empty model name, a negative colour
     * index or an empty avatar path each mean "let the game pick", which is
     * what it did before there was a choice. Applied when a game starts, so
     * changing it mid-game affects the next one.
     */
    external fun setPlayerIdentity(model: String, colorIndex: Int, avatar: String)

    /**
     * M12: replaces the setup options with those in a preset file - upstream's
     * own data/singletutorial.xml, or one of the difficulty presets
     * [getPresets] lists. False if the file could not be read, in which case
     * nothing changed.
     */
    external fun loadSetupPreset(path: String): Boolean

    /**
     * M14: the ready-made games the installed mods offer, as rows for
     * [parsePresets]. Read from each mod's own modinfo.xml, so this is
     * upstream's difficulty menu rather than a copy of it.
     */
    external fun getPresets(): Array<String>

    /**
     * M18: the bots the chosen mod offers, as "name|description" rows.
     * Upstream has no difficulty dial - which AI fills the slots *is* the
     * difficulty, and its own descriptions say how good each one is.
     */
    external fun getBots(): Array<String>

    /** M18: which AI fills the slots that are not the player's. */
    external fun getBotType(): String

    external fun setBotType(name: String): Boolean

    /**
     * M19: the mix. The slots are filled round-robin from this list, so the
     * proportions asked for are the proportions played. Never empty - a game
     * needs someone to play against.
     */
    external fun getBotTypes(): Array<String>

    external fun setBotTypes(names: Array<String>): Boolean

    /**
     * M21: the ambient sounds the current landscape asks for, as
     * "file|gain|looped|min|max" rows. Reads two small XML files, so poll
     * [getLandscapeTex] and only call this when the landscape has changed.
     */
    external fun getAmbientSounds(): Array<String>

    /** M21: which landscape is loaded, as its texture definition's path. */
    external fun getLandscapeTex(): String

    /** M19: every landscape the chosen mod defines, by name. */
    external fun getLandscapes(): Array<String>

    /**
     * M19: the ones a game may choose between. Empty is upstream's own "all
     * of them" (LandscapeDefinitionsBase::landscapeEnabled), not "none".
     */
    external fun getSelectedLandscapes(): Array<String>

    external fun setLandscapes(names: Array<String>): Boolean

    external fun getAvailableMods(): Array<String>

    external fun getSelectedMod(): String

    external fun setSelectedMod(name: String): Boolean

    /**
     * Sends a chat message on a channel ("general" or "team"). Hosting, this
     * goes straight into the server's channel manager; joined, it is a
     * ComsChannelTextMessage to the host. False if there is no tank to speak
     * as yet, or (joined) the handshake has not finished.
     */
    external fun sendChat(channel: String, text: String): Boolean

    /**
     * Chat lines newer than [afterId], oldest first, as "id|channel|who|text".
     * Text is last so it may contain pipes. See [parseChatLines].
     */
    external fun getChatLines(afterId: Int): Array<String>

    /** Bumped on every new chat line, so the HUD can poll one int. */
    external fun getChatVersion(): Int

    /**
     * M6 parity: simulation speed (upstream's SIMULATION_SPEED_* keys).
     * A fraction rather than a float because the engine's clock is
     * fixed-point and 1/8 has an exact representation there. Host only -
     * a joined client follows the host's pace (see the native comment).
     */
    external fun setSimulationSpeed(numerator: Int, denominator: Int): Boolean

    /** The current speed as "numerator|denominator". */
    external fun getSimulationSpeed(): String
}

/**
 * Mirrors the non-shot half of ComsPlayedMoveMessage::MoveType - see
 * [NativeBridge.submitMove]. (eShot is submitted via
 * [NativeBridge.fireWeapon] instead, which needs the weapon/angle/power.)
 */
object MoveType {
    const val RESIGN = 2
    const val SKIP = 3
    const val FINISHED_BUY = 4
}

/** Mirrors ComsDefenseMessage::DefenseChange - see [NativeBridge.useDefense]. */
object DefenseChange {
    const val SHIELD_UP = 1
    const val SHIELD_DOWN = 2
    const val PARACHUTES_UP = 3
    const val PARACHUTES_DOWN = 4
    const val BATTERY_USE = 5
}

/** Mirrors AccessoryPart::AccessoryType - the `type` field of a [WeaponShopEntry]. */
object AccessoryType {
    const val WEAPON = "weapon"
    const val PARACHUTE = "parachute"
    const val SHIELD = "shield"
    const val AUTO_DEFENSE = "autodefense"
    const val BATTERY = "battery"
}

/** Mirrors ClientContext::State (see ClientContext.hpp) for [NativeBridge.getClientJoinState]. */
object ClientJoinState {
    const val NONE = -1
    const val IDLE = 0
    const val CONNECTING = 1
    const val WAITING_AUTH_CHALLENGE = 2
    const val WAITING_CONNECT_ACCEPT = 3
    const val WAITING_LOAD_LEVEL = 4
    const val JOINED = 5
    const val FAILED = 6
}

/**
 * Parsed form of one row from [NativeBridge.getWeaponShop]. [ownedCount] is
 * -1 for "unlimited" (see TanketAccessories::getAccessoryCount()) - a real
 * starting weapon can be unlimited, so callers must treat that as owned,
 * not as zero/unowned (a bug that made a fresh tank's own default weapon
 * impossible to select and confusingly showed as "-1 owned" in the Shop).
 */
data class WeaponShopEntry(
    val accessoryId: Int,
    val name: String,
    val price: Int,
    val ownedCount: Int,
    val isCurrentWeapon: Boolean,
    val type: String,
    /**
     * Upstream's own shop tab for this accessory (`<tabgroup>`): "weapon"
     * or "defense". Not always what [type] implies - Fuel and Rocket Fuel
     * are weapons that upstream files under defense, since they are bought
     * alongside shields rather than alongside missiles.
     */
    val tabGroup: String,
) {
    val isOwned: Boolean get() = ownedCount != 0
    val ownedLabel: String get() = if (ownedCount < 0) "unlimited" else ownedCount.toString()

    /** Weapons are fired; everything else is activated via [NativeBridge.useDefense]. */
    val isWeapon: Boolean get() = type == AccessoryType.WEAPON

    /**
     * The [DefenseChange] code that activates this accessory, or null if it
     * isn't an activatable defense (weapons, and auto-defense which the
     * engine applies on its own rather than on demand).
     */
    val activationChange: Int? get() = when (type) {
        AccessoryType.SHIELD -> DefenseChange.SHIELD_UP
        AccessoryType.PARACHUTE -> DefenseChange.PARACHUTES_UP
        AccessoryType.BATTERY -> DefenseChange.BATTERY_USE
        else -> null
    }
}

fun parseWeaponShop(rows: Array<String>): List<WeaponShopEntry> = rows.mapNotNull { row ->
    val parts = row.split("|")
    if (parts.size != 7) return@mapNotNull null
    WeaponShopEntry(
        accessoryId = parts[0].toIntOrNull() ?: return@mapNotNull null,
        name = parts[1],
        price = parts[2].toIntOrNull() ?: return@mapNotNull null,
        ownedCount = parts[3].toIntOrNull() ?: return@mapNotNull null,
        isCurrentWeapon = parts[4] == "1",
        type = parts[5],
        tabGroup = parts[6],
    )
}

/**
 * One row of the score table. [colorArgb] is the tank's own engine colour,
 * the same one its model and name plate use.
 */
data class PlayerEntry(
    val playerId: Int,
    val name: String,
    val isBot: Boolean,
    val team: Int,
    val score: Int,
    val kills: Int,
    val wins: Int,
    val money: Int,
    val alive: Boolean,
    val ping: Int,
    val colorArgb: Int,
    val isMe: Boolean,
    /** M16: path to this player's avatar image, relative to the data root. */
    val avatar: String,
)

fun parsePlayerList(rows: Array<String>): List<PlayerEntry> = rows.mapNotNull { row ->
    // The avatar is a file name and comes last, so it is the only field that
    // could hold anything unexpected.
    val parts = row.split("|", limit = 13)
    if (parts.size != 13) return@mapNotNull null
    val rgb = parts[10].split(",")
    if (rgb.size != 3) return@mapNotNull null
    PlayerEntry(
        playerId = parts[0].toIntOrNull() ?: return@mapNotNull null,
        name = parts[1],
        isBot = parts[2] == "1",
        team = parts[3].toIntOrNull() ?: 0,
        score = parts[4].toIntOrNull() ?: 0,
        kills = parts[5].toIntOrNull() ?: 0,
        wins = parts[6].toIntOrNull() ?: 0,
        money = parts[7].toIntOrNull() ?: 0,
        alive = parts[8] == "1",
        ping = parts[9].toIntOrNull() ?: 0,
        colorArgb = (0xFF shl 24) or
            ((rgb[0].toIntOrNull() ?: 255) shl 16) or
            ((rgb[1].toIntOrNull() ?: 255) shl 8) or
            (rgb[2].toIntOrNull() ?: 255),
        isMe = parts[11] == "1",
        avatar = parts[12],
    )
}

/** One chat line. [who] is empty when the game itself is talking. */
data class ChatLine(
    val id: Int,
    val channel: String,
    val who: String,
    val text: String,
)

fun parseChatLines(rows: Array<String>): List<ChatLine> = rows.mapNotNull { row ->
    // Split into exactly four, so a message containing pipes survives.
    val parts = row.split("|", limit = 4)
    if (parts.size != 4) return@mapNotNull null
    ChatLine(
        id = parts[0].toIntOrNull() ?: return@mapNotNull null,
        channel = parts[1],
        who = parts[2],
        text = parts[3],
    )
}

/**
 * M10: one game-setup option, as [NativeBridge.getSetupOptions] reports it.
 *
 * [label] is upstream's identifier made presentable - "NumberOfRounds" becomes
 * "Number Of Rounds", "WallConcrete" becomes "Concrete". Upstream writes these
 * for a config file and a desktop options dialog; a phone screen wants them
 * spaced out and without their type prefix.
 */
data class SetupOption(
    val name: String,
    /** M18: which tab of the setup screen this belongs on, decided natively. */
    val group: String,
    /** M19: shown under the tab's "Advanced" heading rather than in front. */
    val advanced: Boolean,
    val kind: SetupKind,
    val value: String,
    val minValue: Int,
    val maxValue: Int,
    val stepValue: Int,
    val choices: List<SetupChoice>,
    val description: String,
) {
    val label: String get() = when (name) {
        // Upstream's own spelling, which is a typo it has carried for
        // twenty years. The option name has to keep it - that is the key in
        // every config file ever written - but the label does not.
        "TeamBallance" -> "Team Balance"
        else -> humanise(name)
    }
}

data class SetupChoice(val value: Int, val rawLabel: String) {
    /** "WallConcrete" -> "Concrete", "TurnSequentialRandom" -> "Sequential Random". */
    val label: String get() = humanise(
        // WindChange before Wind: the WindType values are all
        // "WindChangeSomething", and stripping only "Wind" leaves every one of
        // them starting with a "Change" that the row's own label already says.
        rawLabel.removePrefix("Wall").removePrefix("Turn")
            .removePrefix("TeamBallance").removePrefix("Resign")
            .removePrefix("WindChange").removePrefix("Wind")
    ).ifEmpty { humanise(rawLabel) }
}

enum class SetupKind { BOUNDED_INT, INT, BOOL, ENUM }

/** Splits an upstream identifier into words: "NumberOfRounds" -> "Number Of Rounds". */
private fun humanise(identifier: String): String =
    identifier.replace(Regex("(?<=[a-z0-9])(?=[A-Z])"), " ").trim()

fun parseSetupOptions(rows: Array<String>): List<SetupOption> = rows.mapNotNull { row ->
    // Ten fields, description last so it may contain anything - including
    // the pipes and commas the earlier fields use as separators.
    val parts = row.split("|", limit = 10)
    if (parts.size != 10) return@mapNotNull null
    val kind = when (parts[3].toIntOrNull()) {
        0 -> SetupKind.BOUNDED_INT
        1 -> SetupKind.INT
        2 -> SetupKind.BOOL
        3 -> SetupKind.ENUM
        else -> return@mapNotNull null
    }
    SetupOption(
        name = parts[0],
        group = parts[1],
        advanced = parts[2] == "1",
        kind = kind,
        value = parts[4],
        minValue = parts[5].toIntOrNull() ?: 0,
        maxValue = parts[6].toIntOrNull() ?: 0,
        stepValue = parts[7].toIntOrNull() ?: 1,
        choices = parts[8].split(",").filter { it.isNotBlank() }.mapNotNull { choice ->
            val split = choice.split("=", limit = 2)
            if (split.size != 2) null
            else SetupChoice(split[0].toIntOrNull() ?: return@mapNotNull null, split[1])
        },
        description = parts[9],
    )
}

/**
 * M14: one ready-made game, as a mod's own modinfo.xml describes it.
 *
 * Upstream's difficulty menu is not code anywhere - each mod names a few
 * options files and the words to show beside them - so this port lists what
 * the mods say rather than a copy of what upstream's happen to say today.
 */
data class GamePreset(
    val mod: String,
    val name: String,
    /** Path to the options file, ready for [NativeBridge.loadSetupPreset]. */
    val gameFile: String,
    val description: String,
) {
    /** Upstream writes these over several lines; a menu row wants one. */
    val summary: String get() = description.replace(Regex("\\s*\\n\\s*"), " ").trim()
}

fun parsePresets(rows: Array<String>): List<GamePreset> = rows.mapNotNull { row ->
    // Four fields, description last so it may contain anything.
    val parts = row.split("|", limit = 4)
    if (parts.size != 4) return@mapNotNull null
    GamePreset(mod = parts[0], name = parts[1], gameFile = parts[2], description = parts[3])
}

/** "none" is upstream's own name for the base game, which reads as an absence. */
fun modLabel(mod: String): String =
    if (mod == "none") "Scorched3D" else mod.replaceFirstChar { it.uppercase() }

/** M18: one bot the mod offers, as [NativeBridge.getBots] reports it. */
data class BotOption(val name: String, val description: String)

fun parseBots(rows: Array<String>): List<BotOption> = rows.mapNotNull { row ->
    val parts = row.split("|", limit = 2)
    if (parts.size != 2) return@mapNotNull null
    BotOption(name = parts[0], description = parts[1])
}

/** M21: one ambient sound, as [NativeBridge.getAmbientSounds] reports it. */
data class AmbientSound(
    val file: String,
    val gain: Float,
    /** Looped plays continuously; otherwise it fires every so often. */
    val looped: Boolean,
    val minSeconds: Float,
    val maxSeconds: Float,
)

fun parseAmbientSounds(rows: Array<String>): List<AmbientSound> = rows.mapNotNull { row ->
    val parts = row.split("|")
    if (parts.size != 5) return@mapNotNull null
    AmbientSound(
        file = parts[0],
        gain = parts[1].toFloatOrNull() ?: 1f,
        looped = parts[2] == "1",
        minSeconds = parts[3].toFloatOrNull() ?: 0f,
        maxSeconds = parts[4].toFloatOrNull() ?: 0f,
    )
}
