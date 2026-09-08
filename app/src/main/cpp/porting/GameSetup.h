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
