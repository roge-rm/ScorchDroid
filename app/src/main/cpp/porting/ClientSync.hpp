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

class ClientSync : public Simulator
{
public:
	ClientSync();
	virtual ~ClientSync();

	// Enqueues the SimActions from a ComsSimulateMessage (received live
	// during play, or replayed from a ComsLoadLevelMessage's buffered
	// history when first joining - see ClientContext::processMessage).
	void addComsSimulateMessage( ComsSimulateMessage& message );

	// Fast-forwards the simulator to the given absolute time, used once
	// while joining to catch up to the host's current time (see
	// ClientLoadLevelHandler.cpp's real equivalent, upstream).
	void setSimulationTime( fixed actualTime );

	virtual void newLevel();

protected:
	virtual bool continueToSimulate();

private:
	// The event time of the most recently received ComsSimulateMessage -
	// continueToSimulate() caps the simulator here so it never processes
	// actions the host hasn't actually sent yet.
	fixed waitingEventTime_;
};

#endif  // __INCLUDE_ClientSync_hpp_INCLUDE__
