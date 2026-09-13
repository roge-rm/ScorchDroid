#ifndef __INCLUDE_MiniMapBuilder_hpp_INCLUDE__
#define __INCLUDE_MiniMapBuilder_hpp_INCLUDE__

#include <LandscapeTextureBuilder.hpp>
#include <cstdint>
#include <vector>

class ScorchedContext;

// The picture behind the mini-map: upstream's plan view, which is its
// `Landscape::bitmapPlanAlpha_`.
//
// Upstream does not draw a second landscape for this. `Landscape::generate()`
// box-downsamples the same ground texture it stretches over the terrain
// (`gluScaleImage(mainMap_ -> bitmapPlan_)`, 1024 -> 128 at the default
// texture size) and then turns the water into *alpha* rather than colour, so
// the sea is a hole in the image and whatever sits behind the widget shows
// through. That is what gives the plan view its "island on a dark disc" look,
// and it is why this returns ARGB rather than RGB.
//
// So the expensive half is already done here too: renderer_jni keeps the built
// ground texture in `groundTextureData` (it has to, to burn scorch marks into
// it), and this only ever downsamples what is already in memory.
//
// GL-free and engine-free, for the same reason LandscapeTextureBuilder is:
// it makes the whole thing checkable in host-tests with no EGL context, which
// is where the alpha rule and the arena maths are pinned down.
namespace MiniMapBuilder
{
	// Upstream's plan size at the default texture size (`getPlanTexSize()`
	// returns 64 / 128 / 256 for its three texture-size settings). 128 is
	// plenty for a phone - the map is drawn at about 180dp - and the whole
	// buffer is 64KB.
	const int kDefaultSize = 128;

	struct Image
	{
		int size = 0;
		std::vector<uint32_t> argb;  // size * size, 0xAARRGGBB

		bool valid() const { return size > 0 && argb.size() == size_t(size) * size; }
	};

	// Just the heights, on their own grid.
	//
	// Deliberately not LandscapeTextureBuilder::Snapshot: that also carries a
	// normal per sample, which costs a HeightMap::getNormal for every cell and
	// is worth it for the ground texture's slope blending. Nothing here needs
	// a normal, and this is re-captured every time the terrain deforms.
	struct Heights
	{
		int width = 0, height = 0;   // cells; (width+1)*(height+1) samples
		std::vector<float> heights;

		bool valid() const { return width > 0 && height > 0; }
		float at(int x, int y) const;
	};

	// Copies the engine's current heightmap. Call with the engine lock held.
	//
	// Samples HeightMap::getHeight(int, int) rather than getInterpHeight():
	// the interpolating accessors keep static scratch vectors and are not safe
	// to call off the engine thread (the same trap LandscapeTextureBuilder's
	// Snapshot documents), and at 128 samples across a 256-cell map there is
	// nothing to interpolate anyway.
	Heights captureHeights(ScorchedContext &context);

	// The landscape's water level, or a large negative number when this
	// landscape has no water - which is how upstream spells "no water" here
	// too (`updatePlanATexture` passes -50.0f when the water is off).
	//
	// Read from the landscape definition rather than from the client's Water
	// class, which is client-only and so is not in this build. The idiom is
	// MovementMap::getWaterHeight()'s, minus its movement-restriction test:
	// the border is a LandscapeTexBorderWater and its `height` is the level.
	float waterHeight(ScorchedContext &context);

	// The build proper: GL-free and engine-free, safe on any thread.
	//
	// RGB is a box average of [ground] over each destination texel - [ground]
	// is the full landscape, so this is the whole map, not the arena; the
	// arena rectangle is a sub-rect of it and the caller crops with it, as
	// upstream's plan view does with its texture coordinates.
	//
	// Rows stay in landscape order (row 0 is landscape y = 0). The flip to
	// screen order happens once, in the one place that draws it, along with
	// the flip the tank markers need - keeping both in the same transform is
	// what stops them disagreeing.
	Image build(const LandscapeTextureBuilder::Texture &ground,
	            const Heights &heights,
	            float waterHeight,
	            int size = kDefaultSize);

	// Re-derives the alpha channel against a new heightmap, leaving the
	// colours alone.
	//
	// This is exactly, and only, what upstream re-does when the terrain
	// deforms: `Landscape::simulate()` calls `updatePlanATexture()` on its
	// deform timer, which re-runs the water rule against the new heights.
	// `mainMap_` is never regenerated and `bitmapPlan_` is never re-scaled, so
	// upstream's plan colours are effectively fixed for the round and only its
	// waterline moves. Blasting a channel below the waterline opens the sea
	// into it on the map; craters above the water change nothing.
	void updateWater(Image &image, const Heights &heights, float waterHeight);
}

#endif  // __INCLUDE_MiniMapBuilder_hpp_INCLUDE__
