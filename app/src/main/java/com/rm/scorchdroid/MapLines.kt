package com.rm.scorchdroid

import kotlin.math.hypot

/**
 * Lines drawn on the mini-map, and the geometry behind placing and removing
 * them.
 *
 * Ours, not upstream's. Upstream's plan drawing (`GLWPlanView`,
 * `ComsLinesMessage`) is a freehand scribble made by dragging the right mouse
 * button, decimated at 5px, which every receiver fades out three seconds
 * after it arrives. A phone has no second button, and a three-second
 * scribble is a gesture you make *while* talking - which is the desktop
 * game's context, not this one. So this is a two-tap straight line that stays
 * until it is deleted: the same job (pointing at a place on the map) done in
 * the way a touch screen can do it.
 *
 * Points are landscape coordinates, not widget ones - the same space the
 * tanks and the camera arrow are in - so a line stays pinned to the ground
 * when the map changes size underneath it.
 *
 * These functions are free of Compose and of the engine on purpose: the
 * arithmetic that decides what your finger hit is the part worth testing
 * without a phone in the loop.
 */

/** One end of a half-drawn line: the first tap, waiting for its second. */
data class MapPoint(val x: Float, val y: Float)

/** A finished line, in landscape coordinates, in its drawer's tank colour. */
data class MapLine(
    val ax: Float,
    val ay: Float,
    val bx: Float,
    val by: Float,
    val colorArgb: Int,
)

/**
 * The two-tap line. The first tap is remembered and shown as a dot; the
 * second completes a line and clears the dot. Returns the pending point after
 * this tap and the line it finished, if any.
 *
 * Two taps in the same spot make a zero-length line, which draws as a dot and
 * deletes like any other. That is deliberate: a marker on one place is a
 * thing people want, and special-casing it would mean picking a distance
 * below which a line "wasn't meant", which is not a judgement this can make.
 */
fun placeMapPoint(
    pending: MapPoint?,
    x: Float,
    y: Float,
    colorArgb: Int,
): Pair<MapPoint?, MapLine?> =
    if (pending == null) {
        MapPoint(x, y) to null
    } else {
        null to MapLine(pending.x, pending.y, x, y, colorArgb)
    }

/**
 * Shortest distance from a point to the segment a-b (not to the infinite line
 * through them: a finger near the *extension* of a line has not touched it).
 */
fun distanceToSegment(
    px: Float,
    py: Float,
    ax: Float,
    ay: Float,
    bx: Float,
    by: Float,
): Float {
    val dx = bx - ax
    val dy = by - ay
    val lengthSq = dx * dx + dy * dy
    // A zero-length line is a dot, and the nearest point on it is itself.
    val t = if (lengthSq <= 0f) {
        0f
    } else {
        (((px - ax) * dx + (py - ay) * dy) / lengthSq).coerceIn(0f, 1f)
    }
    return hypot(px - (ax + t * dx), py - (ay + t * dy))
}

/**
 * The line a long press at (x, y) means, or -1 if it missed everything.
 * [threshold] is in landscape units, so callers convert a touch target's
 * worth of screen into ground distance and the miss allowed is the same at
 * either map size.
 *
 * Ties go to the newest line, because two lines crossing under one fingertip
 * is exactly when you are undoing the one you just drew.
 */
fun hitTestMapLine(
    lines: List<MapLine>,
    x: Float,
    y: Float,
    threshold: Float,
): Int {
    var best = -1
    var bestDistance = threshold
    lines.forEachIndexed { index, line ->
        val distance = distanceToSegment(x, y, line.ax, line.ay, line.bx, line.by)
        if (distance <= bestDistance) {
            bestDistance = distance
            best = index
        }
    }
    return best
}
