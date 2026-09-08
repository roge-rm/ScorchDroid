#ifndef SCORCHDROID_CHAT_STORE_H
#define SCORCHDROID_CHAT_STORE_H

#include <string>
#include <vector>

// Android build: the in-game chat log.
//
// Unlike the effect/sound/deform hooks, this needs no patch. Chat does not
// go through a client-only drawing path - it goes through the *server's*
// ServerChannelManager, which this port compiles, and which already keeps
// its own rolling log of the last 25 messages (getLastMessages(), used by
// upstream's server console) with a monotonically increasing id per entry.
// Polling that is enough for the hosting side.
//
// The joining side is different: the server only sends a channel's text to
// destinations that have registered for that channel with a
// ComsChannelMessage, which upstream's ClientChannelManager does and which
// this port's ClientContext now does too. Its incoming
// ComsChannelTextMessage handler pushes straight in here, so both sides end
// up filling the same log by different routes.
//
// Deliberately holds formatted strings rather than LangString: everything
// downstream of here is a Kotlin dialog, and the conversion has to happen
// somewhere.
namespace ScorchDroidChat
{
	struct Line
	{
		// Monotonic, assigned on push. The HUD shows each message for a few
		// seconds and then drops it, so it needs to tell a message it has
		// already started timing from one that has just arrived - which it
		// cannot do by content, since the same player can say the same
		// thing twice.
		unsigned int id = 0;
		// Upstream's channel name: "general", "team", "info", "announce",
		// "combat", "banner", "admin", "whisper", "spam". Worth keeping
		// rather than flattening, because the UI colours by it and because
		// "info"/"combat" are the game talking, not a player.
		std::string channel;
		// The speaking player's name, empty when the server itself is
		// talking (a join notice, a kill message).
		std::string who;
		std::string text;
	};

	void push(const Line &line);

	// The whole log, oldest first. Bounded - see kMaxLines in the .cpp.
	std::vector<Line> snapshot();

	// Just the lines newer than an id the caller has already seen, which is
	// what the transient HUD stack wants - it starts a timer per message and
	// must not restart one it is already showing.
	std::vector<Line> since(unsigned int afterId);

	// Bumped on every push. The UI polls this rather than rebuilding a
	// string list every frame just to find out nothing changed.
	unsigned int version();

	// Dropped when a new game starts, the way upstream's client console is.
	void clear();
}

#endif  // SCORCHDROID_CHAT_STORE_H
