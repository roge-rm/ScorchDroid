package com.rm.scorchdroid

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The mini-map's line arithmetic: what a tap places, and how a line fades and
 * goes.
 *
 * The fade is upstream's rule rather than ours, so it is worth holding to the
 * numbers in `GLWPlanView` exactly - the same reason MiniMapBuilder's alpha
 * rule is checked here rather than by squinting at a screenshot.
 */
class MapLinesTest {
    private val red = 0xFFFF0000.toInt()
    private val t0 = 1_000_000L

    @Test
    fun firstTapPendsAndSecondCompletes() {
        val (pendingAfterFirst, lineAfterFirst) = placeMapPoint(null, 10f, 20f, red, t0)
        assertEquals(MapPoint(10f, 20f, t0), pendingAfterFirst)
        assertNull("the first tap draws a dot, not a line", lineAfterFirst)

        val (pendingAfterSecond, lineAfterSecond) =
            placeMapPoint(pendingAfterFirst, 30f, 40f, red, t0 + 500)
        assertNull("the second tap clears the dot", pendingAfterSecond)
        assertEquals(MapLine(10f, 20f, 30f, 40f, red, t0 + 500), lineAfterSecond)
    }

    @Test
    fun theLineIsStampedWhenItCompletesNotWhenItWasStarted() {
        // A line placed over two and a half seconds would otherwise be born
        // most of the way faded - and a teammate, whose client stamps on
        // arrival, would never see it that way.
        val (pending, _) = placeMapPoint(null, 0f, 0f, red, t0)
        val (_, line) = placeMapPoint(pending, 10f, 10f, red, t0 + 2500)
        assertEquals(t0 + 2500, line!!.atMillis)
        assertEquals(1f, mapLineAlpha(line.atMillis, t0 + 2500), 1e-4f)
    }

    @Test
    fun twoTapsInOnePlaceMakeADot() {
        val (_, line) = placeMapPoint(MapPoint(5f, 5f, t0), 5f, 5f, red, t0)
        assertNotNull(line)
        assertEquals(line!!.ax, line.bx, 1e-4f)
        assertEquals(line.ay, line.by, 1e-4f)
    }

    @Test
    fun alphaFollowsUpstreamsOneMinusAgeOverThree() {
        assertEquals(1f, mapLineAlpha(t0, t0), 1e-4f)
        assertEquals(0.5f, mapLineAlpha(t0, t0 + 1500), 1e-4f)
        assertEquals(1f / 3f, mapLineAlpha(t0, t0 + 2000), 1e-4f)
        assertEquals(0f, mapLineAlpha(t0, t0 + 3000), 1e-4f)
    }

    @Test
    fun alphaIsClampedBothWays() {
        assertEquals("past its life, not negative", 0f, mapLineAlpha(t0, t0 + 9000), 1e-4f)
        // A clock that steps backwards (the wall clock can) must not make a
        // line brighter than solid.
        assertEquals("before it was drawn", 1f, mapLineAlpha(t0, t0 - 500), 1e-4f)
    }

    @Test
    fun aLineGoesExactlyWhenItsAlphaReachesZero() {
        val line = MapLine(0f, 0f, 10f, 0f, red, t0)
        val lines = listOf(line)
        assertEquals(1, expireMapLines(lines, t0 + 2999).size)
        assertTrue(mapLineAlpha(t0, t0 + 2999) > 0f)
        assertEquals("gone the moment it is invisible", 0, expireMapLines(lines, t0 + 3000).size)
    }

    @Test
    fun expiryKeepsTheYoungAndDropsTheOld() {
        val old = MapLine(0f, 0f, 1f, 1f, red, t0)
        val young = MapLine(2f, 2f, 3f, 3f, red, t0 + 2000)
        val kept = expireMapLines(listOf(old, young), t0 + 3000)
        assertEquals(listOf(young), kept)
    }

    @Test
    fun expiryDoesNotRebuildAListWithNothingToDrop() {
        // This runs every frame a line is on screen.
        val lines = listOf(MapLine(0f, 0f, 1f, 1f, red, t0))
        assertSame(lines, expireMapLines(lines, t0 + 100))
        assertSame(emptyList<MapLine>(), expireMapLines(emptyList(), t0))
    }

    @Test
    fun aHalfDrawnLineAbandonsItselfOnTheSameClock() {
        val pending = MapPoint(5f, 5f, t0)
        assertEquals(pending, expireMapPoint(pending, t0 + 2999))
        assertNull(expireMapPoint(pending, t0 + 3000))
        assertNull(expireMapPoint(null, t0))
    }
}
