////////////////////////////////////////////////////////////////////////////////
//    ScorchDroid - Scorched3D for Android
//
//    This file is part of ScorchDroid.
//
//    ScorchDroid is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    ScorchDroid is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License along
//    with this program; if not, write to the Free Software Foundation, Inc.,
//    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
////////////////////////////////////////////////////////////////////////////////

#ifndef SCORCHDROID_NET_BRIDGE_HPP
#define SCORCHDROID_NET_BRIDGE_HPP

#include <BridgeTransport.hpp>
#include <SDL_thread_compat.h>
#include <net/NetInterface.hpp>
#include <net/NetMessageHandler.hpp>
#include <set>

/**
 * A Scorched3D NetInterface over anything that can carry a message
 * (BridgeTransport) - which is how the game runs over a radio that is not a
 * network at all, Bluetooth being the one that matters.
 *
 * Every other NetInterface here is an IP socket. This one deliberately is
 * not, and the engine cannot tell: `NetInterface` is genuinely polymorphic
 * with no downcasts anywhere, and a transport with no IP address can report
 * `ipAddress == 0` - NetLoopBack already does, and the server's ban and
 * AllowSameIP checks are guarded on `!= 0` (ServerMessageHandler.cpp).
 * `connect()`'s `hostName` is passed to the transport verbatim and its
 * `portNo` ignored, so a Bluetooth MAC address travels the existing call
 * path with no signature change.
 *
 * The shape is NetServerTCP3's, because the engine depends on that shape:
 *
 * - **Two queues.** Incoming messages are handed to the engine thread by
 *   `processMessages()`, exactly as every caller already expects; outgoing
 *   ones are queued and written by this object's own thread. Both are
 *   `NetMessageHandler`, which is already the thread-safe hand-off the TCP
 *   interface uses, rather than a queue hand-rolled here.
 * - **The engine thread never waits on the radio.** A Bluetooth write can
 *   block for a long time, and the tick loop calling `sendMessageDest`
 *   must not be where that happens.
 *
 * One instance per process. Two NetInterfaces in one process misroute
 * messages through the shared NetMessagePool singleton - see
 * host-tests/main.cpp, which documents where that was found - and
 * engine_jni.cpp's mutually exclusive host/client modes already enforce it.
 */
class NetBridge : public NetInterface, public NetMessageHandlerI, public BridgeTransportSink
{
public:
	/**
	 * [transport] is owned from here on, including when construction is
	 * followed by a failed start: there is no state in which the caller is
	 * still responsible for it.
	 */
	NetBridge( BridgeTransport* transport );
	virtual ~NetBridge();

	// NetInterface
	virtual bool started();
	virtual bool connect( const char* hostName, int portNo );
	virtual bool start( int portNo );
	virtual void stop();

	virtual int  processMessages();
	virtual void setMessageHandler( NetMessageHandlerI* handler );

	virtual void disconnectAllClients();
	virtual void disconnectClient( unsigned int destination );
	virtual void disconnectClient( NetBuffer& buffer, unsigned int destination );
	virtual void sendMessageServer( NetBuffer& buffer, unsigned int flags = 0 );
	virtual void sendMessageDest( NetBuffer& buffer, unsigned int destination, unsigned int flags = 0 );

	// NetMessageHandlerI - the outgoing queue, drained on the send thread.
	virtual void processMessage( NetMessage& message );

	// BridgeTransportSink - called on the transport's own threads.
	virtual void onPeerConnected( unsigned int peerId );
	virtual void onPayload( unsigned int peerId, const unsigned char* bytes, unsigned int length );
	virtual void onPeerDisconnected( unsigned int peerId );
	virtual void onTransportFailed( const char* reason );

protected:
	BridgeTransport*  transport_;
	NetMessageHandler incomingMessageHandler_;
	NetMessageHandler outgoingMessageHandler_;

	// The peer this interface calls "the server", set by the first peer to
	// connect after connect() - a client only ever has one. UINT_MAX when
	// this side is hosting, matching NetServerTCP3's own "no server
	// destination" value.
	unsigned int serverDestinationId_;

	// Which end this is. Not a question the transport can answer - it has
	// one link either way - but it decides whether the first peer to arrive
	// is "the server" or just another player.
	bool hosting_;

	SDL_Thread*            sendThread_;
	volatile bool          stopped_;
	pthread_mutex_t        peersMutex_;
	std::set< unsigned int > peers_;

	bool startProcessing();
	void actualSendFunc();
	static int sendThreadFunc( void* );

	void sendMessageTypeDest(
		NetBuffer&              buffer,
		unsigned int            destination,
		unsigned int            flags,
		NetMessage::MessageType type
	);

	/**
	 * Tells the engine a peer has gone, once. Both the transport noticing a
	 * dropped link and this side kicking a client end up here, and a
	 * destination that has already been removed must not report a second
	 * disconnect - the engine treats that as a second player leaving.
	 */
	void peerGone( unsigned int peerId, NetMessage::DisconnectFlags flags );

	bool isKnownPeer( unsigned int peerId );
};

#endif  // SCORCHDROID_NET_BRIDGE_HPP
