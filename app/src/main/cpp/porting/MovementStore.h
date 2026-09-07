#ifndef SCORCHDROID_MOVEMENT_STORE_H
#define SCORCHDROID_MOVEMENT_STORE_H

#include <vector>

// Android build: where "my tank" is allowed to move to, for the renderer to
// paint over the ground.
//
// Upstream computes this in TankWeaponSwitcher::switchWeapon the moment a
// position-selecting weapon (Fuel, Rocket Fuel, Teleport) becomes current:
// a MovementMap flood-fill limited by the fuel available, painted into the
// landscape texture by MovementMap::movementTexture(). Both the paint and
// the switch hook are inside `#ifndef S3D_SERVER`, so neither exists in
// this build - the flood fill itself (MovementMap) does, being ordinary
// src/common game logic that the server needs too.
//
// So the mask is recomputed on the simulation side (engine_jni.cpp, which
// knows which tank is ours and when the answer would change) and left here
// for the GL thread to pick up, in the same "publish a small snapshot,
// don't reach across threads" shape as the deform/effect/tracer queues.
//
// One byte per landscape square rather than a bitset: the map is 256x256 at
// most, the renderer indexes it per texture pixel, and a byte keeps that
// inner loop free of shifting and masking.
namespace ScorchDroidMovement
{
	// Replaces the published mask. [reachable] is width * height bytes,
	// row-major by y, 1 where the tank can move to and 0 everywhere else.
	void publish(int width, int height, std::vector<unsigned char> reachable);

	// No position-selecting weapon is current any more - the renderer
	// should put the plain ground texture back.
	void clear();

	// Bumped by every publish() and by a clear() that actually cleared, so
	// the GL thread can tell "nothing to do" from "rebuild" with one read
	// and without copying the mask every frame.
	unsigned int version();

	// Copies the current mask out. Returns false (leaving the arguments
	// untouched) when nothing is published.
	bool get(int &width, int &height, std::vector<unsigned char> &reachable);
}

#endif  // SCORCHDROID_MOVEMENT_STORE_H
