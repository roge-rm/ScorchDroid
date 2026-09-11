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

#ifndef SCORCHDROID_BRIDGE_TRANSPORT_HPP
#define SCORCHDROID_BRIDGE_TRANSPORT_HPP

/**
 * A link that carries whole messages between devices and knows nothing about
 * what is in them.
 *
 * This interface exists so that the hard half of a non-IP transport can be
 * built and tested without the transport. Everything Scorched3D's networking
 * expects - destination ids, connect and disconnect messages, the hand-off
 * between socket threads and the engine thread - lives in NetBridge above
 * this line, with no Android in it; everything below it is sockets and
 * threads with no game in it. The Android implementation is Bluetooth RFCOMM
 * (JniTransport, calling into BluetoothTransport.kt); the one the tests use
 * is a Unix domain socket pair, which is how the whole client-join handshake
 * can be run through this path on a build machine.
 *
 * **Framing is the transport's job.** NetBridge hands over a message and
 * expects a message to arrive, never half of one and never two at once.
 *
 * Peer ids are allocated by the transport, because it is the side that
 * discovers peers. They must be non-zero and unique for the life of the
 * transport - never reused for a later peer, since a stale id arriving late
 * would otherwise deliver one game's traffic into another's destination.
 * NetBridge uses them directly as Scorched3D destination ids.
 */
class BridgeTransportSink
{
public:
	virtual ~BridgeTransportSink();

	/** A peer is connected and can be sent to. Any transport thread. */
	virtual void onPeerConnected( unsigned int peerId ) = 0;

	/**
	 * One whole message from [peerId]. The bytes belong to the caller and
	 * must be copied before this returns. Any transport thread.
	 */
	virtual void onPayload( unsigned int peerId, const unsigned char* bytes, unsigned int length ) = 0;

	/** A peer has gone, for any reason including being asked to. */
	virtual void onPeerDisconnected( unsigned int peerId ) = 0;

	/**
	 * The transport itself failed, rather than one peer - a Bluetooth
	 * connect that never completed, a listen that could not start. Reported
	 * separately because a client that cannot reach the host at all has no
	 * peer id to be disconnected from, and the player still has to be told.
	 */
	virtual void onTransportFailed( const char* reason ) = 0;
};

class BridgeTransport
{
public:
	virtual ~BridgeTransport();

	/** Called once, before anything else. */
	virtual void setSink( BridgeTransportSink* sink ) = 0;

	/** Accept peers. Returns whether listening began, not whether any came. */
	virtual bool startListening() = 0;

	/**
	 * Reach one peer - a Bluetooth MAC address, a file descriptor, whatever
	 * this transport names peers with. Returns whether the attempt started:
	 * success arrives as onPeerConnected, failure as onTransportFailed,
	 * because on a real radio this takes seconds.
	 */
	virtual bool connectTo( const char* endpoint ) = 0;

	/** One whole message. False if it could not be sent at all. */
	virtual bool send( unsigned int peerId, const unsigned char* bytes, unsigned int length ) = 0;

	/** Drop one peer. onPeerDisconnected follows. */
	virtual void disconnect( unsigned int peerId ) = 0;

	/** Drop everything and stop. Must be safe to call twice. */
	virtual void stop() = 0;
};

#endif  // SCORCHDROID_BRIDGE_TRANSPORT_HPP
