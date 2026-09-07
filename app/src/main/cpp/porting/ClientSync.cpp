#include <ClientSync.hpp>
#include <SDL_thread_compat.h>

// Upstream's own averaging window and correction divisor - see
// ClientSimulator's constructor and processComsSimulateMessage. Kept
// identical rather than retuned: the point is to behave like a real client
// on a real host, and these interact with the host's own send-step pacing.
static const int   kTimeDifferenceAverages = 10;
static const Sint64 kTimeCorrectionDivisor = 20;

ClientSync::ClientSync() :
	waitingEventTime_(0),
	serverTimeDifference_(kTimeDifferenceAverages, 0),
	serverStepTime_(0),
	serverRoundTripTime_(0)
{
}

ClientSync::~ClientSync()
{
}

bool ClientSync::continueToSimulate()
{
	return currentTime_ < waitingEventTime_;
}

void ClientSync::addComsSimulateMessage(ComsSimulateMessage &message)
{
	waitingEventTime_ = message.getEventTime();

	std::list<SimAction *>::iterator itor;
	for (itor = message.getActions().begin();
		itor != message.getActions().end();
		++itor)
	{
		simActions_.push_back(new SimActionContainer(*itor, waitingEventTime_));
	}
}

void ClientSync::syncToServerTime(fixed serverActualTime)
{
	// The message left the host at serverActualTime and spent roughly half
	// a round trip getting here, so the host is now about that much further
	// on - upstream's exact estimate.
	fixed difference = (serverActualTime + serverRoundTripTime_ / 2) - actualTime_;
	serverTimeDifference_.addValue(difference);

	// Nudge rather than jump. Snapping actualTime_ straight onto the host's
	// clock would make the simulation stutter forwards on every message
	// (and could step it backwards on a late one, which Simulator has no
	// way to represent), so close a twentieth of the gap each time and let
	// it converge over a second or so.
	actualTime_ += serverTimeDifference_.getAverage() / fixed(kTimeCorrectionDivisor);
}

void ClientSync::setNetStat(fixed roundTripTime, fixed sendStepSize)
{
	serverRoundTripTime_ = roundTripTime;
	serverStepTime_ = sendStepSize;
}

void ClientSync::newLevel()
{
	while (!simActions_.empty())
	{
		SimActionContainer *container = simActions_.front();
		delete container;
		simActions_.pop_front();
	}

	Simulator::newLevel();
	lastTickTime_ = SDL_GetTicks() - 1;

	// A new level restarts the clock at zero on both ends, so any drift
	// measured against the old one is meaningless.
	serverTimeDifference_.reset(0);
}

void ClientSync::setSimulationTime(fixed actualTime)
{
	lastTickTime_ = SDL_GetTicks();
	actualTime_ = actualTime;

	// This is an explicit jump to a known-good host time (the join
	// fast-forward), so the accumulated difference is stale by definition.
	serverTimeDifference_.reset(0);

	simulateTime();
}
