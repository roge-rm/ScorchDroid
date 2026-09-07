#ifndef __INCLUDE_SkyDescription_hpp_INCLUDE__
#define __INCLUDE_SkyDescription_hpp_INCLUDE__

class ScorchedContext;

// M6: what the sky looks like for the landscape currently loaded.
//
// Upstream draws its sky with a whole client subsystem (SkyDome, Sun,
// Stars, Hemisphere, a cloud layer blown by the real wind), none of which
// survives the client/server split. Its *inputs* do: LandscapeTex is
// ordinary src/common landscape definition and carries the sky colour map,
// the time of day, the sun's two angles, the sun colour and the fog
// colour. This turns those into the handful of numbers a shader needs.
//
// Deliberately GL-free so host-tests can check it - the fiddly part is
// reading upstream's colour map correctly, which is exactly the kind of
// indexing that fails silently and looks merely "a bit off" on screen.
namespace ScorchDroidSky
{
	// Vertical sky gradient, horizon (0) to zenith (kGradientSteps - 1).
	// Upstream's own resolution: its colour map is 16 columns (time of day)
	// by 16 rows (height), sampled per hemisphere ring - see
	// Hemisphere::drawColored, which indexes
	// `daytime * components + 16 * components * heightIndex`.
	const int kGradientSteps = 16;

	struct Description
	{
		bool valid = false;

		float gradient[kGradientSteps][3];

		// Unit vector towards the sun in *landscape* axes (x, y, height),
		// from upstream's own Sun::setPosition:
		// (sin(xy)cos(yz), cos(xy)cos(yz), sin(yz)). Note the positive
		// sine on x - the sun's bearing is a clockwise compass angle like
		// the wind's, not the counter-clockwise one a shot is fired along.
		float sunDirection[3] = { 0.0f, 0.0f, 1.0f };
		float sunColor[3] = { 1.0f, 1.0f, 1.0f };

		// The sun as a *position* in landscape coordinates, which is what
		// upstream's light map bakes against: it takes the direction to the
		// sun per texel, so a point source at a finite distance, not a
		// parallel one. Upstream's own radius (900) and map-centre offset.
		float sunPosition[3] = { 0.0f, 0.0f, 900.0f };

		// <skyambience> and <skydiffuse>: the two terms upstream's light map
		// combines as `diffuse * lambert + ambience`.
		float ambience[3] = { 0.4f, 0.4f, 0.4f };
		float diffuse[3] = { 0.4f, 0.4f, 0.4f };

		// The colour upstream fogs distance towards, which is in practice
		// the haze colour at the horizon.
		float fog[3] = { 0.5f, 0.5f, 0.5f };

		// <nohorizonglow> in the landscape definition. Night maps set it,
		// and a glowing horizon under a moon looks wrong.
		bool horizonGlow = true;
	};

	// Returns a Description with valid=false if there is no landscape yet
	// or its colour map won't load; the caller should keep its fallback.
	Description describe(ScorchedContext &context);
}

#endif  // __INCLUDE_SkyDescription_hpp_INCLUDE__
