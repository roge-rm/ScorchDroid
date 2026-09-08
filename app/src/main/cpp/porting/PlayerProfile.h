#ifndef SCORCHDROID_PLAYER_PROFILE_H
#define SCORCHDROID_PLAYER_PROFILE_H

#include <string>
#include <vector>

class ComsTankChangeMessage;
class TankModelStore;
class Vector;

// M11: who the player is, as far as the engine is concerned.
//
// Small, but it has to be shared: the name is needed in two places that know
// nothing about each other. Hosting, engine_jni.cpp's addHumanTank() names the
// tank directly; joining, ClientContext puts it in the connect message and the
// *host* names the tank from that. Before this both were hardcoded - every
// host was "Player" and every joiner announced itself as "ScorchDroid" - which
// is poor in the one place it matters, a game with other people in it.
//
// Upstream's equivalent is the OnlineUserName display option. It is kept here
// rather than in GameSetup because it is a property of the person, not of the
// game being set up: it should not reset when the game options do.
//
// M16 adds the rest of upstream's PlayerDialog to it - the tank model, the
// colour and the avatar - for the same reason and through the same two paths.
// Both of those paths already build a ComsTankChangeMessage, which is exactly
// the message upstream's own dialog sends, so the choices land through
// upstream's own machinery rather than by reaching into the tank.
namespace ScorchDroidProfile
{
	// Defaults to "Player" until the UI sets one, so a game started before
	// settings are read still has a named tank.
	std::string name();

	// Ignores an empty or whitespace-only name rather than letting a tank end
	// up nameless; returns what the name is afterwards.
	std::string setName(const std::string &name);

	// --- M16: the rest of the identity ------------------------------------

	// The tank models a mod offers, in the order its own tanks.xml lists
	// them. Read from the file rather than from TankModelStore because the
	// settings screen exists long before any server does - the same reason
	// GameSetup keeps its own OptionsGame.
	//
	// Note this lists what the mod *declares*, which is what a picker wants;
	// whether a given model suits a given team is a question only the running
	// game can answer, and modelFor() below asks it there.
	std::vector<std::string> models(const std::string &dataRoot, const std::string &mod);

	// Upstream's own tank palette, as 0xRRGGBB. Twenty-six colours, in
	// TankColorGenerator's own order, which is also the order tanks are
	// allocated them - so the first entry is the colour a solo player has
	// been getting all along.
	std::vector<unsigned int> colors();

	// The avatars shipped under data/avatars, as paths relative to the data
	// root. Upstream ships nineteen and lets a player send any PNG; this
	// offers the shipped ones, which every other player already has.
	std::vector<std::string> avatars(const std::string &dataRoot);

	// The player's choices. Each has an "unchosen" value that means "let the
	// game decide", which is what happened before this existed: an empty
	// model name, a negative colour index, an empty avatar path.
	std::string model();
	void setModel(const std::string &name);
	int colorIndex();
	void setColorIndex(int index);
	std::string avatar();
	void setAvatar(const std::string &path);

	// The model name to put in a tank-change message: the player's own if
	// they chose one and the store still has it, otherwise a random one for
	// this team and tanket type, exactly as before.
	//
	// The store is asked rather than trusted, because a mod change can leave
	// a chosen model behind - pick "Tiger II", switch to a mod without it,
	// and the message would name a model the game cannot resolve.
	std::string modelFor(TankModelStore &store, int team, const char *tankType);

	// The colour for the same message: the player's own if they chose one,
	// else [current], which is the colour the engine allocated when the tank
	// was created. The engine still has the last word - TankChangeSimAction
	// ignores a colour another tank is already using.
	Vector colorFor(const Vector &current);

	// Loads the chosen avatar into [message], if one is chosen and readable.
	// A missing file leaves the message alone rather than failing: an avatar
	// is decoration, and losing it should not cost a player their tank.
	void applyAvatar(ComsTankChangeMessage &message);
}

#endif  // SCORCHDROID_PLAYER_PROFILE_H
