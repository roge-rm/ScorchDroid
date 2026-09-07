#ifndef __INCLUDE_ClientSync_hpp_INCLUDE__
#define __INCLUDE_ClientSync_hpp_INCLUDE__

// M5 client-join: a minimal reimplementation of upstream's
// src/client/client/ClientSimulator.cpp - excluded because it mixes in
// GameStateI/ShotCountDown (rendering-loop interfaces this port doesn't
// have) - keeping only the real logic: queue SimActions delivered by
// incoming ComsSimulateMessage messages, and refuse to simulate past the
// last message's event time (never get ahead of what the host has actually
// sent). See ClientContext.hpp for how this fits into the join handshake,
// and the porting plan for the full design.
#include <engine/Simulator.hpp>
#include <coms/ComsSimulateMessage.hpp>
#include <common/RollingAverage.hpp>

class ClientSync : public Simulator
{
public:
	ClientSync();
	virtual ~ClientSync();

	// Enqueues the SimActions from a ComsSimulateMessage (received live
	// during play, or replayed from a ComsLoadLevelMessage's buffered
	// history when first joining - see ClientContext::processMessage).
	void addComsSimulateMessage( ComsSimulateMessage& message );

	// The clock-drift correction half of upstream's
	// ClientSimulator::processComsSimulateMessage - call it only for a
	// message received *live*, never for one replayed out of the level
	// message's buffered history (those carry old timestamps, and upstream
	// skips this for exactly that reason via its loadingLevel_ flag).
	//
	// Without this a joined client runs permanently behind the host: its
	// clock is set once from the level message's actualTime, but its own
	// level load (ComsLoadLevelMessage::loadTanks' SDL_Delay(100) plus
	// generateMaps) has already burned several hundred milliseconds by
	// then, and nothing ever catches it back up. Not a desync - the host
	// stays authoritative and every action still fires at its own event
	// time - but everything is displayed late by that constant offset.
	void syncToServerTime( fixed serverActualTime );

	// The host sends a ComsNetStatMessage every couple of seconds once a
	// destination is loaded (see ServerSimulator::processMessage). Its
	// round-trip time feeds syncToServerTime's estimate of how stale a
	// just-arrived message already is.
	void setNetStat( fixed roundTripTime, fixed sendStepSize );

	// Fast-forwards the simulator to the given absolute time, used once
	// while joining to catch up to the host's current time (see
	// ClientLoadLevelHandler.cpp's real equivalent, upstream).
	void setSimulationTime( fixed actualTime );

	virtual void newLevel();

	// Network-quality readouts, for a future HUD - upstream shows these in
	// its own network-stats display.
	// clang-format off
	fixed getServerStepTime()       { return serverStepTime_; }
	fixed getServerRoundTripTime()  { return serverRoundTripTime_; }
	fixed getServerTimeDifference() { return serverTimeDifference_.getAverage(); }
	// clang-format on

protected:
	virtual bool continueToSimulate();

private:
	// The event time of the most recently received ComsSimulateMessage -
	// continueToSimulate() caps the simulator here so it never processes
	// actions the host hasn't actually sent yet. Note the consequence: a
	// message's actions only fire once the *following* message lifts this
	// cap, so a queued action is always one message behind. That is
	// upstream's behaviour too, not an artifact of this port.
	fixed waitingEventTime_;

	// Rolling average of (host time - our time), and the correction applied
	// from it. Averaged rather than applied raw because a single sample is
	// mostly jitter; sized to match upstream's own 10.
	RollingAverage serverTimeDifference_;
	fixed          serverStepTime_, serverRoundTripTime_;
};

#endif  // __INCLUDE_ClientSync_hpp_INCLUDE__
