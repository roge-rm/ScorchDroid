#ifndef SCORCHDROID_GAME_SETUP_H
#define SCORCHDROID_GAME_SETUP_H

#include <string>
#include <vector>

class OptionsScorched;

// M10: the settings a player chooses before starting a game - rounds, turns,
// wall type, starting money and the rest.
//
// These are ordinary OptionsGame entries that the engine already parses out of
// scorchdroid_server.xml, so nothing here invents a game rule; it only lets the
// player pick among values upstream already defines, and carries the choice
// into the server when the game starts.
//
// It has to be a *standalone* OptionsGame rather than a view onto the running
// server's, because the setup screen exists before there is a server at all -
// ScorchedServer::instance() is null until startServer(), and after
// stopServer() it is null again (see host-tests' testServerRestart). So this
// loads the same config file the server will load, the screen edits that, and
// applyTo() pushes the result into the server once it exists.
//
// Deliberately GL-free and engine-only, like LandscapeTextureBuilder and
// InstanceBuffer, so host-tests can cover the part that actually breaks: not
// the reading, but whether a value survives the first round (see applyTo).
namespace ScorchDroidSetup
{
	// What kind of control the UI should draw for an option, mapped from
	// upstream's OptionEntry::EntryType. Anything upstream offers that isn't
	// one of these is simply not exposed.
	enum Kind
	{
		eBoundedInt = 0,  // a slider: min, max and step are meaningful
		eInt,             // a plain number
		eBool,            // a switch
		eEnum,            // a choice among `choices`
	};

	struct Choice
	{
		// Upstream's own identifier, e.g. "WallConcrete". Left as-is here;
		// making it presentable is the UI's job, not the engine layer's.
		std::string label;
		int value = 0;
	};

	struct Option
	{
		std::string name;         // upstream's option name, the key for set()
		std::string description;  // upstream's own one-line description
		Kind kind = eInt;
		std::string value;        // current value, as a string
		int minValue = 0, maxValue = 0, stepValue = 1;
		std::vector<Choice> choices;   // eEnum only
	};

	// Loads the shipped config if it hasn't been loaded yet. Safe to call
	// repeatedly; only the first call reads the file.
	void ensureLoaded(const std::string &settingsFile);

	// The curated list, in the order the setup screen should show it. Not
	// every OptionsGame entry: upstream has around two hundred, most of them
	// server-administration details that mean nothing on a phone.
	std::vector<Option> options();

	// Returns false if the name isn't one of the exposed options, or if the
	// value is rejected by upstream's own validation (out of range for a
	// bounded int, not one of an enum's values). Rejection is upstream's
	// decision, not a rule invented here.
	bool set(const std::string &name, const std::string &value);

	// Forget every choice and go back to what the config file says.
	void reset();

	// M12: replaces the current options with those in [path], for the
	// tutorial.
	//
	// Upstream's data/singletutorial.xml is the reusable half of its tutorial:
	// seven inert "Target" players instead of bots that shoot back, no shot
	// clock and no buying phase, generous money, a capped arms level, light
	// steady wind and a short list of simple landscapes. That is exactly the
	// setup a first game wants, and it is only an options file - unlike the
	// step-by-step content, whose runner is client-only and whose conditions
	// name upstream dialogs that do not exist here.
	//
	// Returns false if the file cannot be read, leaving the options untouched
	// rather than half-applied.
	bool loadPreset(const std::string &path);

	// The mods available to choose, always with "none" first. Read from the
	// data directory rather than hardcoded, so a mod dropped in alongside
	// upstream's own appears without a code change.
	std::vector<std::string> mods(const std::string &dataRoot);

	// "none" for upstream's base game. Unlike every other option here, this
	// one cannot be applied after the server starts: startServerInternal()
	// calls setDataFileMod() and loadModFiles() partway through its own
	// startup, long before applyTo() could run. That is what writeSessionFile
	// below exists for.
	std::string mod();
	bool setMod(const std::string &name);

	// The bot AIs a mod actually provides.
	//
	// Comment-aware, which is the whole point: the Apocalypse mod's
	// data/tankais.xml *contains* definitions for Shark, Moron and three
	// others, but they sit inside an XML comment, so upstream's parser never
	// sees them and the mod really does offer only seven. A scan that missed
	// that would report bots the engine cannot create - which is precisely
	// the bug this exists to prevent, and precisely the mistake made while
	// diagnosing it.
	//
	// Empty if the file cannot be read, which callers must treat as "don't
	// know" rather than "no bots".
	std::vector<std::string> botNames(const std::string &dataRoot, const std::string &mod);

	// Replaces any configured bot AI the chosen mod does not provide with one
	// it does, and returns how many entries changed.
	//
	// The shipped config asks for "Moron", which the base game defines and
	// Apocalypse does not. Without this, choosing that mod produced a game
	// that loaded its landscape and then waited forever for a bot that could
	// never be created, logging "Failed to find a tank ai called Moron" every
	// tick where nothing surfaced it.
	int ensureBotsValidForMod(const std::string &dataRoot);

	// Writes the chosen options to [path] as a config file for the server to
	// read at startup, and returns false if it could not be written.
	//
	// This is how the mod choice takes effect, and the reason it is worth
	// doing for everything rather than only the mod: options that arrive
	// through the config file are in place before startServerInternal() reads
	// any of them, and they are what OptionsScorched snapshots, so nothing has
	// to be re-applied afterwards and nothing can be reverted by
	// commitChanges().
	bool writeSessionFile(const std::string &path);

	// Copies the chosen values into a running server's options.
	//
	// MUST be followed by OptionsScorched::updateChangeSet(), and the caller
	// does that rather than this function, because it is the caller that knows
	// the server is fully started. Without it every value here is silently
	// undone at the first round: OptionsScorched snapshots the main options
	// during startServer(), and ServerStateNewGame calls commitChanges(),
	// which copies that snapshot back over them. The entry then still reads as
	// "changed" while holding the file's value, which is exactly as confusing
	// as it sounds - it cost an afternoon when the debug money flag hit it.
	void applyTo(OptionsScorched &options);
}

#endif  // SCORCHDROID_GAME_SETUP_H
