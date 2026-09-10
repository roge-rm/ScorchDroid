#include "SoundEventQueue.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace ScorchDroidAudio
{
	int soundChannels = kDefaultSoundChannels;

	namespace
	{
		struct QueuedSound
		{
			std::string file;
			int         priority;
			bool        positioned;
			float       x, y, z;
			float       gain;
			float       referenceDistance;
			float       rolloff;
			// Filled in at drain time, once the listener is known.
			float       distance;
		};

		std::mutex queueMutex;
		std::vector<QueuedSound> queue;
		const size_t kMaxQueueSize = 64;  // Drop oldest if the JNI side stops draining.

		void push(const QueuedSound &sound)
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			if (queue.size() >= kMaxQueueSize) {
				queue.erase(queue.begin());
			}
			queue.push_back(sound);
		}

		// OpenAL's AL_INVERSE_DISTANCE model, which is the one upstream
		// selects (Sound::init calls alDistanceModel(AL_INVERSE_DISTANCE)):
		//
		//   gain = referenceDistance /
		//          (referenceDistance + rolloff * (distance - referenceDistance))
		//
		// Clamped to the source gain at the top, as AL_MAX_GAIN does, so a
		// sound closer than its reference distance is not amplified.
		float attenuate(const QueuedSound &sound)
		{
			if (!sound.positioned) return sound.gain;

			const float refDist = (sound.referenceDistance > 0.0f)
				? sound.referenceDistance : kDefaultReferenceDistance;
			const float denominator =
				refDist + sound.rolloff * (sound.distance - refDist);
			if (denominator <= 0.0f) return sound.gain;

			const float falloff = refDist / denominator;
			return sound.gain * std::min(1.0f, std::max(0.0f, falloff));
		}
	}

	void pushSoundEvent(const std::string &soundFile, int priority)
	{
		QueuedSound sound;
		sound.file              = soundFile;
		sound.priority          = priority;
		sound.positioned        = false;
		sound.x = sound.y = sound.z = 0.0f;
		sound.gain              = kDefaultGain;
		sound.referenceDistance = kDefaultReferenceDistance;
		sound.rolloff           = kDefaultRolloff;
		sound.distance          = 0.0f;
		push(sound);
	}

	void pushSoundEventAt(const std::string &soundFile, float x, float y, float z)
	{
		pushSoundEventAt(soundFile, x, y, z,
			kDefaultGain, kDefaultReferenceDistance, kDefaultRolloff);
	}

	void pushSoundEventAt(const std::string &soundFile, float x, float y, float z,
		float gain, float referenceDistance, float rolloff)
	{
		QueuedSound sound;
		sound.file              = soundFile;
		sound.priority          = kPriorityAction;
		sound.positioned        = true;
		sound.x                 = x;
		sound.y                 = y;
		sound.z                 = z;
		sound.gain              = gain;
		sound.referenceDistance = referenceDistance;
		sound.rolloff           = rolloff;
		sound.distance          = 0.0f;
		push(sound);
	}

	std::vector<SelectedSound> drainSoundEvents(
		bool haveListener, float listenerX, float listenerY, float listenerZ)
	{
		std::vector<QueuedSound> drained;
		{
			std::lock_guard<std::mutex> lock(queueMutex);
			drained.swap(queue);
		}

		for (size_t i = 0; i < drained.size(); i++) {
			QueuedSound &sound = drained[i];
			if (!haveListener) {
				// Nothing drawn yet, so there is no listener to be far from.
				sound.positioned = false;
				continue;
			}
			if (!sound.positioned) continue;
			const float dx = sound.x - listenerX;
			const float dy = sound.y - listenerY;
			const float dz = sound.z - listenerZ;
			sound.distance = sqrtf(dx * dx + dy * dy + dz * dz);
		}

		// Upstream's own ordering (Sound.cpp's lt_virt, read the other way
		// up since it iterates its sort in reverse): priority decides, and
		// distance breaks the tie. So the beep that says your turn is
		// running out is not lost under a burst of explosions, and among
		// sounds of equal standing the nearest wins. A positionless sound
		// is upstream's relative case - it sits at the listener, so it
		// sorts first among its own priority.
		std::stable_sort(drained.begin(), drained.end(),
			[](const QueuedSound &a, const QueuedSound &b) {
				if (a.priority != b.priority) return a.priority > b.priority;
				return a.distance < b.distance;
			});

		const size_t channels = (soundChannels > 0)
			? (size_t) soundChannels : (size_t) kDefaultSoundChannels;
		if (drained.size() > channels) drained.resize(channels);

		std::vector<SelectedSound> selected;
		selected.reserve(drained.size());
		for (size_t i = 0; i < drained.size(); i++) {
			const float gain = attenuate(drained[i]);
			// Below this nothing is audible over the rest of the mix, and
			// starting a stream for it would only cost a channel that a
			// sound someone can hear could have had.
			if (gain < 0.01f) continue;
			SelectedSound sound;
			sound.file     = drained[i].file;
			sound.gain     = gain;
			sound.priority = drained[i].priority;
			selected.push_back(sound);
		}
		return selected;
	}
}
