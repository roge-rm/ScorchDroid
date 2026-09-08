#include <PlayerProfile.h>

#include <XML/XMLFile.hpp>
#include <common/Vector.hpp>
#include <coms/ComsTankChangeMessage.hpp>
#include <tank/TankAvatar.hpp>
#include <tank/TankColorGenerator.hpp>
#include <tank/TankModel.hpp>
#include <tank/TankModelStore.hpp>

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <mutex>

namespace
{
	std::mutex g_mutex;
	std::string g_name = "Player";
	// The unchosen values, which are what this port did before there was
	// anything to choose: a random model, the colour the engine allocates,
	// and no avatar at all.
	std::string g_model;
	int g_colorIndex = -1;
	std::string g_avatar;

	std::string trimmed(const std::string &value)
	{
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return "";
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	bool readable(const std::string &path)
	{
		std::ifstream probe(path.c_str());
		return probe.is_open();
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

	std::vector<std::string> models(const std::string &dataRoot, const std::string &mod)
	{
		std::vector<std::string> found;

		// Upstream's own fallback order, from S3D::getModFile: the mod's own
		// file if it has one, else the base game's. The Apocalypse mod ships
		// no tanks.xml at all, so without this it would offer no models
		// rather than the hundred and five it actually plays with.
		std::string path = dataRoot + "/data/globalmods/" + mod + "/data/tanks.xml";
		if (!readable(path)) path = dataRoot + "/data/globalmods/none/data/tanks.xml";

		XMLFile file;
		if (!file.readFile(path)) return found;
		if (!file.getRootNode()) return found;

		XMLNode *tankNode = nullptr;
		while (file.getRootNode()->getNamedChild("tank", tankNode, false))
		{
			std::string name;
			if (!tankNode->getNamedChild("name", name, false)) continue;
			// "Random" is upstream's own name for "surprise me", and its
			// model is a question mark. It is not a tank anyone chooses -
			// getRandomModel skips it and getModelByName refuses it - so the
			// UI's own "Random" entry is an empty choice, not this.
			if (name == "Random") continue;
			found.push_back(name);
		}
		return found;
	}

	std::vector<unsigned int> colors()
	{
		std::vector<unsigned int> found;
		std::vector<Vector *> &all = TankColorGenerator::instance()->getAllColors();
		for (size_t i = 0; i < all.size(); i++)
		{
			Vector &colour = *all[i];
			// Upstream stores these as 0..1 floats rounded to two decimal
			// places (see TankColorGenerator::addColor), so this comes back
			// one or two off the literals it was built from - 255 becomes
			// 255, but 140 becomes 140 only because the rounding is done in
			// the same direction. Close enough to show, and it is the colour
			// the game will actually draw.
			found.push_back(
				((unsigned int) (colour[0] * 255.0f + 0.5f) << 16) |
				((unsigned int) (colour[1] * 255.0f + 0.5f) << 8) |
				((unsigned int) (colour[2] * 255.0f + 0.5f)));
		}
		return found;
	}

	std::vector<std::string> avatars(const std::string &dataRoot)
	{
		std::vector<std::string> found;
		const std::string dir = dataRoot + "/data/avatars";
		DIR *handle = opendir(dir.c_str());
		if (!handle) return found;
		while (struct dirent *entry = readdir(handle))
		{
			const std::string file = entry->d_name;
			// PNGs only: the directory also holds upstream's agreement.txt,
			// which says where the images came from.
			if (file.size() < 5) continue;
			if (file.compare(file.size() - 4, 4, ".png") != 0) continue;
			found.push_back("data/avatars/" + file);
		}
		closedir(handle);
		// Stable order, so a picker does not reshuffle between visits.
		std::sort(found.begin(), found.end());
		return found;
	}

	std::string model()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_model;
	}

	void setModel(const std::string &value)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_model = trimmed(value);
	}

	int colorIndex()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_colorIndex;
	}

	void setColorIndex(int index)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_colorIndex = index;
	}

	std::string avatar()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_avatar;
	}

	void setAvatar(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_avatar = trimmed(path);
	}

	std::string modelFor(TankModelStore &store, int team, const char *tankType)
	{
		const std::string chosen = model();
		if (!chosen.empty())
		{
			TankModel *found = store.getModelByName(chosen.c_str());
			// isOfTeam and isOfTankType are upstream's own filters, the same
			// ones getRandomModel applies: a model declared for team 2 in a
			// team game is not one this tank may wear, and the engine would
			// quietly substitute one anyway.
			if (found && found->isOfTeam(team) && found->isOfTankType(tankType))
			{
				return found->getName();
			}
		}
		return store.getRandomModel(team, false, tankType)->getName();
	}

	Vector colorFor(const Vector &current)
	{
		const int index = colorIndex();
		if (index < 0) return current;
		std::vector<Vector *> &all = TankColorGenerator::instance()->getAllColors();
		if (index >= (int) all.size()) return current;
		return *all[index];
	}

	void applyAvatar(ComsTankChangeMessage &message)
	{
		const std::string chosen = avatar();
		if (chosen.empty()) return;

		// Loaded through upstream's own TankAvatar rather than by reading the
		// file here, because the receiving end is TankAvatar::setFromBuffer -
		// the same pairing TankAddSimAction uses to give every bot the
		// computer avatar.
		TankAvatar loaded;
		if (!loaded.loadFromFile(chosen)) return;

		message.setPlayerIconName(chosen.c_str());
		message.getPlayerIcon().addDataToBuffer(
			loaded.getFile().getBuffer(), loaded.getFile().getBufferUsed());
	}
}
