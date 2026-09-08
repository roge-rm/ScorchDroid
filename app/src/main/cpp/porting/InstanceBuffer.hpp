#ifndef __INCLUDE_InstanceBuffer_hpp_INCLUDE__
#define __INCLUDE_InstanceBuffer_hpp_INCLUDE__

#include <vector>

// M6 performance: the per-instance data for instanced drawing of landscape
// scenery, and the packing of it into the flat float array the instanced
// mesh shader's vertex attributes read.
//
// A landscape scatters up to ~2,000 non-tank targets, and the measurement
// that motivated this found they use *one* model between them - so they
// were ~2,000 draw calls of the same mesh with a different matrix each,
// which is the textbook case for instancing. Everything else the renderer
// draws is either already one call (terrain, sky, water, particles) or a
// handful of objects (tanks, shots, shields), and is deliberately left
// alone.
//
// Deliberately GL-free so host-tests can cover it. The GL calls around it
// cannot be tested here, but the packing is exactly where an ordering
// mistake would hide - a transposed pair of floats would put every tree in
// the wrong place, or tint them all black, with nothing to point at.
namespace ScorchDroidInstances
{
	// One drawn copy of a mesh. Not a full 4x4 matrix per instance: the
	// rotation is about the world up axis only and the scale is uniform, so
	// eight floats carry everything and the vertex shader rebuilds the
	// transform with a single sin/cos pair.
	struct Instance
	{
		// World position of the instance's origin, already including any
		// model-specific base lift - the caller knows the model, this does
		// not.
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float scale = 1.0f;
		// About the world up axis, matching Mat4::rotateY.
		float rotationRadians = 0.0f;
		// Flat colour, already multiplied by upstream's per-target
		// brightness. RGB rather than the single grey upstream uses for
		// models, because the procedural trees are tinted per kind (green,
		// snow-laden, burnt) and share this path.
		float r = 1.0f, g = 1.0f, b = 1.0f;
	};

	// Two vec4 attributes: (x, y, z, scale) then (rotation, r, g, b).
	const int kFloatsPerInstance = 8;

	// Appends [instances] to [out] in attribute order. Appends rather than
	// assigns so a caller can pack several buckets into one buffer; clear
	// [out] first for a single bucket.
	void pack(const std::vector<Instance> &instances, std::vector<float> &out);
}

#endif  // __INCLUDE_InstanceBuffer_hpp_INCLUDE__
