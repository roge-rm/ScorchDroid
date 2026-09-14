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

    // A square arena that does not start at the origin, so an off-by-arenaX
    // cannot hide, and a tall one, so the letterbox is exercised.
    private val square = MiniMapInfo(64f, 128f, 512f, 512f, 640f, 640f, 0, 0)
    private val tall = MiniMapInfo(0f, 0f, 256f, 512f, 640f, 640f, 0, 0)

    @Test
    fun theArenaCentreIsTheWidgetCentre() {
        val (u, v) = landscapeToPlanFraction(64f + 256f, 128f + 256f, square)
        assertEquals(0.5f, u, 1e-4f)
        assertEquals(0.5f, v, 1e-4f)
    }

    @Test
    fun theArenaEdgesLandOnUpstreamsInset() {
        // Upstream draws the map inset by 10/128 of the widget, and line
        // fractions are of the whole widget - so the arena's own corner is
        // at the inset, not at 0.
        val (uLeft, vBottom) = landscapeToPlanFraction(64f, 128f, square)
        assertEquals(kPlanInsetFraction, uLeft, 1e-4f)
        assertEquals("landscape y = bottom is v = inset, not 1 - inset", kPlanInsetFraction, vBottom, 1e-4f)

        val (uRight, vTop) = landscapeToPlanFraction(64f + 512f, 128f + 512f, square)
        assertEquals(1f - kPlanInsetFraction, uRight, 1e-4f)
        assertEquals(1f - kPlanInsetFraction, vTop, 1e-4f)
    }

    @Test
    fun yRunsTheOppositeWayFromTheCanvas() {
        val (_, low) = landscapeToPlanFraction(320f, 200f, square)
        val (_, high) = landscapeToPlanFraction(320f, 400f, square)
        assertTrue("more landscape y must mean more v on the wire", high > low)
    }

    @Test
    fun aTallArenaIsLetterboxedNotStretched() {
        // 256 wide in a 512 span: the arena occupies the middle half of the
        // widget horizontally and all of it vertically.
        val (uLeft, _) = landscapeToPlanFraction(0f, 0f, tall)
        val (uRight, _) = landscapeToPlanFraction(256f, 0f, tall)
        val (_, vBottom) = landscapeToPlanFraction(0f, 0f, tall)
        val (_, vTop) = landscapeToPlanFraction(0f, 512f, tall)
        assertEquals(0.5f, (uLeft + uRight) / 2f, 1e-4f)
        assertEquals("half the width of the full span", (1f - 2 * kPlanInsetFraction) / 2f, uRight - uLeft, 1e-4f)
        assertEquals("the full height of it", 1f - 2 * kPlanInsetFraction, vTop - vBottom, 1e-4f)
    }

    @Test
    fun theConversionRoundTrips() {
        for (info in listOf(square, tall)) {
            for (p in listOf(100f to 200f, 0f to 0f, 300f to 511f)) {
                val x = info.arenaX + p.first
                val y = info.arenaY + p.second
                val (u, v) = landscapeToPlanFraction(x, y, info)
                val (backX, backY) = planFractionToLandscape(u, v, info)
                assertEquals(x, backX, 1e-2f)
                assertEquals(y, backY, 1e-2f)
            }
        }
    }

    @Test
    fun aDegenerateArenaDoesNotDivideByZero() {
        val empty = MiniMapInfo(0f, 0f, 0f, 0f, 0f, 0f, 0, 0)
        assertEquals(0f to 0f, landscapeToPlanFraction(5f, 5f, empty))
        assertEquals(0f to 0f, planFractionToLandscape(0.5f, 0.5f, empty))
    }

    @Test
    fun aHalfDrawnLineAbandonsItselfOnTheSameClock() {
        val pending = MapPoint(5f, 5f, t0)
        assertEquals(pending, expireMapPoint(pending, t0 + 2999))
        assertNull(expireMapPoint(pending, t0 + 3000))
        assertNull(expireMapPoint(null, t0))
    }
}
