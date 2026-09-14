#include "PlanLineStore.h"

#include <deque>
#include <mutex>

namespace ScorchDroidPlanLines
{
	namespace
	{
		std::mutex storeMutex;
		std::deque<Line> lines;
		unsigned int nextId = 1;
		unsigned int storeVersion = 0;

		// Every line here is gone from the HUD three seconds after it
		// arrives, so this only has to cover the gap between arriving and
		// being polled. Upstream's own cap on one message is 150 points;
		// this is comfortably more than the busiest moment can leave
		// waiting, and each entry is six floats.
		const size_t kMaxLines = 256;

		// Upstream's pen-up marker: a null vector between two strokes.
		bool isPenUp(float x, float y)
		{
			return x == 0.0f && y == 0.0f;
		}
	}

	void pushStroke(unsigned int playerId, const std::vector<float> &interleavedXy)
	{
		if (playerId == 0) return;

		std::lock_guard<std::mutex> lock(storeMutex);
		bool pushedAny = false;

		// Walk the point list turning consecutive pairs into lines, exactly
		// as upstream's GL_LINE_STRIP does: a null vector ends the strip and
		// starts a new one, so it joins nothing to what follows.
		bool havePrevious = false;
		float previousX = 0.0f, previousY = 0.0f;
		for (size_t i = 0; i + 1 < interleavedXy.size(); i += 2)
		{
			const float x = interleavedXy[i];
			const float y = interleavedXy[i + 1];
			if (isPenUp(x, y))
			{
				havePrevious = false;
				continue;
			}
			if (havePrevious)
			{
				Line line;
				line.id = nextId++;
				line.playerId = playerId;
				line.ax = previousX;
				line.ay = previousY;
				line.bx = x;
				line.by = y;
				lines.push_back(line);
				pushedAny = true;
			}
			previousX = x;
			previousY = y;
			havePrevious = true;
		}

		while (lines.size() > kMaxLines) lines.pop_front();
		if (pushedAny) storeVersion++;
	}

	std::vector<Line> since(unsigned int afterId)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::vector<Line> result;
		for (const Line &line : lines)
		{
			if (line.id > afterId) result.push_back(line);
		}
		return result;
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
		storeVersion++;
	}
}
