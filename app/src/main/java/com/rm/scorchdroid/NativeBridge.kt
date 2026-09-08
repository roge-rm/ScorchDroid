package com.rm.scorchdroid

object NativeBridge {
    init {
        System.loadLibrary("scorchdroid_engine")
    }

    external fun helloFromNative(): String

    /** Points the native engine's cwd/$HOME at the extracted data root (see AssetDataExtractor). */
    external fun initEngine(dataRoot: String): Boolean

    /** M2 vertical slice: boots a real local game via ScorchedServer::startServer. */
    external fun startLocalGame(): Boolean

    /** Advances the real game simulation by one step (see ServerSimulator). */
    external fun tickEngine()

    /** Debug-only: current ServerState + tank count. */
    external fun getGameStateDebugString(): String

    /**
     * M4 touch-fire: (normX, normY) in the same [-0.9, 0.9] landscape space
     * the renderer draws in - fires "my tank" (see getMyTankId) aimed at the
     * tapped landscape point, at the given elevation (touch-controlled via
     * the elevation SeekBar - see MainActivity).
     */
    external fun handleTap(normX: Float, normY: Float, elevationDegrees: Float): Boolean

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
     * aim gesture (see MainActivity.setUpTouchToFire) once the player
     * releases; handleTap covers the plain-tap case.
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
)

fun parsePlayerList(rows: Array<String>): List<PlayerEntry> = rows.mapNotNull { row ->
    val parts = row.split("|")
    if (parts.size != 12) return@mapNotNull null
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
