package com.rm.scorchdroid

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The mini-map's line geometry, which decides what a tap places and what a
 * long press deletes.
 *
 * On a phone both of those are a fingertip against a few pixels, so the
 * arithmetic is the part that is worth pinning down away from the phone -
 * the same reason MiniMapBuilder's alpha rule is checked in host-tests
 * rather than by squinting at a screenshot.
 */
class MapLinesTest {
    private val red = 0xFFFF0000.toInt()

    @Test
    fun firstTapPendsAndSecondCompletes() {
        val (pendingAfterFirst, lineAfterFirst) = placeMapPoint(null, 10f, 20f, red)
        assertEquals(MapPoint(10f, 20f), pendingAfterFirst)
        assertNull("the first tap draws a dot, not a line", lineAfterFirst)

        val (pendingAfterSecond, lineAfterSecond) =
            placeMapPoint(pendingAfterFirst, 30f, 40f, red)
        assertNull("the second tap clears the dot", pendingAfterSecond)
        assertEquals(MapLine(10f, 20f, 30f, 40f, red), lineAfterSecond)
    }

    @Test
    fun twoTapsInOnePlaceMakeADot() {
        val (_, line) = placeMapPoint(MapPoint(5f, 5f), 5f, 5f, red)
        assertNotNull(line)
        assertEquals(0f, distanceToSegment(5f, 5f, line!!.ax, line.ay, line.bx, line.by), 1e-4f)
        // And it is still findable, or it could never be removed.
        assertEquals(0, hitTestMapLine(listOf(line), 6f, 5f, 2f))
    }

    @Test
    fun distanceIsToTheSegmentNotTheInfiniteLine() {
        // The segment runs along y = 0 from x = 0 to x = 10.
        // Beside its middle: the perpendicular.
        assertEquals(3f, distanceToSegment(5f, 3f, 0f, 0f, 10f, 0f), 1e-4f)
        // Beyond its end: the distance to the end, not to the line's
        // extension, which would be 3 and would make a long press far off
        // the end of a line delete it.
        assertEquals(5f, distanceToSegment(14f, 3f, 0f, 0f, 10f, 0f), 1e-4f)
        // Exactly on an endpoint.
        assertEquals(0f, distanceToSegment(0f, 0f, 0f, 0f, 10f, 0f), 1e-4f)
    }

    @Test
    fun aLongPressThatMissesEverythingHitsNothing() {
        val lines = listOf(MapLine(0f, 0f, 10f, 0f, red))
        assertEquals(-1, hitTestMapLine(lines, 5f, 50f, 4f))
        assertEquals(-1, hitTestMapLine(emptyList(), 5f, 0f, 4f))
    }

    @Test
    fun theThresholdIsInclusiveAndInLandscapeUnits() {
        val lines = listOf(MapLine(0f, 0f, 10f, 0f, red))
        assertEquals("just inside", 0, hitTestMapLine(lines, 5f, 4f, 4f))
        assertEquals("just outside", -1, hitTestMapLine(lines, 5f, 4.01f, 4f))
    }

    @Test
    fun theNearestLineWins() {
        val near = MapLine(0f, 0f, 10f, 0f, red)
        val far = MapLine(0f, 20f, 10f, 20f, red)
        assertEquals(0, hitTestMapLine(listOf(near, far), 5f, 1f, 30f))
        assertEquals(1, hitTestMapLine(listOf(far, near), 5f, 1f, 30f))
    }

    @Test
    fun equallyCloseLinesGoToTheNewest() {
        // Two lines crossing under one fingertip: the second was drawn last.
        val first = MapLine(0f, 0f, 10f, 0f, red)
        val second = MapLine(5f, -5f, 5f, 5f, red)
        assertEquals(1, hitTestMapLine(listOf(first, second), 5f, 0f, 4f))
    }
}
