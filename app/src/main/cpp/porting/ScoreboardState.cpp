#include <ScoreboardState.h>

#include <mutex>

namespace
{
	std::mutex g_mutex;
	ScorchDroidScoreboard::State g_state;
}

namespace ScorchDroidScoreboard
{
	void show(bool finalScore)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_state.showing = true;
		g_state.finalScore = finalScore;
	}

	void hide()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_state.showing = false;
		// finalScore is deliberately left alone: the flag only means
		// anything while the board is up, and clearing it here would race
		// the UI's poll for no gain.
	}

	State get()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_state;
	}
}
