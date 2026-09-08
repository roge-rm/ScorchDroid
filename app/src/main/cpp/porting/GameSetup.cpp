#include <GameSetup.h>

#include <common/OptionsGame.hpp>
#include <common/OptionsScorched.hpp>
#include <common/OptionEntry.hpp>

#include <mutex>
#include <algorithm>
#include <dirent.h>

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
