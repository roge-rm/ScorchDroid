#ifndef SCORCHDROID_PLAN_LINE_STORE_H
#define SCORCHDROID_PLAN_LINE_STORE_H

#include <vector>

// Android build: plan lines drawn by *other* players, waiting to be picked up
// by the HUD.
//
// Upstream's plan drawing (GLWPlanView, ComsLinesMessage) reaches a client
// two different ways depending on which end of the game you are, and this
// port has the same split as it does for chat:
//
//  - **Joined**, lines arrive as a ComsLinesMessage relayed by the host, and
//    ClientContext's handler pushes them straight in here - the same route
//    and the same shape as ComsChannelTextMessage and the chat log.
//  - **Hosting**, there is no socket to yourself and no client handler to
//    run. The host's copy of a client's line comes from ServerLinesHandler,
//    which is ordinary src/server code this build already compiles and which
//    already does every check that matters. Patch 0024 adds one call to it
//    rather than registering a second handler for the type: ComsMessageHandler
//    keeps exactly one handler per message id (recvHandlers_[id] = handler),
//    so a second registration would silently *replace* the relay and nobody
//    would receive anything at all.
//
// Nothing the local player draws comes through here. The sender shows their
// own line at once the way sendChat does, and upstream's relay deliberately
// skips the destination it arrived from, so there is no echo to filter.
//
// Coordinates stay exactly as they travel - upstream's 0-1 fractions of the
// plan widget - and are turned into landscape coordinates in Kotlin, next to
// the inverse that put them on the wire, so the two conversions cannot drift
// apart.
namespace ScorchDroidPlanLines
{
	struct Line
	{
		// Monotonic, assigned on push. The HUD polls "everything after the
		// last id I saw", the same as the chat log: it fades each line on
		// its own clock and must not restart one already on screen.
		unsigned int id = 0;
		// Whose line it is. The HUD colours it with that tank's colour, as
		// upstream's drawLine does, and drops it if the tank has gone.
		unsigned int playerId = 0;
		// Fractions of the plan widget, upstream's convention throughout:
		// x rightwards, y *upwards*.
		float ax = 0.0f;
		float ay = 0.0f;
		float bx = 0.0f;
		float by = 0.0f;
	};

	// Splits a ComsLinesMessage's point list into whole lines and stores
	// them. Upstream's strokes are polylines with a null vector between
	// them, so an incoming stroke of n points becomes n-1 lines - which is
	// what makes a PC client's freehand scribble show up here at all,
	// rather than only the two-point lines this port sends.
	void pushStroke(unsigned int playerId, const std::vector<float> &interleavedXy);

	// Lines newer than an id the caller has already seen.
	std::vector<Line> since(unsigned int afterId);

	// Bumped on every push, so the HUD can poll one integer.
	unsigned int version();

	// Dropped when a new game starts - the lines pointed at a landscape
	// that has gone.
	void clear();
}

#endif  // SCORCHDROID_PLAN_LINE_STORE_H
