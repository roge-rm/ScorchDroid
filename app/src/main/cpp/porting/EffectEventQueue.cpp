#include "EffectEventQueue.h"
#include <mutex>

namespace ScorchDroidEffects
{
	namespace
	{
		std::mutex queueMutex;
		std::vector<EffectEvent> queue;
		const size_t kMaxQueued = 512;
	}

	size_t maxQueued() { return kMaxQueued; }

	void push(const EffectEvent &effect)
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (queue.size() >= kMaxQueued) return;
		queue.push_back(effect);
	}

	std::vector<EffectEvent> drain()
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		std::vector<EffectEvent> result;
		result.swap(queue);
		return result;
	}
}
