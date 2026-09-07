#ifndef __INCLUDE_LandscapeTextureBuilder_hpp_INCLUDE__
#define __INCLUDE_LandscapeTextureBuilder_hpp_INCLUDE__

#include <vector>
#include <string>

class ScorchedContext;

// M6: builds the landscape's ground texture - the RGB image that gets
// stretched over the terrain mesh so the ground reads as actual terrain
// (grass/rock/sand bands, cliffs, shoreline) instead of the flat green
// field the M6 slice-1 renderer drew.
//
// This is a from-scratch reimplementation of what upstream does in
// GLImageModifier::addHeightToBitmap(): blend the landscape definition's
// four ground textures by height, blend in `rockside` on steep slopes and
// `shore` at the waterline, with a little height noise so the bands don't
// look like contour lines. Upstream's version lives in src/client/GLEXT,
// i.e. the excluded fixed-function client layer, so it can't be reused -
// but the inputs can: LandscapeTex (the landscape definition's texture
// names) and ImageFactory (the png/jpg/bmp loaders) are both in
// src/common, which we compile.
//
// Deliberately GL-free and returning a plain pixel buffer, so the terrain
// texture can be generated and checked without an EGL context (see
// host-tests) - renderer_jni.cpp does the glTexImage2D upload separately.
namespace LandscapeTextureBuilder
{
	struct Texture
	{
		int width = 0;
		int height = 0;
		std::vector<unsigned char> rgb;  // width * height * 3

		bool valid() const { return width > 0 && height > 0 && rgb.size() == size_t(width) * height * 3; }
	};

	// Generates a `size` x `size` RGB ground texture for the landscape
	// currently loaded in [context]. Returns an invalid Texture if there's
	// no landscape yet, or if the definition doesn't use the generated
	// (texture0..3 + rockside + shore) style - see LandscapeTexType.
	//
	// [error], if given, receives a short reason on failure - there are
	// several distinct ways this can come up empty (no landscape, a
	// non-generated texture style, an image that wouldn't load) and they
	// need very different fixes, so the caller shouldn't have to guess.
	Texture build(ScorchedContext &context, int size, std::string *error = nullptr);

	// The rectangle of [texture] a scorch touched, in texture pixels, so
	// the caller can re-upload just that part.
	struct Rect
	{
		int x = 0, y = 0, width = 0, height = 0;

		bool valid() const { return width > 0 && height > 0; }
	};

	// Burns one scorch mark into an already-built ground texture, at a
	// blast centred on heightmap cell (centreX, centreY). A port of
	// DeformTextures::deformLandscape, which is client-only upstream.
	//
	// [textureName] is the weapon's own <deformtexture> if it named one;
	// empty falls back to the landscape definition's <scorch> image, which
	// is what upstream's ExplosionTextures::getScorchBitmap does. The image
	// is tiled across the landscape rather than fitted to the blast, again
	// matching upstream - so neighbouring craters get visibly different
	// patches of it instead of the same stamp repeated.
	//
	// Two deliberate differences from upstream, neither visual:
	//  - The falloff is evaluated directly per pixel rather than bilinearly
	//    interpolated out of DeformLandscape's precomputed 100x100 map.
	//    That map's values are a pure function of the radius (see
	//    DeformLandscapeCache), so this computes the same curve without
	//    having to ship 80KB per blast through the event queue.
	//  - Blending is straight CPU work on the pixel buffer; upstream folds
	//    it into a glTexSubImage2D of its own. Keeping it GL-free is what
	//    lets host-tests check it.
	//
	// Returns the touched rectangle, or an invalid Rect if the blast missed
	// the texture entirely or the scorch image wouldn't load.
	Rect applyScorch(ScorchedContext &context, Texture &texture,
					 int centreX, int centreY, float radius,
					 const std::string &textureName);

	// Where a tank with a position-selecting weapon (Fuel, Rocket Fuel,
	// Teleport) is allowed to go, drawn over the ground the way upstream's
	// MovementMap::movementTexture does it: reachable ground at full
	// brightness, everything else at quarter brightness, and a red line
	// along the boundary between them.
	//
	// [mask] is one byte per landscape square, row-major by landscape y,
	// non-zero where the tank can move to - see MovementStore.h for who
	// fills it in. Returns a tinted copy rather than modifying [source],
	// because the untinted texture has to survive to be put back when the
	// weapon is switched away, and it keeps accumulating scorch marks in
	// the meantime.
	//
	// Upstream compares each texel against its right and lower neighbour to
	// find the boundary, which is what gives the outline its one-texel
	// width regardless of how coarse the landscape grid is; this does the
	// same.
	Texture applyMovementMask(const Texture &source,
							  const unsigned char *mask, int maskWidth, int maskHeight);

	// Bakes the sun's lighting - including terrain shadowing itself - into
	// an already-built ground texture. A port of upstream's
	// ImageModifier::addLightMapToBitmap, which is client-only, and which
	// upstream calls in exactly this place (Landscape.cpp, right after the
	// texture is generated) on any machine without hardware shadows.
	//
	// Per texel: the lambert term against the interpolated ground normal,
	// then a march along the ray towards the sun looking for terrain in the
	// way. Blocking terrain that only just clips the ray softens the shadow
	// in proportion; anything deeper is fully dark. The result is
	// `diffuse * light + ambience`, multiplied into the texture.
	//
	// Baking rather than shading per fragment is upstream's own choice and
	// a good one here: hills shadow each other, the cost is paid once per
	// landscape, and the phone does nothing per frame. It does mean the
	// terrain must then be drawn *unlit* or the lighting lands twice.
	//
	// Returns false if there is no landscape or the texture is unusable.
	bool applyLightMap(ScorchedContext &context, Texture &texture);

	// How close each landscape cell is to the shoreline, for the water to
	// draw foam along. One byte per cell of a [size] x [size] grid, 255 at
	// the water's edge falling to 0 by [reach] units of depth.
	//
	// Upstream finds the shore the same way (WaterWaveDistance::generate:
	// ground within a few units *below* the waterline, whose neighbours
	// cross it) and uses it to place wave sprites. This bakes the whole
	// band into a mask instead, which the water shader can sample - no
	// sprites to place, and it follows the coast a crater carves.
	//
	// GL-free, so host-tests cover it. Empty if there is no landscape.
	std::vector<unsigned char> buildShoreMask(ScorchedContext &context,
											  float waterHeight, int size,
											  float reach = 3.0f);
}

#endif  // __INCLUDE_LandscapeTextureBuilder_hpp_INCLUDE__
