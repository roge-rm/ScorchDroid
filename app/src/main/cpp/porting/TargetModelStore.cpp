#include "TargetModelStore.h"

#include <map>
#include <mutex>

namespace ScorchDroidTargets
{
	namespace
	{
		std::mutex storeMutex;
		std::map<unsigned int, Info> targets;
	}

	void push(unsigned int playerId, const ModelID &model,
			  float scale, float brightness, float rotationDegrees, bool billboard)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		// Deliberately never cleared. Every round rebuilds the landscape and
		// creates fresh targets, but ids come from the same allocator, so a
		// new target simply overwrites the entry for the id it inherits.
		// Only the renderer reads this, and only for ids that are currently
		// in the target container - an entry for a target that no longer
		// exists is never looked up, and the map is bounded by the largest
		// number of targets a landscape has ever held.
		Info &info = targets[playerId];
		info.model = model;
		info.scale = scale;
		info.brightness = brightness;
		info.rotationDegrees = rotationDegrees;
		info.billboard = billboard;
	}

	bool get(unsigned int playerId, Info &out)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::map<unsigned int, Info>::iterator itor = targets.find(playerId);
		if (itor == targets.end()) return false;
		out = itor->second;
		return true;
	}

	size_t size()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return targets.size();
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		targets.clear();
	}
}
