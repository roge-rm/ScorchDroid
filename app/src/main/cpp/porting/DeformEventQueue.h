#ifndef SCORCHDROID_DEFORM_EVENT_QUEUE_H
#define SCORCHDROID_DEFORM_EVENT_QUEUE_H

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
}

#endif  // SCORCHDROID_DEFORM_EVENT_QUEUE_H
