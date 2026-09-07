#include "DeformEventQueue.h"
#include <mutex>

namespace ScorchDroidLandscape
{
	namespace
	{
		std::mutex regionMutex;
		bool dirty = false;
		int dirtyMinX = 0, dirtyMinY = 0, dirtyMaxX = 0, dirtyMaxY = 0;

		// Shares regionMutex with the region above - both are pushed from
		// the same place (one deform reports both) and drained from the
		// same place, so a second lock would buy nothing.
		std::vector<ScorchEvent> scorchQueue;
		const size_t kMaxScorchEvents = 64;
	}

	void pushDeform(int centreX, int centreY, int radius)
	{
		if (radius < 1) radius = 1;
		const int minX = centreX - radius;
		const int minY = centreY - radius;
		const int maxX = centreX + radius;
		const int maxY = centreY + radius;

		std::lock_guard<std::mutex> lock(regionMutex);
		if (!dirty) {
			dirty = true;
			dirtyMinX = minX; dirtyMinY = minY;
			dirtyMaxX = maxX; dirtyMaxY = maxY;
			return;
		}
		if (minX < dirtyMinX) dirtyMinX = minX;
		if (minY < dirtyMinY) dirtyMinY = minY;
		if (maxX > dirtyMaxX) dirtyMaxX = maxX;
		if (maxY > dirtyMaxY) dirtyMaxY = maxY;
	}

	bool takeDirtyRegion(int &minX, int &minY, int &maxX, int &maxY)
	{
		std::lock_guard<std::mutex> lock(regionMutex);
		if (!dirty) return false;
		minX = dirtyMinX; minY = dirtyMinY;
		maxX = dirtyMaxX; maxY = dirtyMaxY;
		dirty = false;
		return true;
	}

	void clearDirtyRegion()
	{
		std::lock_guard<std::mutex> lock(regionMutex);
		dirty = false;
		scorchQueue.clear();  // same reasoning: stale coordinates, new landscape
	}

	void pushScorch(int centreX, int centreY, float radius, const char *texture)
	{
		if (radius <= 0.0f) return;

		ScorchEvent event;
		event.centreX = centreX;
		event.centreY = centreY;
		event.radius = radius;
		if (texture) event.texture = texture;

		std::lock_guard<std::mutex> lock(regionMutex);
		if (scorchQueue.size() >= kMaxScorchEvents) return;  // drop newest, keep the earlier marks
		scorchQueue.push_back(event);
	}

	std::vector<ScorchEvent> drainScorchEvents()
	{
		std::lock_guard<std::mutex> lock(regionMutex);
		std::vector<ScorchEvent> result;
		result.swap(scorchQueue);
		return result;
	}
}
