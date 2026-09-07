#include "DeformEventQueue.h"
#include <mutex>

namespace ScorchDroidLandscape
{
	namespace
	{
		std::mutex regionMutex;
		bool dirty = false;
		int dirtyMinX = 0, dirtyMinY = 0, dirtyMaxX = 0, dirtyMaxY = 0;
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
	}
}
