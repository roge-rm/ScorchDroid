#ifndef SCORCHDROID_PLAYER_PROFILE_H
#define SCORCHDROID_PLAYER_PROFILE_H

#include <string>

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
namespace ScorchDroidProfile
{
	// Defaults to "Player" until the UI sets one, so a game started before
	// settings are read still has a named tank.
	std::string name();

	// Ignores an empty or whitespace-only name rather than letting a tank end
	// up nameless; returns what the name is afterwards.
	std::string setName(const std::string &name);
}

#endif  // SCORCHDROID_PLAYER_PROFILE_H
