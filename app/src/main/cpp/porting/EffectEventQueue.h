#ifndef SCORCHDROID_EFFECT_EVENT_QUEUE_H
#define SCORCHDROID_EFFECT_EVENT_QUEUE_H

#include <vector>

// Android build: every weapon effect upstream draws - explosions, napalm
// fires, laser beams, lightning arcs, shield impacts - is raised by an
// Action in src/common/actions, which this port compiles and simulates
// correctly. What it cannot reuse is the drawing: each Action's visual
// half sits behind #ifndef S3D_SERVER and is written against the excluded
// fixed-function client (GLState, ExplosionTextures, ParticleEmitter,
// Landscape::getSmoke, gluQuadric...).
//
// So the events are the reusable part and only the presentation is ours to
// write - the same split already used for sound (SoundEventQueue, patch
// 0006) and terrain deformation (DeformEventQueue, patch 0010). The
// patched actions push one record here when they fire; the renderer drains
// it and animates whatever it likes, with no engine knowledge flowing back
// the other way.
//
// COORDINATES ARE ENGINE-SPACE, NOT RENDER-SPACE: (x, y, height), matching
// FixedVector everywhere in src/common, where index 2 is the vertical axis.
// The renderer draws in a y-up world and swizzles on the way in. Keeping
// the queue in the engine's own convention means the patched actions can
// push their vectors unchanged, with no chance of a component getting
// silently reordered inside a submodule patch.
namespace ScorchDroidEffects
{
	enum EffectType
	{
		eExplosion = 0,  // expanding fireball; size is the blast radius
		eNapalm,         // one burning patch; size is the flame radius
		eLaser,          // beam from position to endPosition
		eLightning,      // one arc segment, position to endPosition
		eShieldHit,      // flash on a shield sphere; size is the shield radius
		eSkyFlash,       // whole-sky flash (a nuke); position is unused
		eTeleport,       // a tank arriving or leaving; size is its radius
	};

	struct EffectEvent
	{
		EffectType type = eExplosion;

		// Engine-space (x, y, height). endPosition is only meaningful for
		// the beam types (eLaser, eLightning).
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float endX = 0.0f, endY = 0.0f, endZ = 0.0f;

		// World units. What it measures depends on the type - see above.
		float size = 1.0f;

		// 0..1 RGB. Explosions carry the weapon's own <explosioncolour>;
		// the rest get a sensible constant from the pushing site, since
		// upstream's equivalent colour lives in a texture we don't load.
		float r = 1.0f, g = 1.0f, b = 1.0f;
	};

	void push(const EffectEvent &effect);

	// Drains everything queued since the last call. Called once per frame
	// from the render thread (see renderer_jni.cpp).
	std::vector<EffectEvent> drain();

	// Bounded: a sustained weapon raises effects every simulation step, and
	// anything arriving faster than the renderer drains it is dropped
	// rather than allowed to grow without limit. Dropping is safe because
	// these are momentary visuals with no gameplay meaning - unlike the
	// deform queue, nothing downstream depends on seeing every one.
	size_t maxQueued();
}

#endif  // SCORCHDROID_EFFECT_EVENT_QUEUE_H
