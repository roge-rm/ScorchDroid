#ifndef SCORCHDROID_TRACER_STORE_H
#define SCORCHDROID_TRACER_STORE_H

#include <cstdint>
#include <vector>

// Android build: upstream's ranging tracers. Two purchasable weapons use
// them - "Tracer" (<showendpoint>, a marker where the shot landed) and
// "Smoke Tracer" (<showshotpath>, the whole flight path) - and both leave
// their mark behind after the shot so you can range a target and still see
// where the ranging shot went while aiming the real one.
//
// Upstream keeps this in RenderTracer, a client singleton, and
// ShotProjectile's own recording of the path is inside #ifndef S3D_SERVER
// (the `positions_` member does not even exist in this build). So the
// recording is re-created here rather than by reinstating that member,
// which keeps the submodule patch to two small blocks in the .cpp with no
// header change.
//
// Coordinates are engine-space (x, y, height), like the other queues - the
// renderer swizzles on the way in.
namespace ScorchDroidTracer
{
	struct Point
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	// A path being recorded, keyed by the shot itself. Upstream samples the
	// position every 0.1s; the patched ShotProjectile does the same.
	void addPathPoint(uintptr_t shotId, unsigned int playerId, float x, float y, float z);

	// Moves that shot's path into the finished set. Called when the shot
	// collides - a path that never collides is dropped by clearAll().
	void finishPath(uintptr_t shotId);

	// A bare impact marker, for <showendpoint> weapons.
	void addEndPoint(unsigned int playerId, float x, float y, float z);

	// Everything one player has left behind. Upstream draws only the
	// current tank's tracers, in that tank's colour - they are a private
	// ranging aid, not a shared one - so this is queried per player.
	void getFor(unsigned int playerId,
				std::vector<Point> &endPoints,
				std::vector<std::vector<Point> > &paths);

	// Cleared at the start of each round, matching RenderTracer::newGame.
	void clearAll();
}

#endif  // SCORCHDROID_TRACER_STORE_H
