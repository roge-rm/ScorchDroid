#ifndef SCORCHDROID_SHORE_BREAKERS_H
#define SCORCHDROID_SHORE_BREAKERS_H

#include <vector>

class ScorchedContext;

// W10c: upstream's breakers - the animated wave sprites that wash up along
// every shoreline (WaterWaves.cpp). This is the GL-free half: finding where
// the shore is and cutting it into the segments the sprites are drawn on.
// The renderer turns the segments into quads and animates them.
//
// Upstream's method, kept step for step: every heightmap cell just under
// the waterline (within 4 units below it) with a neighbour above it is a
// shore point; the points are chained into paths by walking to a marked
// neighbour; each path is cut into segments of a random length, and each
// segment gets a perpendicular pushed 6 units out to sea (flipped if the
// first guess pointed at land). Segments are dealt at random between two
// sets, which upstream draws with two different sprite images.
namespace ScorchDroidBreakers
{
	struct Segment
	{
		// The four corners of the sprite's home quad and its seaward
		// perpendicular, in the engine's own landscape coordinates (x, y);
		// a, b are the two shore-side corners and c, d the seaward ones.
		float ax, ay, bx, by, cx, cy, dx, dy;
		float perpX, perpY;
		int   set;   // 0 or 1: which of the two sprite images
	};

	// Walks the current landscape's heightmap. Deterministic for a given
	// seed, so a test can assert on it and a rebuild after a crater does
	// not reshuffle every breaker on the map.
	std::vector<Segment> build(ScorchedContext &context, float waterHeight, unsigned int seed);
}

#endif  // SCORCHDROID_SHORE_BREAKERS_H
