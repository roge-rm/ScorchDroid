#ifndef __INCLUDE_ClientContext_hpp_INCLUDE__
#define __INCLUDE_ClientContext_hpp_INCLUDE__

// M5 client-join, Phase 1: a from-scratch "client mode" ScorchedContext,
// standing in for upstream's excluded src/client/client/ScorchedClient -
// see the porting plan for the full design and why this is safe to build
// as new code rather than porting that class (it turned out to be just
// ScorchedContext + rendering-only members once traced through).
//
// Drives the real connect handshake (ComsConnectMessage -> ComsConnect-
// AuthMessage -> ComsConnectAcceptMessage -> mod-file check -> Coms-
// LoadLevelMessage -> ComsLevelLoadedMessage) against a real NetServerTCP3
// socket, using only public APIs already compiled into scorched_common -
// no upstream patch needed, same pattern as engine_jni.cpp's
// addHumanTank()/fireWeapon(). Once joined, tick() pumps incoming
// ComsSimulateMessage traffic into a ClientSync (see ClientSync.hpp) the
// same way ServerSimulator/Simulator already do server-side.
//
// Deliberately NOT wired into engine_jni.cpp yet (see the porting plan's
// M5 "Phase 2" notes) - this class is exercised standalone by
// host-tests/main.cpp first, against a real ScorchedServer over real
// loopback sockets, before the bigger engine_jni.cpp refactor to let the
// Android UI pick "host" vs "join".

#include <engine/ScorchedContext.hpp>
#include <coms/ComsMessageHandler.hpp>
#include <ClientSync.hpp>
#include <string>

class TargetSpace;

class ClientContext : public ScorchedContext, public ComsMessageHandlerI, public ComsMessageConnectionHandlerI
{
public:
	enum State
	{
		sIdle,
		sConnecting,
		sWaitingAuthChallenge,
		sWaitingConnectAccept,
		sWaitingLoadLevel,
		sJoined,
		sFailed
	};

	ClientContext();
	virtual ~ClientContext();

	// ScorchedContext
	virtual bool          getServerMode() { return false; }
	virtual TargetSpace&  getTargetSpace() { return *targetSpace_; }
	virtual Simulator&    getSimulator();

	// Starts the connection - opens a real socket immediately, but the
	// handshake itself only progresses via tick() below (matches how
	// tickEngine() already drives everything server-side, one poll at a
	// time, rather than blocking).
	bool connectToServer( const char* host, int port );

	// Call every frame: pumps the network (advancing the handshake state
	// machine as replies arrive) and, once sJoined, simulates.
	void tick();

	State              getState() { return state_; }
	const std::string& getFailureReason() { return failureReason_; }

	// The destinationId the host assigned us in ComsConnectAcceptMessage -
	// mirrors kHumanDestinationId's role in engine_jni.cpp's server-mode
	// path: TankAddSimAction (relayed to us like any other client via
	// ComsSimulateMessage, see ClientSync) sets this same destinationId on
	// "my tank", so callers identify their own tank identically in either
	// mode by comparing against this value. Valid only once getState() has
	// reached sJoined (or later); 0 beforehand.
	unsigned int getMyDestinationId() { return myDestinationId_; }

	// Phase 2: lets engine_jni.cpp forward a real client action (a played
	// move, a buy/sell request, ...) to the host exactly as a real client
	// UI would, reusing the same message classes and server-side handlers
	// (ServerPlayedMoveHandler, ServerBuyAccessoryHandler, ...) that were
	// already compiled in and wired up back in M1 - nothing new needed
	// server-side. Only valid once sJoined; returns false (no-op) otherwise
	// so callers can fail the same way findMyTank()==nullptr already does
	// elsewhere in engine_jni.cpp.
	bool sendGameMessage( ComsMessage& message );

	// ComsMessageHandlerI - registered for each message type this client
	// needs to react to (see the constructor).
	virtual bool processMessage( NetMessage& message, const char* messageType, NetBufferReader& reader );

	// ComsMessageConnectionHandlerI
	virtual void clientConnected( NetMessage& message );
	virtual void clientDisconnected( NetMessage& message );
	virtual void clientError( NetMessage& message, const std::string& errorString );
	virtual void messageRecv( unsigned int destinationId ) {}
	virtual void messageSent( unsigned int destinationId ) {}

private:
	// Replicates ComsMessageSender::sendToServer() (upstream's real
	// version is #ifndef S3D_SERVER'd out of this build entirely, since it
	// hardcodes ScorchedClient::instance() - see ComsMessageSender.cpp) -
	// same ~10 lines, against this context instead.
	void sendToServer( ComsMessage& message, unsigned int flags = 0 );

	void fail( const std::string& reason );

	// A freshly-joined tank sits in TankState::sLoading/sSpectator (not
	// "playing" - see TankState::getTankPlaying()) until something sends a
	// ComsTankChangeMessage(spectate=false) for it, exactly as a real
	// client's "choose your tank" dialog would - ServerTankChangeHandler
	// (already wired up server-side since M1) does the rest, queuing the
	// TankChangeSimAction that actually promotes it. Without this,
	// ServerStateEnoughPlayers::enoughPlayers() can never count this tank
	// and the host-side match never leaves ServerWaitingForPlayersState -
	// found the hard way chasing why a fired shot never arrived. Sent once,
	// as soon as our own tank exists locally (mirrored via ComsSimulateMessage
	// like everything else - see ClientSync); a no-op if it never shows up
	// (the known Phase 1 "own tank" gap).
	void sendTankChangeIfNeeded();

	static TargetSpace* targetSpace_;

	ClientSync*  clientSync_;
	State        state_;
	std::string  failureReason_;
	unsigned int myDestinationId_ = 0;
	bool         tankChangeSent_ = false;
};

#endif  // __INCLUDE_ClientContext_hpp_INCLUDE__
