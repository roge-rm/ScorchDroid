#include "SoundEventQueue.h"
#include <mutex>

namespace ScorchDroidAudio
{
	namespace
	{
		std::mutex queueMutex;
		std::vector<std::string> queue;
		const size_t kMaxQueueSize = 64;  // Drop oldest if the JNI side stops draining.
	}

	void pushSoundEvent(const std::string &soundFile)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (queue.size() >= kMaxQueueSize) {
			queue.erase(queue.begin());
		}
		queue.push_back(soundFile);
	}

	std::vector<std::string> drainSoundEvents()
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		std::vector<std::string> result;
		result.swap(queue);
		return result;
	}
}
