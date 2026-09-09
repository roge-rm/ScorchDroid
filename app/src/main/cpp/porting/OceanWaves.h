#ifndef SCORCHDROID_OCEAN_WAVES_H
#define SCORCHDROID_OCEAN_WAVES_H

#include <vector>

// W4: upstream's ocean, generated the way it generates it.
//
// Scorched3D's water is not a sum of sine waves. Its Water2 subsystem builds
// the surface from a Tessendorf spectrum - the "Simulating Ocean Water"
// method, by way of Danger from the Deep, whose ocean_wave_generator.hpp it
// vendors - and inverse-FFTs it into a tile that repeats across the sea. That
// is what gives it the irregular, wind-driven crests you cannot get from two
// sines however you add them.
//
// This is the same algorithm, at upstream's own resolution (128) and tile
// size (256 world units), with its own small FFT rather than the FFTW
// dependency upstream links against - a 128-point radix-2 transform is a
// page of code and one fewer library to carry onto Android.
//
// GL-free on purpose: the arithmetic is the part that goes subtly wrong, and
// this way host-tests can check it. What the renderer does with the result -
// a texture the water shader samples - is its own business.
namespace ScorchDroidOcean
{
	// Upstream's own, from Water2Constants.hpp: a 128x128 tile covering 256
	// world units, repeated across the water.
	const int kResolution = 128;
	const float kTileLength = 256.0f;

	struct Tile
	{
		// Row-major, kResolution^2, all in world units at upstream's own
		// scale (so the sea grows with the wind, from a few tenths of a
		// unit in a calm to about four in a gale).
		//
		// Height is the displacement about the water plane. dispX and
		// dispZ are the *horizontal* displacement of the same point - the
		// "choppy" term of Tessendorf's method, which upstream applies
		// through compute_displacements(-2): a point is pulled sideways
		// towards the crest, which is what makes crests sharp and troughs
		// broad rather than the surface a sum of smooth sines. Both are
		// in the tile's own frame, x along the tile's x and z along its y
		// (which the renderer lays along world z).
		std::vector<float> height;
		std::vector<float> dispX;
		std::vector<float> dispZ;

		// The unit normal of the *displaced* surface at each point, as
		// Water2Patch builds it: cross products across the four neighbours
		// two units away. Not an analytic slope of the height field - once
		// the points move sideways that is no longer the surface's slope.
		// Stored Y-up, in the renderer's world frame (x, up, z), so the
		// shader uses it as it is. This is also what upstream's
		// Water2Patches::generateNormalMap bakes into its normal map.
		std::vector<float> normalX;
		std::vector<float> normalY;
		std::vector<float> normalZ;
	};

	// Builds the wave spectrum for a given wind. Deterministic for a given
	// seed, so the same landscape can look the same twice, and so a test can
	// assert on it.
	//
	// windSpeed is upstream's own parameter in its own units (its default is
	// around 10); windDirection is the bearing the wind blows towards, in
	// radians, in the *tile's* frame: x along the tile's x, and y along the
	// tile's y, which the renderer lays along world z. World z runs the
	// opposite way to the engine's y, so a caller with an engine wind
	// vector passes atan2(dir.x, -dir.y), not atan2(dir.x, dir.y).
	void reseed(float windSpeed, float windDirectionRadians, unsigned int seed);

	// Advances to [seconds] and fills [out] with that moment's surface. The
	// spectrum is fixed; time only rotates each wave's phase, which is why
	// this can be called at whatever rate the device can afford without the
	// sea changing character.
	void generate(float seconds, Tile &out);
}

#endif  // SCORCHDROID_OCEAN_WAVES_H
