package com.rm.scorchdroid

/**
 * Lines drawn on the mini-map, and the arithmetic behind placing and expiring
 * them.
 *
 * The *gesture* is ours, because a phone has no second mouse button: upstream
 * scribbles freehand by dragging the right one, where this sets one end of a
 * straight line with a tap and the other with a second tap. Everything the
 * wire can see is upstream's, deliberately, so a PC client on the other end
 * of a `ComsLinesMessage` renders what this draws and this renders what it
 * sends:
 *
 *  - a line is two points and a pen-up, which is exactly what upstream's
 *    `GLWPlanView` produces for the shortest possible drag;
 *  - lines **expire three seconds after they arrive**, and fade to nothing as
 *    they go (`drawLine`'s `1 - age/3`), rather than staying until removed;
 *  - there is no delete, because `ComsLinesMessage` is append-only and cannot
 *    express one. A stroke that removes itself does not need removing.
 *
 * The three seconds are counted from when the line was *completed*, not from
 * when its first point was tapped, and that is the faithful reading rather
 * than a shortcut: upstream's receivers stamp points with their own arrival
 * time (`simulateLine` sets `first[2] = totalTime_`), so a whole stroke
 * appears at once at the far end and fades together. Stamping on completion
 * is what a teammate would see anyway - and it stops a line that took a
 * moment to place from being born half faded.
 *
 * Points are landscape coordinates, not widget ones - the same space the
 * tanks and the camera arrow are in - so a line stays pinned to the ground it
 * pointed at when the map changes size underneath it.
 *
 * These functions are free of Compose and of the engine on purpose: the
 * arithmetic that decides what is on the map, and how faded, is the part
 * worth testing without a phone in the loop.
 */

/** Upstream's own lifetime for a drawn point: `GLWPlanView` uses 3 seconds. */
const val kMapLineLifeMillis = 3000L

/**
 * The first tap of a line, waiting for its second. Not on the wire - nothing
 * is sent until there is a line - but it expires on the same clock, which is
 * also how a half-drawn line is abandoned now that there is no delete: leave
 * it alone and it goes.
 */
data class MapPoint(val x: Float, val y: Float, val atMillis: Long)

/** A finished line, in landscape coordinates, in its drawer's tank colour. */
data class MapLine(
    val ax: Float,
    val ay: Float,
    val bx: Float,
    val by: Float,
    val colorArgb: Int,
    val atMillis: Long,
)

/**
 * The two-tap line. The first tap is remembered and shown as a dot; the
 * second completes a line and clears the dot. Returns the pending point after
 * this tap and the line it finished, if any.
 *
 * Two taps in the same spot make a zero-length line, which draws as a dot and
 * fades like any other. That is deliberate: a marker on one place is a thing
 * people want, and special-casing it would mean picking a distance below
 * which a line "wasn't meant", which is not a judgement this can make.
 */
fun placeMapPoint(
    pending: MapPoint?,
    x: Float,
    y: Float,
    colorArgb: Int,
    nowMillis: Long,
): Pair<MapPoint?, MapLine?> =
    if (pending == null) {
        MapPoint(x, y, nowMillis) to null
    } else {
        null to MapLine(pending.x, pending.y, x, y, colorArgb, nowMillis)
    }

/**
 * How solid a line is now: upstream's `1 - age/3`, clamped. Reaches zero
 * exactly when [expireMapLines] drops it, so a line never blinks out while
 * still visible.
 */
fun mapLineAlpha(atMillis: Long, nowMillis: Long): Float {
    val age = (nowMillis - atMillis).toFloat() / kMapLineLifeMillis.toFloat()
    return (1f - age).coerceIn(0f, 1f)
}

/** The lines still worth drawing. Upstream pops them off the front the same way. */
fun expireMapLines(lines: List<MapLine>, nowMillis: Long): List<MapLine> =
    if (lines.none { nowMillis - it.atMillis >= kMapLineLifeMillis }) {
        // The common case by far, and worth not rebuilding the list for:
        // this runs every frame a line is on screen.
        lines
    } else {
        lines.filter { nowMillis - it.atMillis < kMapLineLifeMillis }
    }

/** The pending dot if it is still alive, or null once it has aged out. */
fun expireMapPoint(pending: MapPoint?, nowMillis: Long): MapPoint? =
    pending?.takeIf { nowMillis - it.atMillis < kMapLineLifeMillis }
