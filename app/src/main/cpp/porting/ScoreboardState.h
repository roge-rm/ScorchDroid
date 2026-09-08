#ifndef SCORCHDROID_SCOREBOARD_STATE_H
#define SCORCHDROID_SCOREBOARD_STATE_H

// The end-of-round scoreboard, which upstream puts on screen by itself.
//
// ShowScoreAction is queued by ServerStateScore at the end of every round
// and again at the end of the match, and it holds the game there for
// RoundScoreTime (5s) or ScoreTime (15s) respectively. Upstream's client
// half of that action stimulates its own ClientState::StimScore, which is
// what raises the score table - and, like every other piece of
// presentation in src/common/actions, it lives behind #ifndef S3D_SERVER
// and is absent here. So this port simulated the pause correctly and
// showed nothing during it: the score table existed, but only if the
// player thought to open it by hand, and the round was over by then.
//
// This is the same hook pattern as SoundEventQueue and EffectEventQueue,
// but a *state* rather than a queue: there is exactly one scoreboard and
// what matters is whether it is up, not how many times it was asked for.
// The UI polls it on its existing tick and opens or closes the score
// dialog to match.
namespace ScorchDroidScoreboard
{
	struct State
	{
		bool showing = false;
		// The match is over, not just the round - upstream shows this one
		// for ScoreTime rather than RoundScoreTime and calls it the final
		// score. Kept so the UI can title it accordingly.
		bool finalScore = false;
	};

	// Called from the patched ShowScoreAction as the round ends.
	void show(bool finalScore);

	// Called when that action's timer runs out and play resumes.
	void hide();

	State get();
}

#endif  // SCORCHDROID_SCOREBOARD_STATE_H
