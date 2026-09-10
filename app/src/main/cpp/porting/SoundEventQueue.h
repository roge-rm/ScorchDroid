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
//
// The queue carries each sound's world position and its attenuation
// parameters, because without them there is no way to reproduce what
// upstream actually does with a burst of sounds. Upstream does not mix
// everything it is asked to: Sound::updateSources() (client/sound/Sound.cpp)
// sorts every playing source by priority and then by distance, hands out a
// fixed pool of channels to the winners, and stops the rest. A weapon like
// Death's Head, which detonates dozens of times, is bounded by that pool -
// where this port used to start an unbounded player per event, at full gain,
// which is painful rather than loud.
//
// Sounds carry upstream's own priority (VirtualSoundPriority), and the
// comparison is priority first, then distance - so a burst of explosions
// cannot drown out the beep telling you your turn is running out, and among
// equals the nearest wins. That is what drainSoundEvents implements.
//
// Every sound raised from src/common is eAction; the two that are not come
// from src/client, where the countdown and the chat notification both use
// eText.
namespace ScorchDroidAudio
{
	// How many sounds may play at once. Upstream's own default, from
	// OptionsDisplay's "SoundChannels" entry (OptionsDisplay.cpp:412):
	// Sound::init() creates exactly this many OpenAL sources up front, so a
	// ninth simultaneous sound physically cannot play.
	extern int soundChannels;
	const int kDefaultSoundChannels = 8;

	// Upstream's VirtualSoundSource constructor defaults. Every sound but
	// SoundAction's uses them, since SoundAction is the only site that reads
	// the weapon's own values.
	const float kDefaultGain              = 1.0f;
	const float kDefaultReferenceDistance = 75.0f;
	const float kDefaultRolloff           = 1.0f;

	// Upstream's VirtualSoundPriority, the two values this port raises.
	// Higher wins a channel; the rest of upstream's bands (environment,
	// missile, music, rotation) belong to client subsystems this port
	// either does not have or drives another way.
	const int kPriorityAction = 10000;
	const int kPriorityText   = 100;

	// A sound with no position - upstream's setRelative() case, which plays
	// at full gain wherever the listener is.
	void pushSoundEvent(const std::string &soundFile, int priority = kPriorityAction);

	// A sound at a point in the landscape, with upstream's default
	// attenuation. Coordinates are engine-space (x, y, z=height), exactly
	// what upstream passes to SoundUtils::playAbsoluteSound.
	void pushSoundEventAt(const std::string &soundFile, float x, float y, float z);

	// As above, but carrying the weapon's own gain, reference distance and
	// rolloff - the three values SoundAction sets and nothing else does.
	void pushSoundEventAt(const std::string &soundFile, float x, float y, float z,
		float gain, float referenceDistance, float rolloff);

	// One sound that should actually be played, and how loudly.
	struct SelectedSound
	{
		std::string file;
		float       gain;
		// Carried through to the player, not just used for the sort here.
		// A channel is held for as long as the sample lasts, and upstream's
		// own beep.wav is a 5-second file with 0.07s of audible beep at the
		// front and silence after it - so six countdown beeps can sit on
		// six channels saying nothing. Upstream is no different, and the
		// reason it does not matter there is exactly this priority: eText
		// yields to any eAction, so a round's explosions evict the silence.
		// Flatten that to gain alone and a point-blank explosion ranks
		// level with a beep.
		int         priority;
	};

	// Drains the queue and answers with the sounds that win a channel,
	// nearest first, each with its attenuated gain.
	//
	// Selection happens here rather than being left to the player for two
	// reasons. It needs the listener, which only the caller has. And the
	// tick that drains this queue runs every 100ms where upstream's client
	// draws at 60fps, so a burst upstream would have spread over six frames
	// arrives here as one batch - handing all of it to the player at once
	// would be an artifact of this port's tick rate, not upstream's mix.
	//
	// [haveListener] false means no camera has been published yet (nothing
	// has been drawn), in which case everything plays unattenuated rather
	// than being silently dropped.
	std::vector<SelectedSound> drainSoundEvents(
		bool haveListener, float listenerX, float listenerY, float listenerZ);
}

#endif  // SCORCHDROID_SOUND_EVENT_QUEUE_H
