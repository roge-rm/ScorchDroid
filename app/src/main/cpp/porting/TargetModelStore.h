#ifndef SCORCHDROID_TARGET_MODEL_STORE_H
#define SCORCHDROID_TARGET_MODEL_STORE_H

#include <common/ModelID.hpp>

// Android build: how to draw a non-tank target - trees, buildings, ships,
// and everything else a landscape scatters about.
//
// The simulation half of these is entirely present: TargetContainer holds
// them, shots collide with them, MovementMap refuses to path through them,
// and they burn and fall. Only the *appearance* is missing, and only
// because of where upstream keeps it: TargetDefinition::createTarget works
// out the model, its scale, its brightness and its rotation, and then hands
// all four to `new TargetRendererImplTarget(...)` inside `#ifndef
// S3D_SERVER`. In this build that whole block is compiled out, so the
// values are computed and immediately discarded, and the target has no
// renderer to ask.
//
// So the hook (patch 0013) keeps them here instead, keyed by the target's
// player id, and the renderer looks them up. Same shape as the sound,
// deform, effect and tracer hooks: the patch adds an `#else` beside an
// existing split and nothing more.
//
// The ModelID is stored rather than a loaded Model: loading is the
// renderer's business (it caches per Model* already), it needs a GL thread
// nowhere near this call, and a landscape can scatter hundreds of targets
// that share a handful of models.
namespace ScorchDroidTargets
{
	struct Info
	{
		ModelID model;
		// The definition's own scale, which is *not* the tank sizing rule -
		// a building is meant to dwarf a tank.
		float scale = 1.0f;
		// Grey multiplier upstream calls "color", randomised per target
		// when the definition doesn't fix it, so a copse of identical trees
		// doesn't look stamped.
		float brightness = 1.0f;
		// Degrees about the landscape's up axis, already resolved from the
		// definition's rotation/rotationsnap.
		float rotationDegrees = 0.0f;
		// Upstream draws these as camera-facing sprites rather than meshes.
		bool billboard = false;
	};

	void push(unsigned int playerId, const ModelID &model,
			  float scale, float brightness, float rotationDegrees, bool billboard);

	// False when nothing was recorded for that target - a tank, say, which
	// is created by TankAddSimAction and never goes through a
	// TargetDefinition at all.
	bool get(unsigned int playerId, Info &out);

	// Number of targets recorded, for tests.
	size_t size();

	// Forget everything. Called when a game ends: these are keyed by
	// playerId, and the next game hands the same ids to entirely different
	// targets, so a surviving entry would draw the previous game's model.
	void clear();
}

#endif  // SCORCHDROID_TARGET_MODEL_STORE_H
