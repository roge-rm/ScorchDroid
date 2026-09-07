#ifndef SCORCHDROID_DEFORM_EVENT_QUEUE_H
#define SCORCHDROID_DEFORM_EVENT_QUEUE_H

#include <string>
#include <vector>

// Android build: DeformLandscape really does carve craters into the shared
// heightmap under S3D_SERVER (the simulation half is not client-only), but
// the *notification* that it happened - Landscape::recalculateLandscape()
// plus VisibilityPatchGrid::recalculateLandscapeErrors(pos, radius) - lives
// entirely inside #ifndef S3D_SERVER, so nothing tells a renderer the ground
// changed shape. Without a hook the terrain mesh is built once per round and
// craters are simply invisible.
//
// This is the same shape of problem as the client-only sound trigger (see
// SoundEventQueue.h and patches/scorched3d/0006-*), so it gets the same
// shape of fix: a tiny queue the patched engine pushes to, drained by the
// presentation layer - here the GL renderer, which rebuilds only the
// affected part of the terrain mesh.
//
// Events are coalesced into a single dirty rectangle rather than kept as a
// list: the renderer would union them per frame anyway, and a bounded rect
// can never grow unboundedly the way a queue can if nobody drains it.
// Coordinates are heightmap grid coordinates (the same integer space
// HeightMap::getHeight(x, y) uses), NOT world units - they happen to be the
// same scale, but keeping the units explicit avoids the confusion that
// caused when the renderer's terrain grid was introduced.
namespace ScorchDroidLandscape
{
	// Called from the patched DeformLandscape (see the patch series) with
	// the crater/flatten centre and its radius in heightmap cells.
	void pushDeform(int centreX, int centreY, int radius);

	// Drains the accumulated dirty region. Returns false (leaving the
	// out-params untouched) if nothing has deformed since the last call, so
	// the common no-deform frame costs one mutex and one bool.
	bool takeDirtyRegion(int &minX, int &minY, int &maxX, int &maxY);

	// Forgets any pending region - called when the whole landscape is
	// rebuilt anyway (a new round), so stale coordinates from the previous
	// landscape can't drive a partial update of the new one.
	void clearDirtyRegion();

	// One scorch mark: the burnt patch upstream paints into the landscape
	// texture at a blast site (DeformTextures::deformLandscape, client-only
	// like the rest of this). Unlike the mesh region above these are NOT
	// coalesced - each mark is a separate blend at its own position, with
	// its own radius and its own image, so merging them would be wrong.
	struct ScorchEvent
	{
		int         centreX = 0;  // heightmap cells, like the region above
		int         centreY = 0;
		float       radius = 0.0f;
		std::string texture;      // weapon's <deformtexture>; empty means the landscape's own
	};

	// Called from the patched DeformLandscape when a deform that names a
	// texture actually moved ground. Note the deform points map upstream
	// passes to DeformTextures is not carried: its contents are a pure
	// function of the radius (see DeformLandscapeCache), so the consumer
	// recomputes the falloff instead of copying a 100x100 fixed array per
	// blast across the queue.
	void pushScorch(int centreX, int centreY, float radius, const char *texture);

	// Drains the queued scorch marks. Bounded - a sustained weapon can
	// deform every simulation step, and marks that arrive faster than they
	// can be painted are dropped rather than allowed to grow without limit.
	std::vector<ScorchEvent> drainScorchEvents();
}

#endif  // SCORCHDROID_DEFORM_EVENT_QUEUE_H
