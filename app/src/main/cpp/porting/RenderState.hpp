#ifndef __INCLUDE_RenderState_hpp_INCLUDE__
#define __INCLUDE_RenderState_hpp_INCLUDE__

// The other direction from EngineState.hpp: a value the GL thread publishes
// that the simulation side needs.
//
// Where the camera actually is, in engine coordinates (x, y, z=height) - the
// listener the sound mix is built around, exactly as upstream's
// Sound::listener_ follows MainCamera. The audio queue needs it to decide
// which of a burst of sounds are near enough to be worth a channel and how
// loud each one is (see SoundEventQueue.h).
//
// Published in engine coordinates rather than the renderer's own world axes
// so the caller does not have to know about the landscape-to-world mapping,
// which is a reflection in one axis and easy to get backwards.
//
// Answers false until a frame has been drawn, since there is no camera
// before that.
bool renderListenerEnginePosition( float& x, float& y, float& z );

#endif  // __INCLUDE_RenderState_hpp_INCLUDE__
