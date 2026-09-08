#include <GameSetup.h>

#include <common/OptionsGame.hpp>
#include <common/OptionsScorched.hpp>
#include <common/OptionEntry.hpp>
#include <XML/XMLFile.hpp>

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
	// M17 fills this out: the first nine were the ones a first game needs,
	// and everything else upstream lets a host decide was simply
	// unreachable. The order is the order the screen shows them - the shape
	// of the game first, then the clocks, then the arsenal, then the
	// physics - rather than OptionsGame's own, which is grouped by the
	// section of the config file it lives in.
	// M19: each entry also says whether it is an everyday option or an
	// advanced one. Upstream keeps around forty-five of these behind an
	// "Advanced Options" button on its own setup dialog, and the split here
	// is the same judgement: the ones that change what a game *is* stay in
	// front, and the ones that tune how it is scored, paid for and timed sit
	// under a heading you have to open.
	struct Exposed
	{
		const char *name;
		const char *group;
		bool advanced;
	};
	const Exposed kExposed[] = {
		// --- Game -------------------------------------------------------
		{ "NumberOfRounds",           "Game",    false },
		{ "MaxNumberOfRoundTurns",    "Game",    false },
		{ "TurnType",                 "Game",    false },
		{ "ShotTime",                 "Game",    false },
		{ "BuyingTime",               "Game",    false },
		{ "ResignMode",               "Game",    false },
		{ "RoundTime",                "Game",    true  },
		{ "StartTime",                "Game",    true  },
		{ "AIShotTime",               "Game",    true  },
		{ "RemoveTime",               "Game",    true  },
		{ "ScoreWonForRound",         "Game",    true  },
		{ "ScoreWonForLives",         "Game",    true  },
		{ "ScorePerKill",             "Game",    true  },
		{ "ScorePerAssist",           "Game",    true  },
		{ "ScorePerMoney",            "Game",    true  },
		{ "ScorePerResign",           "Game",    true  },
		{ "ScorePerSpectate",         "Game",    true  },
		// --- Players ----------------------------------------------------
		{ "NumberOfPlayers",          "Players", false },
		{ "PlayerLives",              "Players", false },
		{ "Teams",                    "Players", false },
		{ "TeamBallance",             "Players", false },
		{ "RemoveBotsAtPlayers",      "Players", true  },
		{ "ResidualPlayers",          "Players", true  },
		// --- Arms -------------------------------------------------------
		{ "MoneyStarting",            "Arms",    false },
		{ "StartArmsLevel",           "Arms",    false },
		{ "EndArmsLevel",             "Arms",    false },
		{ "WeaponSpeed",              "Arms",    false },
		{ "WeaponScale",              "Arms",    true  },
		{ "GiveAllWeapons",           "Arms",    true  },
		{ "MaxNumberWeapons",         "Arms",    true  },
		{ "DelayedDefenseActivation", "Arms",    true  },
		{ "MoneyBuyOnRound",          "Arms",    true  },
		{ "MoneyInterest",            "Arms",    true  },
		{ "MoneyPerRound",            "Arms",    true  },
		{ "MoneyWonForRound",         "Arms",    true  },
		{ "MoneyWonForLives",         "Arms",    true  },
		{ "MoneyWonPerKillPoint",     "Arms",    true  },
		{ "MoneyWonPerAssistPoint",   "Arms",    true  },
		{ "MoneyWonPerMultiKillPoint","Arms",    true  },
		{ "MoneyWonPerHitPoint",      "Arms",    true  },
		{ "MoneyPerHealthPoint",      "Arms",    true  },
		// --- World ------------------------------------------------------
		{ "WallType",                 "World",   false },
		{ "Gravity",                  "World",   false },
		{ "WindForce",                "World",   false },
		{ "WindType",                 "World",   false },
		{ "MovementRestriction",      "World",   true  },
		{ "MaxClimbingDistance",      "World",   true  },
		{ "MinFallingDistance",       "World",   true  },
		{ "TankFallingDamage",        "World",   true  },
		{ "CycleMaps",                "World",   true  },
		{ nullptr, nullptr, false },
	};

	std::string trimmed(const std::string &value)
	{
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return "";
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

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
		// Upstream keeps its retired options in the list, flagged rather than
		// deleted, so a config file written years ago still parses. They read
		// as ordinary bounded ints and enums, which is exactly how one would
		// end up on this screen by mistake - MaxArmsLevel, ScoreType and
		// AutoBallanceTeams all look like live options and none of them does
		// anything any more.
		if (entry->getData() & OptionEntry::DataDepricated) return false;

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
		for (int i = 0; kExposed[i].name; i++)
		{
			OptionEntry *entry = findEntry(*g_options, kExposed[i].name);
			if (!entry) continue;
			Option option;
			option.group = kExposed[i].group;
			option.advanced = kExposed[i].advanced;
			if (describe(entry, option)) result.push_back(option);
		}
		return result;
	}

	bool set(const std::string &name, const std::string &value)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;

		bool exposed = false;
		for (int i = 0; kExposed[i].name; i++)
		{
			if (name == kExposed[i].name) { exposed = true; break; }
		}
		if (!exposed) return false;

		OptionEntry *entry = findEntry(*g_options, name);
		if (!entry) return false;
		// Upstream's own validation decides, not ours: a bounded int refuses
		// a value outside its range and an enum refuses one that isn't in its
		// list, which is exactly the behaviour a setup screen should inherit
		// rather than reimplement.
		if (!entry->setValueFromString(value)) return false;

		// M18: how many players there are is really two options - the
		// maximum, and the minimum the round waits for before it starts
		// (ServerStateEnoughPlayers fills up to the minimum with bots). A
		// screen that offered both would be asking a question nobody playing
		// alone wants to answer, and setting only the maximum does nothing at
		// all: the shipped config's minimum is 2, so a game "of eight" still
		// started with one bot. Upstream's own single-player files set the
		// pair together for the same reason - singleeasy is 3 and 3.
		if (name == "NumberOfPlayers")
		{
			OptionEntry *minimum = findEntry(*g_options, "NumberOfMinPlayers");
			if (minimum) minimum->setValueFromString(value);
		}
		return true;
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

	std::vector<Preset> presets(const std::string &dataRoot, const std::string &mod)
	{
		std::vector<Preset> found;
		const std::string modDir = dataRoot + "/data/globalmods/" + mod;

		// A real parse this time, not the scan botNames() uses. There the
		// point was to see the file exactly as upstream's parser does,
		// commented-out entries included - here the same is true for the
		// opposite reason: a <game> inside a comment is one upstream does not
		// offer, and XMLFile is what decides that.
		XMLFile file;
		if (!file.readFile(modDir + "/data/modinfo.xml")) return found;
		if (!file.getRootNode()) return found;

		XMLNode *gameNode = nullptr;
		while (file.getRootNode()->getNamedChild("game", gameNode, false))
		{
			Preset preset;
			preset.mod = mod;
			std::string gamefile;
			if (!gameNode->getNamedChild("description", preset.description, false)) continue;
			if (!gameNode->getNamedChild("gamefile", gamefile, false)) continue;
			// Upstream's own fallback: an entry with no short description is
			// listed under its long one.
			if (!gameNode->getNamedChild("shortdescription", preset.name, false))
			{
				preset.name = preset.description;
			}

			// The path is mod-relative, as every path in a mod's files is -
			// upstream resolves it with getModFile(), which this port cannot
			// use here because that reads the *currently loaded* mod and the
			// point is to list a mod that has not been loaded.
			preset.gamefile = modDir + "/" + gamefile;
			std::ifstream probe(preset.gamefile.c_str());
			if (!probe.is_open()) continue;

			found.push_back(preset);
		}
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

	std::vector<Bot> bots(const std::string &dataRoot)
	{
		std::string chosenMod;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			chosenMod = g_options ? g_options->getMod() : "none";
		}

		std::vector<Bot> found;
		// Upstream's own first entry, and not an AI: ServerStateEnoughPlayers
		// treats "Random" as "any of them", filling the slot with whichever
		// bot it likes. Its single-player files use it to mix a game up.
		Bot random;
		random.name = "Random";
		random.description = "A different computer player each time";
		found.push_back(random);

		std::vector<Bot> scanned = botsFor(dataRoot, chosenMod);
		for (size_t i = 0; i < scanned.size(); i++)
		{
			// Target is upstream's inert practice dummy - it never fires
			// back. It belongs in the target-practice preset, not in a list
			// of opponents, where picking it would look like a game that
			// never fights back for no stated reason.
			if (scanned[i].name == "Target" || scanned[i].name == "Hard Target") continue;
			found.push_back(scanned[i]);
		}
		return found;
	}

	std::vector<std::string> landscapes(const std::string &dataRoot)
	{
		std::string chosenMod;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			chosenMod = g_options ? g_options->getMod() : "none";
		}

		std::vector<std::string> found;
		// Same comment-aware scan as the bots, and for the same reason: a
		// landscape inside a comment is one the engine will not offer, and a
		// picker that listed it would produce a game that cannot start.
		std::vector<Bot> scanned = scanNamed(
			dataRoot + "/data/globalmods/" + chosenMod + "/data/landscapes.xml", "landscape");
		if (scanned.empty() && chosenMod != "none")
		{
			// Upstream's own fallback (S3D::getModFile): a mod with no
			// landscapes.xml of its own plays the base game's.
			scanned = scanNamed(
				dataRoot + "/data/globalmods/none/data/landscapes.xml", "landscape");
		}
		for (size_t i = 0; i < scanned.size(); i++) found.push_back(scanned[i].name);
		return found;
	}

	std::vector<std::string> selectedLandscapes()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<std::string> found;
		if (!g_options) return found;
		const std::string value = g_options->getLandscapes();
		size_t start = 0;
		while (start < value.size())
		{
			const size_t colon = value.find(':', start);
			const std::string name = trimmed(value.substr(
				start, colon == std::string::npos ? std::string::npos : colon - start));
			if (!name.empty()) found.push_back(name);
			if (colon == std::string::npos) break;
			start = colon + 1;
		}
		return found;
	}

	bool setLandscapes(const std::vector<std::string> &names)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;
		std::string value;
		for (size_t i = 0; i < names.size(); i++)
		{
			if (!value.empty()) value += ":";
			value += names[i];
		}
		// An empty list is upstream's own "all of them", not "none of them" -
		// there is no way to say "no landscapes" and a game with nothing to
		// play on could not start.
		return g_options->getLandscapesEntry().setValueFromString(value);
	}

	std::vector<std::string> botTypes()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<std::string> found;
		if (!g_options) return found;
		std::list<OptionEntry *> &players = g_options->getPlayerTypeOptions();
		int index = 0;
		for (std::list<OptionEntry *>::iterator itor = players.begin();
			itor != players.end();
			++itor, index++)
		{
			// Slot one is the player; the rest are the mix, and each distinct
			// name in slot order is one chip in the UI.
			if (index == 0) continue;
			const std::string name = (*itor)->getValueAsString();
			if (name.empty() || name == "Human") continue;
			if (std::find(found.begin(), found.end(), name) == found.end())
			{
				found.push_back(name);
			}
		}
		return found;
	}

	bool setBotTypes(const std::vector<std::string> &names)
	{
		if (names.empty()) return false;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;

		std::list<OptionEntry *> &players = g_options->getPlayerTypeOptions();
		int index = 0;
		for (std::list<OptionEntry *>::iterator itor = players.begin();
			itor != players.end();
			++itor, index++)
		{
			if (index == 0) continue;
			// Round-robin rather than random, so the mix is the one asked
			// for: two of three slots are Sharks if two of the three chips
			// are, however many players the game ends up with.
			(*itor)->setValueFromString(names[(index - 1) % names.size()]);
		}
		return true;
	}

	std::string botType()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return "";
		// Slot two: the first that is not the player. Slot one is the human
		// and says nothing about the bots.
		std::list<OptionEntry *> &players = g_options->getPlayerTypeOptions();
		int index = 0;
		for (std::list<OptionEntry *>::iterator itor = players.begin();
			itor != players.end();
			++itor, index++)
		{
			if (index == 1) return (*itor)->getValueAsString();
		}
		return "";
	}

	bool setBotType(const std::string &name)
	{
		if (name.empty()) return false;
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_options) return false;

		std::list<OptionEntry *> &players = g_options->getPlayerTypeOptions();
		int index = 0;
		for (std::list<OptionEntry *>::iterator itor = players.begin();
			itor != players.end();
			++itor, index++)
		{
			// Every slot but the first, whatever the player count happens to
			// be: the ones past it are simply never reached
			// (ServerStateEnoughPlayers stops at NumberOfPlayers), and
			// leaving them behind would mean raising the count later quietly
			// brought back the previous choice.
			if (index == 0) continue;
			(*itor)->setValueFromString(name);
		}
		return true;
	}

	std::vector<std::string> botNames(const std::string &dataRoot, const std::string &mod)
	{
		std::vector<std::string> names;
		std::vector<Bot> scanned = botsFor(dataRoot, mod);
		for (size_t i = 0; i < scanned.size(); i++) names.push_back(scanned[i].name);
		return names;
	}

	std::vector<Bot> botsFor(const std::string &dataRoot, const std::string &mod)
	{
		return scanNamed(dataRoot + "/data/globalmods/" + mod + "/data/tankais.xml", "ai");
	}

	std::vector<Bot> scanNamed(const std::string &path, const std::string &tag)
	{
		// A scan rather than an XML parse: the file's shape is upstream's own
		// and fixed - <ais> of <ai>, each opening with its <name> and then its
		// <description> - and the alternative is standing up the whole XMLFile
		// machinery, with a context, to read seven strings.
		//
		// Comments are tracked because they matter here: the Apocalypse mod
		// keeps five of its twelve AI definitions inside one, and a scanner
		// that read them would hand back bots the engine will refuse to
		// create.
		std::vector<Bot> found;
		const std::string openTag = "<" + tag + ">";
		std::ifstream file(path.c_str());
		if (!file.is_open()) return found;

		std::string line;
		bool inComment = false;
		bool inAi = false;
		bool wantDescription = false;
		bool inDescription = false;
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

				if (live.find(openTag) != std::string::npos)
				{
					inAi = true;
					wantDescription = false;
					inDescription = false;
				}
				if (inAi)
				{
					const size_t nameStart = live.find("<name>");
					const size_t nameEnd = live.find("</name>");
					if (nameStart != std::string::npos && nameEnd != std::string::npos &&
						nameEnd > nameStart)
					{
						Bot bot;
						bot.name = live.substr(nameStart + 6, nameEnd - nameStart - 6);
						found.push_back(bot);
						// The first <name> in an <ai> is the AI's own; the
						// rest belong to its weapons. The description that
						// follows is its own too, and only the first one.
						inAi = false;
						wantDescription = true;
					}
				}
				else if (wantDescription && !found.empty())
				{
					// Upstream writes these over one line or several, so the
					// text is collected until the closing tag rather than
					// read off a single line.
					std::string text = live;
					const size_t descStart = live.find("<description>");
					if (descStart != std::string::npos)
					{
						inDescription = true;
						text = live.substr(descStart + 13);
					}
					if (inDescription)
					{
						const size_t descEnd = text.find("</description>");
						if (descEnd != std::string::npos)
						{
							text = text.substr(0, descEnd);
							inDescription = false;
							wantDescription = false;
						}
						std::string &into = found.back().description;
						if (!into.empty() && !text.empty()) into += " ";
						into += trimmed(text);
					}
				}

				if (open == std::string::npos) { pos = line.size(); break; }
				inComment = true;
				pos = open + 4;
			}
		}

		// Trailing whitespace from a description broken over several lines.
		for (size_t i = 0; i < found.size(); i++)
		{
			found[i].description = trimmed(found[i].description);
		}
		return found;
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
		for (int i = 0; kExposed[i].name; i++)
		{
			OptionEntry *chosen = findEntry(*g_options, kExposed[i].name);
			OptionEntry *target = findEntry(options.getMainOptions(), kExposed[i].name);
			if (!chosen || !target) continue;
			target->setValueFromString(chosen->getValueAsString());
		}
	}
}
