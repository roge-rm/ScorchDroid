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

// As above, and the listener's right vector in the same space - what a sound
// needs to be placed across the stereo field. Upstream hands OpenAL the
// camera's orientation every frame; this is the same information, in the one
// form the port's player can use.
bool renderListenerEngineBasis( float& x, float& y, float& z,
								float& rightX, float& rightY, float& rightZ );

#endif  // __INCLUDE_RenderState_hpp_INCLUDE__
