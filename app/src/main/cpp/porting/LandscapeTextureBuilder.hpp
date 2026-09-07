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
}

#endif  // __INCLUDE_LandscapeTextureBuilder_hpp_INCLUDE__
