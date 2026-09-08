#include <GameSetup.h>

#include <common/OptionsGame.hpp>
#include <common/OptionsScorched.hpp>
#include <common/OptionEntry.hpp>

#include <mutex>
#include <algorithm>
#include <dirent.h>
#include <fstream>

namespace
{
	std::mutex g_mutex;
	OptionsGame *g_options = nullptr;
	std::string g_settingsFile;

	// The options the setup screen offers, in the order it shows them.
	//
	// Curated on purpose. OptionsGame carries around two hundred entries, and
	// the great majority are server-administration concerns - ban lists, log
	// destinations, master-server publishing, sync-check tuning - that mean
	// nothing when the "server" is the phone in your hand. These are the ones
	// that change how a game plays.
	const char *kExposed[] = {
		"NumberOfRounds",
		"MaxNumberOfRoundTurns",
		"TurnType",
		"WallType",
		"MoneyStarting",
		"ShotTime",
		"BuyingTime",
		"WindForce",
		"WindType",
		nullptr,
	};

	OptionEntry *findEntry(OptionsGame &game, const std::string &name)
	{
		std::list<OptionEntry *> &entries = game.getOptions();
		for (std::list<OptionEntry *>::iterator itor = entries.begin();
			itor != entries.end();
			++itor)
		{
			if (name == (*itor)->getName()) return *itor;
		}
		return nullptr;
	}

	bool describe(OptionEntry *entry, ScorchDroidSetup::Option &out)
	{
		out.name = entry->getName();
		out.description = entry->getDescription();
		out.value = entry->getValueAsString();

		switch (entry->getEntryType())
		{
		case OptionEntry::OptionEntryBoundedIntType:
		{
			OptionEntryBoundedInt *bounded = (OptionEntryBoundedInt *) entry;
			out.kind = ScorchDroidSetup::eBoundedInt;
			out.minValue = bounded->getMinValue();
			out.maxValue = bounded->getMaxValue();
			out.stepValue = bounded->getStepValue();
			return true;
		}
		case OptionEntry::OptionEntryEnumType:
		{
			OptionEntryEnum *enumEntry = (OptionEntryEnum *) entry;
			out.kind = ScorchDroidSetup::eEnum;
			// Upstream terminates these arrays with an empty description
			// rather than a null pointer - see OptionEntryEnum::setValue.
			for (OptionEntryEnum::EnumEntry *choice = enumEntry->getEnums();
				choice->description[0];
				choice++)
			{
				ScorchDroidSetup::Choice added;
				added.label = choice->description;
				added.value = choice->value;
				out.choices.push_back(added);
			}
			return true;
		}
		case OptionEntry::OptionEntryIntType:
			out.kind = ScorchDroidSetup::eInt;
			return true;
		case OptionEntry::OptionEntryBoolType:
			out.kind = ScorchDroidSetup::eBool;
			return true;
		default:
			// Strings, vectors and the rest have no control to draw yet, and
			// silently showing one as a number would be worse than omitting
			// it. Mod selection, the one string option worth having, gets its
			// own path rather than being forced through this one.
			return false;
		}
	}
}

namespace ScorchDroidSetup
{
	void ensureLoaded(const std::string &settingsFile)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_options) return;
		g_settingsFile = settingsFile;
		g_options = new OptionsGame();
		g_options->readOptionsFromFile(settingsFile);
	}

	std::vector<Option> options()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<Option> result;
		if (!g_options) return result;
		for (int i = 0; kExposed[i]; i++)
		{
			OptionEntry *entry = findEntry(*g_options, kExposed[i]);
			if (!entry) continue;
			Option option;
			if (describe(entry, option)) result.push_back(option);
		}
		return result;
	}

	bool set(const std::string &name, const std::string &value)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;

		bool exposed = false;
		for (int i = 0; kExposed[i]; i++)
		{
			if (name == kExposed[i]) { exposed = true; break; }
		}
		if (!exposed) return false;

		OptionEntry *entry = findEntry(*g_options, name);
		if (!entry) return false;
		// Upstream's own validation decides, not ours: a bounded int refuses
		// a value outside its range and an enum refuses one that isn't in its
		// list, which is exactly the behaviour a setup screen should inherit
		// rather than reimplement.
		return entry->setValueFromString(value);
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return;
		// Rebuilt, not just re-read. Most of these options are absent from
		// the config file - it only overrides a handful, and the rest hold
		// the defaults compiled into OptionsGame - so re-reading the file
		// leaves every one of those sitting at whatever the player last
		// chose. A fresh OptionsGame starts from the defaults, and reading
		// the file over it then reproduces exactly the state the server
		// itself starts from.
		delete g_options;
		g_options = new OptionsGame();
		g_options->readOptionsFromFile(g_settingsFile);
	}

	bool loadPreset(const std::string &path)
	{
		// Checked before anything else, because upstream's reader treats a
		// file it cannot open exactly like an empty one and returns success -
		// see the "return true for an empty file" in
		// OptionEntryHelper::readFromFile. Without this a mistyped preset path
		// would quietly produce a config of nothing but compiled defaults,
		// which is neither the preset nor what the player had, and would look
		// like the tutorial simply being wrong.
		{
			std::ifstream probe(path.c_str());
			if (!probe.is_open()) return false;
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		// Onto a fresh OptionsGame rather than over the current one: a preset
		// is "these settings", not "these settings plus whatever was chosen
		// before", and most options are absent from any given file, so
		// layering would leave the player's last game showing through.
		OptionsGame *loaded = new OptionsGame();
		if (!loaded->readOptionsFromFile(path))
		{
			delete loaded;
			return false;
		}
		delete g_options;
		g_options = loaded;
		return true;
	}

	std::vector<std::string> mods(const std::string &dataRoot)
	{
		// "none" is upstream's own name for the base game, not a placeholder
		// meaning "no mod", and it is always first so the list reads as a
		// choice rather than as an optional extra.
		std::vector<std::string> found;
		found.push_back("none");

		const std::string modsDir = dataRoot + "/data/globalmods";
		DIR *dir = opendir(modsDir.c_str());
		if (!dir) return found;
		while (struct dirent *entry = readdir(dir))
		{
			std::string name = entry->d_name;
			if (name == "." || name == ".." || name == "none") continue;
			// Directories only: anything else in here is not a mod.
			const std::string path = modsDir + "/" + name;
			DIR *sub = opendir(path.c_str());
			if (!sub) continue;
			closedir(sub);
			found.push_back(name);
		}
		closedir(dir);
		// Stable order, so the list does not reshuffle between visits just
		// because the filesystem returned entries differently.
		std::sort(found.begin() + 1, found.end());
		return found;
	}

	std::string mod()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return "none";
		return g_options->getMod();
	}

	bool setMod(const std::string &name)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;
		return g_options->getModEntry().setValueFromString(name);
	}

	std::vector<std::string> botNames(const std::string &dataRoot, const std::string &mod)
	{
		// A scan rather than an XML parse: the file's shape is upstream's own
		// and fixed - <ais> of <ai>, each opening with its <name> - and the
		// alternative is standing up the whole XMLFile machinery, with a
		// context, to read seven strings.
		//
		// Comments are tracked because they matter here: the Apocalypse mod
		// keeps five of its twelve AI definitions inside one, and a scanner
		// that read them would hand back bots the engine will refuse to
		// create.
		std::vector<std::string> names;
		const std::string path =
			dataRoot + "/data/globalmods/" + mod + "/data/tankais.xml";
		std::ifstream file(path.c_str());
		if (!file.is_open()) return names;

		std::string line;
		bool inComment = false;
		bool inAi = false;
		while (std::getline(file, line))
		{
			size_t pos = 0;
			while (pos < line.size())
			{
				if (inComment)
				{
					const size_t close = line.find("-->", pos);
					if (close == std::string::npos) { pos = line.size(); break; }
					inComment = false;
					pos = close + 3;
					continue;
				}
				const size_t open = line.find("<!--", pos);
				const std::string live = line.substr(pos,
					open == std::string::npos ? std::string::npos : open - pos);

				if (!inAi && live.find("<ai>") != std::string::npos) inAi = true;
				if (inAi)
				{
					const size_t nameStart = live.find("<name>");
					const size_t nameEnd = live.find("</name>");
					if (nameStart != std::string::npos && nameEnd != std::string::npos &&
						nameEnd > nameStart)
					{
						names.push_back(live.substr(nameStart + 6, nameEnd - nameStart - 6));
						// The first <name> in an <ai> is the AI's own; the
						// rest belong to its weapons.
						inAi = false;
					}
				}

				if (open == std::string::npos) { pos = line.size(); break; }
				inComment = true;
				pos = open + 4;
			}
		}
		return names;
	}

	int ensureBotsValidForMod(const std::string &dataRoot)
	{
		std::string chosenMod;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_options) return 0;
			chosenMod = g_options->getMod();
		}

		std::vector<std::string> valid = botNames(dataRoot, chosenMod);
		if (valid.empty()) return 0;   // unreadable: leave the config alone

		std::lock_guard<std::mutex> lock(g_mutex);
		int changed = 0;
		std::list<OptionEntry *> &players = g_options->getPlayerTypeOptions();
		for (std::list<OptionEntry *>::iterator itor = players.begin();
			itor != players.end();
			++itor)
		{
			const std::string current = (*itor)->getValueAsString();
			// "Human" is a player slot, not a bot, and an empty slot is empty.
			if (current == "Human" || current.empty()) continue;
			if (std::find(valid.begin(), valid.end(), current) != valid.end()) continue;
			(*itor)->setValueFromString(valid[0]);
			changed++;
		}
		return changed;
	}

	bool writeSessionFile(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;
		// Every option, not just the changed ones: this file replaces the
		// shipped config as what the server reads, so anything omitted would
		// silently fall back to a compiled default rather than to what the
		// shipped config says.
		return g_options->writeOptionsToFile(path, true);
	}

	void applyTo(OptionsScorched &options)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return;
		for (int i = 0; kExposed[i]; i++)
		{
			OptionEntry *chosen = findEntry(*g_options, kExposed[i]);
			OptionEntry *target = findEntry(options.getMainOptions(), kExposed[i]);
			if (!chosen || !target) continue;
			target->setValueFromString(chosen->getValueAsString());
		}
	}
}
