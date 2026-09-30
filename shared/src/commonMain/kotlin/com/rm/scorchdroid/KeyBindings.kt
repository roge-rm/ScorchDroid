package com.rm.scorchdroid

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.input.key.Key

/**
 * What a key can do in a game. The defaults are Scorched3D's own
 * (data/keys.xml), for the actions this port has.
 *
 * [held] actions act for as long as the key is down, and Shift and Ctrl
 * change how fast (see GameController.applyHeldKeys), so they're bound to a
 * bare key. The rest act once per press, and can be bound with Shift, Ctrl or
 * Alt, the way Shift+Tab is the previous weapon.
 */
enum class KeyAction(val title: String, val group: String, val held: Boolean, val defaults: List<KeyBinding>) {
    TURN_LEFT("Turn left", GROUP_AIM, true, keys(Key.DirectionLeft)),
    TURN_RIGHT("Turn right", GROUP_AIM, true, keys(Key.DirectionRight)),
    RAISE("Raise the barrel", GROUP_AIM, true, keys(Key.DirectionUp)),
    LOWER("Lower the barrel", GROUP_AIM, true, keys(Key.DirectionDown)),
    POWER_UP("More power", GROUP_AIM, true, keys(Key.Equals, Key.Plus, Key.NumPadAdd, Key.PageUp)),
    POWER_DOWN("Less power", GROUP_AIM, true, keys(Key.Minus, Key.NumPadSubtract, Key.PageDown)),
    FIRE("Fire", GROUP_AIM, false, keys(Key.Spacebar, Key.F)),
    UNDO("Back to your last shot's aim", GROUP_AIM, false, keys(Key.U)),
    AIM_AT_POINTER("Aim where the mouse is", GROUP_AIM, false, keys(Key.A)),

    NEXT_WEAPON("Next weapon", GROUP_ARMS, false, keys(Key.Tab)),
    PREVIOUS_WEAPON("Previous weapon", GROUP_ARMS, false, listOf(KeyBinding(Key.Tab, shift = true))),
    SHIELD("Shield", GROUP_ARMS, false, keys(Key.D)),
    PARACHUTES("Parachutes", GROUP_ARMS, false, keys(Key.P)),
    BATTERY("Battery", GROUP_ARMS, false, keys(Key.B)),

    CAMERA_TOP("Top down view", GROUP_CAMERA, false, keys(Key.One)),
    CAMERA_BEHIND("Above and behind view", GROUP_CAMERA, false, keys(Key.Two)),
    CAMERA_TANK("View from the tank", GROUP_CAMERA, false, keys(Key.Three)),
    CAMERA_SHOT("Follow the shot", GROUP_CAMERA, false, keys(Key.Four)),
    CAMERA_ACTION("Action view", GROUP_CAMERA, false, keys(Key.Nine)),
    CAMERA_MENU("Camera views", GROUP_CAMERA, false, keys(Key.C)),
    CAMERA_LEFT("Camera left", GROUP_CAMERA, true, keys(Key.NumPad4)),
    CAMERA_RIGHT("Camera right", GROUP_CAMERA, true, keys(Key.NumPad6)),
    CAMERA_UP("Camera up", GROUP_CAMERA, true, keys(Key.NumPad8)),
    CAMERA_DOWN("Camera down", GROUP_CAMERA, true, keys(Key.NumPad2)),
    ZOOM_IN("Zoom in", GROUP_CAMERA, true, keys(Key.NumPad9)),
    ZOOM_OUT("Zoom out", GROUP_CAMERA, true, keys(Key.NumPad3)),

    CHAT("Chat", GROUP_OTHER, false, keys(Key.T, Key.Enter, Key.Slash)),
    TEAM_CHAT("Team chat", GROUP_OTHER, false, keys(Key.Y)),
    SCORES("Scores", GROUP_OTHER, false, keys(Key.S)),
    ACTIONS("More actions", GROUP_OTHER, false, keys(Key.O)),
    SPEED_1("Normal speed", GROUP_OTHER, false, keys(Key.F1)),
    SPEED_2("2x speed", GROUP_OTHER, false, keys(Key.F2)),
    SPEED_3("3x speed", GROUP_OTHER, false, keys(Key.F3)),
    SPEED_4("4x speed", GROUP_OTHER, false, keys(Key.F4)),
    LEAVE("Leave the game", GROUP_OTHER, false, keys(Key.Escape)),
}

const val GROUP_AIM = "Aiming"
const val GROUP_ARMS = "Weapons and defences"
const val GROUP_CAMERA = "Camera"
const val GROUP_OTHER = "Other"

// Key is a value class, so the defaults above pass it through here one at a time.
private fun keys(a: Key) = listOf(KeyBinding(a))
private fun keys(a: Key, b: Key) = listOf(KeyBinding(a), KeyBinding(b))
private fun keys(a: Key, b: Key, c: Key) = listOf(KeyBinding(a), KeyBinding(b), KeyBinding(c))
private fun keys(a: Key, b: Key, c: Key, d: Key) = listOf(KeyBinding(a), KeyBinding(b), KeyBinding(c), KeyBinding(d))

/** A key, and the modifiers that have to be held with it. */
data class KeyBinding(val key: Key, val shift: Boolean = false, val ctrl: Boolean = false, val alt: Boolean = false) {
    val label: String
        get() = buildString {
            if (ctrl) append("Ctrl+")
            if (alt) append("Alt+")
            if (shift) append("Shift+")
            append(KeyNames.name(key))
        }

    /** How it's saved. The same as [label], unless the key has no name. */
    val saved: String
        get() = buildString {
            if (ctrl) append("Ctrl+")
            if (alt) append("Alt+")
            if (shift) append("Shift+")
            append(KeyNames.savedName(key))
        }

    companion object {
        fun parse(text: String): KeyBinding? {
            val parts = text.split('+')
            val key = KeyNames.fromSaved(parts.last()) ?: return null
            val mods = parts.dropLast(1)
            return KeyBinding(key, shift = "Shift" in mods, ctrl = "Ctrl" in mods, alt = "Alt" in mods)
        }
    }
}

/**
 * The names keys are shown and saved by. Compose has no names for keys of
 * its own, and a key's code differs between a phone and a browser, so the
 * keys anyone is likely to bind are named here, and anything else is saved
 * by its code (which is fine: each device keeps its own settings).
 */
object KeyNames {
    private val named: List<Pair<Key, String>> = buildList {
        val letters = listOf(
            Key.A, Key.B, Key.C, Key.D, Key.E, Key.F, Key.G, Key.H, Key.I, Key.J, Key.K, Key.L, Key.M,
            Key.N, Key.O, Key.P, Key.Q, Key.R, Key.S, Key.T, Key.U, Key.V, Key.W, Key.X, Key.Y, Key.Z,
        )
        letters.forEachIndexed { i, key -> add(key to ('A' + i).toString()) }
        val digits = listOf(Key.Zero, Key.One, Key.Two, Key.Three, Key.Four, Key.Five, Key.Six, Key.Seven, Key.Eight, Key.Nine)
        digits.forEachIndexed { i, key -> add(key to i.toString()) }
        val numPad = listOf(
            Key.NumPad0, Key.NumPad1, Key.NumPad2, Key.NumPad3, Key.NumPad4,
            Key.NumPad5, Key.NumPad6, Key.NumPad7, Key.NumPad8, Key.NumPad9,
        )
        numPad.forEachIndexed { i, key -> add(key to "Num $i") }
        val functions = listOf(
            Key.F1, Key.F2, Key.F3, Key.F4, Key.F5, Key.F6, Key.F7, Key.F8, Key.F9, Key.F10, Key.F11, Key.F12,
        )
        functions.forEachIndexed { i, key -> add(key to "F${i + 1}") }
        add(Key.DirectionLeft to "Left")
        add(Key.DirectionRight to "Right")
        add(Key.DirectionUp to "Up")
        add(Key.DirectionDown to "Down")
        add(Key.Spacebar to "Space")
        add(Key.Enter to "Enter")
        add(Key.Tab to "Tab")
        add(Key.Escape to "Esc")
        add(Key.Backspace to "Backspace")
        add(Key.Delete to "Delete")
        add(Key.Insert to "Insert")
        add(Key.MoveHome to "Home")
        add(Key.MoveEnd to "End")
        add(Key.PageUp to "Page Up")
        add(Key.PageDown to "Page Down")
        add(Key.Minus to "-")
        add(Key.Equals to "=")
        add(Key.Plus to "+")
        add(Key.LeftBracket to "[")
        add(Key.RightBracket to "]")
        add(Key.Semicolon to ";")
        add(Key.Apostrophe to "'")
        add(Key.Comma to ",")
        add(Key.Period to ".")
        add(Key.Slash to "/")
        add(Key.Backslash to "\\")
        add(Key.Grave to "`")
        add(Key.NumPadAdd to "Num +")
        add(Key.NumPadSubtract to "Num -")
        add(Key.NumPadMultiply to "Num *")
        add(Key.NumPadDivide to "Num /")
        add(Key.NumPadEnter to "Num Enter")
        add(Key.NumPadDot to "Num .")
    }
    private val byKey = named.associate { it }
    private val byName = named.associate { (key, name) -> name to key }

    /** Keys that only modify another one, and can't be bound on their own. */
    val modifiers = setOf(
        Key.ShiftLeft, Key.ShiftRight, Key.CtrlLeft, Key.CtrlRight,
        Key.AltLeft, Key.AltRight, Key.MetaLeft, Key.MetaRight,
    )

    fun name(key: Key): String = byKey[key] ?: "Key ${key.keyCode}"
    fun savedName(key: Key): String = byKey[key] ?: "#${key.keyCode}"
    fun key(name: String): Key? = byName[name]
    fun fromSaved(text: String): Key? =
        if (text.startsWith("#")) text.drop(1).toLongOrNull()?.let { Key(it) } else byName[text]
}

/**
 * Which keys do what, saved with the other settings. Starts as Scorched3D's
 * defaults, and each action the player changes is saved on its own, so a new
 * action added in a later version gets its default rather than nothing.
 */
class KeyBindings(private val prefs: KeyValueStore) {
    // Compose state, so the key screen redraws as bindings change.
    var bindings by mutableStateOf(load())
        private set

    private fun load(): Map<KeyAction, List<KeyBinding>> = KeyAction.entries.associateWith { action ->
        val saved = prefs.getString(prefKey(action), UNSET)
        if (saved == UNSET) {
            action.defaults
        } else {
            saved.split(SEPARATOR).filter { it.isNotEmpty() }.mapNotNull { KeyBinding.parse(it) }
        }
    }

    fun keysFor(action: KeyAction): List<KeyBinding> = bindings[action].orEmpty()

    /**
     * Makes [binding] the one key for [action], and takes it off any other
     * action that had it. Returns those, so the screen can say so.
     */
    fun assign(action: KeyAction, binding: KeyBinding): List<KeyAction> {
        val clean = if (action.held) binding.copy(shift = false, ctrl = false, alt = false) else binding
        val takenFrom = bindings.filter { (other, keys) -> other != action && keys.any { it.conflicts(clean) } }.keys.toList()
        val next = bindings.toMutableMap()
        for (other in takenFrom) next[other] = next[other].orEmpty().filterNot { it.conflicts(clean) }
        next[action] = listOf(clean)
        save(next)
        return takenFrom
    }

    fun clear(action: KeyAction) {
        save(bindings + (action to emptyList()))
    }

    fun resetAll() {
        save(KeyAction.entries.associateWith { it.defaults })
    }

    private fun save(next: Map<KeyAction, List<KeyBinding>>) {
        for ((action, keys) in next) {
            if (keys != bindings[action]) prefs.putString(prefKey(action), keys.joinToString(SEPARATOR) { it.saved })
        }
        bindings = next
    }

    /**
     * The one-press action for a key pressed with these modifiers: an exact
     * match first, so Shift+Tab finds the previous weapon, then the key on
     * its own.
     */
    fun pressAction(key: Key, shift: Boolean, ctrl: Boolean, alt: Boolean): KeyAction? {
        val exact = KeyBinding(key, shift, ctrl, alt)
        val plain = KeyBinding(key)
        val candidates = bindings.filterKeys { !it.held }
        return candidates.entries.firstOrNull { exact in it.value }?.key
            ?: candidates.entries.firstOrNull { plain in it.value }?.key
    }

    /** Whether any key of a held [action] is down. Modifiers don't matter for these. */
    fun isHeld(action: KeyAction, keysDown: Set<Key>): Boolean = keysFor(action).any { it.key in keysDown }

    /** Whether this key belongs to a held action. */
    fun isHeldKey(key: Key): Boolean = bindings.any { (action, keys) -> action.held && keys.any { it.key == key } }

    private fun KeyBinding.conflicts(other: KeyBinding): Boolean =
        key == other.key && shift == other.shift && ctrl == other.ctrl && alt == other.alt

    private fun prefKey(action: KeyAction) = "keys.${action.name.lowercase()}"

    private companion object {
        const val UNSET = "\u0000unset"
        const val SEPARATOR = "|"
    }
}
