#include "ChatStore.h"

#include <deque>
#include <mutex>

namespace ScorchDroidChat
{
	namespace
	{
		std::mutex storeMutex;
		std::deque<Line> lines;
		unsigned int nextId = 1;
		unsigned int storeVersion = 0;

		// Deliberately more than the transient HUD stack ever shows at
		// once: the stack is a few seconds of the most recent traffic, but
		// the log dialog scrolls back, and upstream's own server-side
		// rolling buffer is 25. A hundred short strings is nothing.
		const size_t kMaxLines = 100;
	}

	void push(const Line &line)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		Line stored = line;
		stored.id = nextId++;
		lines.push_back(stored);
		while (lines.size() > kMaxLines) lines.pop_front();
		storeVersion++;
	}

	std::vector<Line> snapshot()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return std::vector<Line>(lines.begin(), lines.end());
	}

	std::vector<Line> since(unsigned int afterId)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::vector<Line> out;
		for (std::deque<Line>::iterator itor = lines.begin(); itor != lines.end(); ++itor) {
			if (itor->id > afterId) out.push_back(*itor);
		}
		return out;
	}

	unsigned int version()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return storeVersion;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		lines.clear();
		// Ids keep counting rather than restarting: a HUD that has just
		// shown line 7 must not be handed a different line 7 next round.
		storeVersion++;
	}
}
