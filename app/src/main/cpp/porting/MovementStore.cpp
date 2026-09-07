#include "MovementStore.h"

#include <mutex>

namespace ScorchDroidMovement
{
	namespace
	{
		std::mutex storeMutex;
		int maskWidth = 0, maskHeight = 0;
		std::vector<unsigned char> maskReachable;
		unsigned int maskVersion = 0;
	}

	void publish(int width, int height, std::vector<unsigned char> reachable)
	{
		if (width <= 0 || height <= 0) return;
		if (reachable.size() != (size_t) width * (size_t) height) return;

		std::lock_guard<std::mutex> lock(storeMutex);
		maskWidth = width;
		maskHeight = height;
		maskReachable = std::move(reachable);
		maskVersion++;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		// Only a real change bumps the version - engine_jni calls this on
		// every tick that has no position-selecting weapon current, which is
		// almost every tick of a normal game, and each bump would cost the
		// renderer a full texture re-upload.
		if (maskReachable.empty()) return;
		maskWidth = maskHeight = 0;
		maskReachable.clear();
		maskVersion++;
	}

	unsigned int version()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return maskVersion;
	}

	bool get(int &width, int &height, std::vector<unsigned char> &reachable)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		if (maskReachable.empty()) return false;
		width = maskWidth;
		height = maskHeight;
		reachable = maskReachable;
		return true;
	}
}
