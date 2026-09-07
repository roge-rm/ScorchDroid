#ifndef __INCLUDE_EngineState_hpp_INCLUDE__
#define __INCLUDE_EngineState_hpp_INCLUDE__

class ScorchedContext;

// M5 Phase 2: the single ScorchedContext this process is driving - either
// ScorchedServer (hosting) or a ClientContext (joined as a client), or
// nullptr if neither role has reached a usable state yet (see engine_jni.cpp's
// EngineMode/activeContext()). Shared between engine_jni.cpp (the
// simulation thread, tickEngine/fireWeapon/...) and renderer_jni.cpp (the
// GL thread) so the renderer can draw whichever role's live state exists,
// the same way engine_jni.cpp's own JNI surface already does - both read
// through the common ScorchedContext base (target container, landscape
// maps, ...), so nothing renderer-side needs to know which role is active.
ScorchedContext *engineActiveContext();

// M5: this process's own destinationId (see engine_jni.cpp's findMyTank())
// - kHumanDestinationId when hosting, or the real destinationId the host
// assigned us (ClientContext::getMyDestinationId()) when joined as a
// client. Lets the renderer tell "my tank" apart from everyone else's
// (previously drawn as identical, undifferentiated dots - not obvious
// which one was actually yours once a real opponent's tank was on screen
// too).
unsigned int engineMyDestinationId();

#endif  // __INCLUDE_EngineState_hpp_INCLUDE__
