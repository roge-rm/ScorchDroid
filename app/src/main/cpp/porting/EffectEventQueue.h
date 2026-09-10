#ifndef SCORCHDROID_EFFECT_EVENT_QUEUE_H
#define SCORCHDROID_EFFECT_EVENT_QUEUE_H

#include <string>
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
		// A WeaponAnimation's own effect - upstream's only one is
		// ExplosionLaserBeamRenderer, the blue column a tank can go up in
		// when it dies. size is the column's radius.
		eDeathBeam,
		eShieldHit,      // flash on a shield sphere; size is the shield radius
		eSkyFlash,       // whole-sky flash (a nuke); position is unused
		eTeleport,       // a tank arriving or leaving; size is its radius
		// One lingering smoke puff, upstream's Landscape::getSmoke(). Grey,
		// slow, rising, and alpha-blended rather than additive - it is the
		// only effect here that darkens what is behind it instead of
		// lighting it. Raised from three places, each with its own gating
		// and cadence that this port reproduces rather than re-invents:
		// a gun's muzzle flash, a napalm fire, and a tank driving.
		eSmoke,
		// The mushroom cloud a nuke-class weapon raises. Distinct from
		// eExplosion: upstream draws both, the cloud only for weapons whose
		// <createmushroomamount> wins its roll. size is the blast size.
		eMushroom,
		// One thrown rock. Upstream's explosions fling opaque tumbling rock
		// meshes alongside the fireball; being solid and dark they read
		// against bright ground where an additive spark cannot. size is the
		// blast size; one event per rock.
		eDebris,
		// A shot striking the arena boundary wall. `value` carries which
		// side (OptionsTransient::WallSide), since the flash lies in that
		// wall's plane.
		eWallHit,
		// A speech bubble over a tank that just said something.
		eTalk,
		// A floating damage number over a target that was just hurt.
		// `value` is the amount.
		eDamage,
		// Napalm's actual fire, one event per particle upstream emits when a
		// burning point is added (three per point, at its own offsets) -
		// distinct from eNapalm, which is the per-tick flicker this port
		// raises from the same block upstream raises its smoke from. These
		// are the long-lived ones: `value` carries the burn time, which for
		// a standard napalm is four seconds against the flicker's one, and
		// it is having a hundred points' worth of them alight at once that
		// makes a napalm field read as fire rather than as sparks.
		eNapalmFire,
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

		// Type-specific scalar: the damage amount for eDamage, the wall
		// side for eWallHit. Unused by the rest.
		float value = 0.0f;

		// 0..1 RGB. Explosions carry the weapon's own <explosioncolour>;
		// the rest get a sensible constant from the pushing site.
		float r = 1.0f, g = 1.0f, b = 1.0f;

		// V1: upstream's texture set for the particles this effect raises
		// (a name from data/textureset.xml: the weapon's <explosiontexture>,
		// <napalmtexture>...), whether the set animates over the particle's
		// life, the particle life range, whether it is drawn additively
		// (<luminance>) and whether the wind blows it. explosionType is
		// ExplosionParams::ExplosionType; for the ring types the axis rides
		// in endX/endY/endZ.
		std::string texture;
		float life1 = 0.0f, life2 = 0.0f;
		bool animate = false;
		bool additive = true;
		bool windAffected = false;
		int explosionType = 0;
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
