#ifndef SCORCHDROID_PARTICLE_TEXTURES_H
#define SCORCHDROID_PARTICLE_TEXTURES_H

#include <string>
#include <vector>

// V1: upstream's particle texture sets. Its ExplosionTextures reads
// data/textureset.xml into named sets of images - the animated explosions
// (exp00..exp08, ten frames each), the teleport (trans), the napalm flames
// (33 frames), smoke, the ring, the generic particle - and every particle
// is a camera-facing quad drawn with one of them, chosen by age when the
// set is animated. Each image is loaded as its own alpha (ImageFactory::
// loadImage(file, file)), so a black background is transparent.
//
// This is the GL-free half: the sets as one stack of 128-square RGBA
// layers for a 2D array texture, plus the name -> layer range table. Layer
// 0 is a procedural soft disc for the few particles that have no upstream
// texture (rain is drawn as streaks; snow and splash spray use the disc).
namespace ScorchDroidParticleTextures
{
	const int kSize = 128;

	struct Set
	{
		std::string name;
		int firstLayer = 0;
		int count = 0;
	};

	struct Atlas
	{
		int layers = 0;
		std::vector<unsigned char> rgba;   // layers * kSize * kSize * 4
		std::vector<Set> sets;
		bool valid() const { return layers > 0 && rgba.size() == (size_t) layers * kSize * kSize * 4; }
		// The set, or nullptr.
		const Set *find(const std::string &name) const;
	};

	// Reads data/textureset.xml from the mod data, plus talk.bmp (which
	// upstream keeps outside the sets) as the set "talk". Empty sets are
	// left out; a missing file is logged and skipped.
	Atlas load();
}

#endif  // SCORCHDROID_PARTICLE_TEXTURES_H
