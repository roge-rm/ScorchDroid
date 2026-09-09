#ifndef SCORCHDROID_AMBIENT_SOUND_H
#define SCORCHDROID_AMBIENT_SOUND_H

#include <string>
#include <vector>

class ScorchedContext;

// M21: the landscape's own ambient sound - waves on a shore, rain, birds in
// the trees.
//
// Upstream hangs these off the landscape's texture definition: tex*.xml lists
// <ambientsounds>, each naming a file like
// data/landscapes/ambientsoundwaves.xml, and each of those holds one or more
// <sound> blocks with a position, a timing and a file. Arizona gets ocean
// waves, the jungle maps get birdsong on a loop with the odd chirp over it,
// and a rainy map gets rain.
//
// Read from those files rather than from the parsed LandscapeSoundType
// objects the engine already holds, for two reasons: the file names and
// timings sit behind protected members with no accessors, and the whole
// sound layer is written around VirtualSoundSource, which is client-only.
// The data is the part worth having, and it is plain XML.
//
// What is *not* reproduced is position. Upstream places each source in the
// world - at the waterline with a falloff, or on the tree group - and mixes
// it by distance through OpenAL. This port has no spatial audio at all, so
// these play flat, at the gain the file asks for. That is a real difference
// and it is audible on a map where the sea is far away; it is still much
// closer to upstream than silence.
namespace ScorchDroidAmbient
{
	struct Sound
	{
		// Absolute path to the wav, already resolved through the mod.
		std::string file;
		float gain = 1.0f;
		// Upstream's two timings: looped plays continuously, repeat plays
		// once every so often (birdsong over the loop).
		bool looped = true;
		float minSeconds = 0.0f;
		float maxSeconds = 0.0f;
	};

	// The sounds one landscape's texture definition asks for, given its
	// tex*.xml. Split out from the call below so it can be checked against a
	// named landscape rather than whichever one a game happened to pick.
	std::vector<Sound> forTexFile(const std::string &texPath);

	// The sounds the currently loaded landscape asks for. Empty when there is
	// no landscape yet, or when it defines none - most of upstream's do
	// define some, but texblank is silent.
	std::vector<Sound> forCurrentLandscape(ScorchedContext &context);
}

#endif  // SCORCHDROID_AMBIENT_SOUND_H
