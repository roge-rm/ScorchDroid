#include "TracerStore.h"

#include <map>
#include <mutex>

namespace ScorchDroidTracer
{
	namespace
	{
		struct PendingPath
		{
			unsigned int playerId = 0;
			std::vector<Point> points;
		};

		struct PlayerTracers
		{
			std::vector<Point> endPoints;
			std::vector<std::vector<Point> > paths;
		};

		std::mutex storeMutex;
		std::map<uintptr_t, PendingPath> pending;
		std::map<unsigned int, PlayerTracers> finished;

		// A round's worth of ranging shots is a handful; these caps only
		// exist so a pathological weapon (or a shot that never collides)
		// can't grow the store without limit.
		const size_t kMaxPointsPerPath = 512;
		const size_t kMaxPathsPerPlayer = 32;
		const size_t kMaxEndPointsPerPlayer = 64;
	}

	void addPathPoint(uintptr_t shotId, unsigned int playerId, float x, float y, float z)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		PendingPath &path = pending[shotId];
		path.playerId = playerId;
		if (path.points.size() >= kMaxPointsPerPath) return;
		Point point;
		point.x = x; point.y = y; point.z = z;
		path.points.push_back(point);
	}

	void finishPath(uintptr_t shotId)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::map<uintptr_t, PendingPath>::iterator itor = pending.find(shotId);
		if (itor == pending.end()) return;

		// A single point is not a path - nothing to draw between.
		if (itor->second.points.size() > 1) {
			PlayerTracers &player = finished[itor->second.playerId];
			if (player.paths.size() < kMaxPathsPerPlayer) {
				player.paths.push_back(itor->second.points);
			}
		}
		pending.erase(itor);
	}

	void addEndPoint(unsigned int playerId, float x, float y, float z)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		PlayerTracers &player = finished[playerId];
		if (player.endPoints.size() >= kMaxEndPointsPerPlayer) return;
		Point point;
		point.x = x; point.y = y; point.z = z;
		player.endPoints.push_back(point);
	}

	void getFor(unsigned int playerId,
				std::vector<Point> &endPoints,
				std::vector<std::vector<Point> > &paths)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::map<unsigned int, PlayerTracers>::iterator itor = finished.find(playerId);
		if (itor == finished.end()) return;
		endPoints = itor->second.endPoints;
		paths = itor->second.paths;
	}

	void clearAll()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		pending.clear();
		finished.clear();
	}
}
