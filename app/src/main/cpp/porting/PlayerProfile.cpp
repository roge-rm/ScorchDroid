#include <PlayerProfile.h>

#include <mutex>

namespace
{
	std::mutex g_mutex;
	std::string g_name = "Player";

	std::string trimmed(const std::string &value)
	{
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return "";
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}
}

namespace ScorchDroidProfile
{
	std::string name()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_name;
	}

	std::string setName(const std::string &value)
	{
		const std::string clean = trimmed(value);
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!clean.empty()) g_name = clean;
		return g_name;
	}
}
