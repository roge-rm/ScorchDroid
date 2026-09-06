#ifndef SCORCHDROID_SOUND_EVENT_QUEUE_H
#define SCORCHDROID_SOUND_EVENT_QUEUE_H

#include <string>
#include <vector>

// Android build: SoundAction::simulate() (actions/SoundAction.cpp) is
// entirely #ifndef S3D_SERVER - sound playback is a client-presentation
// concern in upstream's architecture, same as rendering, so nothing plays
// sounds under our S3D_SERVER=1 build. This is the hook that lets
// SoundAction notify us anyway (a small, targeted patch - see
// patches/scorched3d/) so a new Android-side audio layer (SoundPool, not
// vendored OpenAL/OGG - see the porting plan) can react, the same way the
// GLES3 renderer reacts to live tank/landscape state instead of reusing
// GLW/GLEXT.
namespace ScorchDroidAudio
{
	void pushSoundEvent(const std::string &soundFile);

	// Drains and returns the queued sound file paths since the last call.
	// Called from the JNI tick loop (see engine_jni.cpp).
	std::vector<std::string> drainSoundEvents();
}

#endif  // SCORCHDROID_SOUND_EVENT_QUEUE_H
