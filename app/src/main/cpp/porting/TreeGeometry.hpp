#ifndef __INCLUDE_TreeGeometry_hpp_INCLUDE__
#define __INCLUDE_TreeGeometry_hpp_INCLUDE__

#include <3dsparse/TreeModelFactory.hpp>
#include <vector>

// M6: upstream's real trees.
//
// Trees are the one thing on a landscape that is neither a model file nor
// part of the terrain: TreeModelFactory::createModel returns an *empty*
// Model - a bounding box and nothing else - and the geometry is built
// procedurally at draw time by src/client/graph/ModelRendererTree.cpp, which
// is client-only and so absent from this build. This is a port of that
// generation, minus the fixed-function drawing.
//
// What makes upstream's trees look like trees rather than cones, and what
// this reproduces:
//
//  - The foliage is *textured from an atlas* of pine/palm/oak rosettes
//    photographed from above, with a matching alpha mask that cuts a ragged
//    needle silhouette out of each branch layer. That mask is most of the
//    difference: without it a branch layer is a smooth cone edge.
//  - Texture coordinates sweep radially around the atlas cell, so a layer
//    reads as foliage seen from above.
//  - Every rim normal is jittered by +-20 degrees, so neighbouring fronds
//    catch the light differently instead of forming a smooth shaded cone.
//  - There are 28 tree types, not one - pine, pine2-4 and their snow
//    variants, yellow, light, burnt, palm, palmB1-7 and oak1-4 - each with
//    its own atlas cell and proportions.
//
// **Deliberate deviation**: upstream calls RAND (rand()/RAND_MAX) while
// building, so its trees differ run to run. This uses a small seeded
// generator instead, seeded per tree type, so the geometry is reproducible -
// which is what lets host-tests assert anything about it at all, and means a
// landscape looks the same twice. The jitter is still there; it is just no
// longer a function of global rand() state.
//
// Deliberately GL-free, like LandscapeTextureBuilder and InstanceBuffer, for
// the same reason: this is where a transposed axis or a mis-scaled texture
// coordinate would hide, and none of it needs a GPU to check.
namespace ScorchDroidTrees
{
	// Which texture atlas a type samples. Upstream keeps five
	// GLTextureReferences; note that its "palm A" set actually samples the
	// *pine* image, which is not a mistake here but upstream's own choice.
	enum Atlas
	{
		eAtlasPineA = 0,  // data/textures/pine2.bmp  + pine2a.bmp
		eAtlasPineB,      // data/textures/pine3.bmp  + pine3a.bmp
		eAtlasPalmA,      // data/textures/pine.bmp   + pinea.bmp
		eAtlasPalmB,      // data/textures/palm2.bmp  + palm2a.bmp
		eAtlasOak,        // data/textures/oak.bmp    + oaka.bmp
		eAtlasCount
	};

	// The image and mask each atlas loads, as mod-relative paths.
	const char *atlasImage(Atlas atlas);
	const char *atlasMask(Atlas atlas);

	// Which atlas the given tree type draws with.
	Atlas atlasFor(TreeModelFactory::TreeType type);

	// Upstream multiplies the burnt types by a flat 0.3 grey
	// (glColor3f(0.3, 0.3, 0.3) at the head of their display lists).
	bool isBurnt(TreeModelFactory::TreeType type);

	// Eight floats per vertex: position, normal, texture coordinate - the
	// same layout the instanced mesh path already uses, plus the UV.
	const int kFloatsPerVertex = 8;

	// Appends the tree's triangles to [out] and returns the vertex count
	// added. Vertices are in the renderer's Y-up space: upstream builds
	// these Z-up like its models, so the same (x, y, z) -> (x, z, -y) remap
	// uploadMeshGroup applies is applied here, and for the same reason - the
	// alternative is a reflection, which would wind every face backwards.
	//
	// Returns 0 for a type with no geometry.
	int build(TreeModelFactory::TreeType type, std::vector<float> &out);
}

#endif  // __INCLUDE_TreeGeometry_hpp_INCLUDE__
