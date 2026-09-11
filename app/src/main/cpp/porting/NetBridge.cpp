#include <NetBridge.hpp>
#include <net/NetMessagePool.hpp>
#include <common/Logger.hpp>
#include <common/Defines.hpp>
#include <limits.h>
#include <string.h>

BridgeTransportSink::~BridgeTransportSink()
{
}

BridgeTransport::~BridgeTransport()
{
}

NetBridge::NetBridge(BridgeTransport *transport)
	: transport_(transport)
	, serverDestinationId_(UINT_MAX)
	, hosting_(false)
	, sendThread_(nullptr)
	, stopped_(false)
{
	pthread_mutex_init(&peersMutex_, nullptr);
	if (transport_) transport_->setSink(this);
}

NetBridge::~NetBridge()
{
	stop();
	delete transport_;
	transport_ = nullptr;
	pthread_mutex_destroy(&peersMutex_);
}

bool NetBridge::started()
{
	return (nullptr != sendThread_);
}

bool NetBridge::start(int portNo)
{
	if (!transport_) return false;
	stop();
	hosting_ = true;
	if (!transport_->startListening())
	{
		Logger::log("NetBridge: transport would not start listening");
		return false;
	}
	if (!startProcessing()) return false;
	return true;
}

bool NetBridge::connect(const char *hostName, int portNo)
{
	if (!transport_) return false;
	stop();
	hosting_ = false;
	// The port is genuinely meaningless here and is not passed on: a
	// Bluetooth service is named by a UUID the transport already knows, and
	// the endpoint is the whole of the address. connect()'s signature is
	// upstream's and is left alone - see this class's header.
	if (!transport_->connectTo(hostName))
	{
		Logger::log(S3D::formatStringBuffer("NetBridge: transport would not reach %s", hostName));
		return false;
	}
	if (!startProcessing()) return false;
	return true;
}

bool NetBridge::startProcessing()
{
	DIALOG_ASSERT(!sendThread_);

	stopped_ = false;
	// Everything queued for sending comes back to processMessage() below,
	// on the thread started here rather than on the caller's.
	outgoingMessageHandler_.setMessageHandler(this);

	sendThread_ = SDL_CreateThread(NetBridge::sendThreadFunc, (void *) this);
	if (nullptr == sendThread_)
	{
		Logger::log("NetBridge: failed to create the send thread");
		return false;
	}
	return true;
}

int NetBridge::sendThreadFunc(void *c)
{
	NetBridge *th = (NetBridge *) c;
	th->actualSendFunc();
	th->sendThread_ = nullptr;
	Logger::log("NetBridge: shutdown");
	return 0;
}

void NetBridge::actualSendFunc()
{
	while (!stopped_)
	{
		outgoingMessageHandler_.processMessages();
		// Nothing here is polling a socket - receiving happens on the
		// transport's own threads - so this loop only exists to pick up
		// what the engine thread queued, and can afford to be gentle.
		SDL_Delay(5);
	}
	// Anything still queued was queued before the stop and is now
	// undeliverable; the handler returns it to the pool on destruction.
}

void NetBridge::stop()
{
	if (!started()) return;

	SDL_Thread *localSendThread = sendThread_;
	stopped_ = true;
	if (transport_) transport_->stop();

	int status = 0;
	SDL_WaitThread(localSendThread, &status);
	sendThread_ = nullptr;

	pthread_mutex_lock(&peersMutex_);
	peers_.clear();
	pthread_mutex_unlock(&peersMutex_);
	serverDestinationId_ = UINT_MAX;
}

int NetBridge::processMessages()
{
	return incomingMessageHandler_.processMessages();
}

void NetBridge::setMessageHandler(NetMessageHandlerI *handler)
{
	incomingMessageHandler_.setMessageHandler(handler);
}

void NetBridge::sendMessageServer(NetBuffer &buffer, unsigned int flags)
{
	if (serverDestinationId_ == UINT_MAX)
	{
		// Only reachable by sending before the ConnectMessage that names the
		// server has been processed, which the engine never does - it sends
		// its first message from clientConnected().
		Logger::log("NetBridge: message for the server before one was connected");
		return;
	}
	sendMessageDest(buffer, serverDestinationId_, flags);
}

void NetBridge::sendMessageDest(NetBuffer &buffer, unsigned int destination, unsigned int flags)
{
	sendMessageTypeDest(buffer, destination, flags, NetMessage::BufferMessage);
}

void NetBridge::disconnectAllClients()
{
	NetBuffer buffer;
	sendMessageTypeDest(buffer, 0, 0, NetMessage::DisconnectAllMessage);
}

void NetBridge::disconnectClient(unsigned int destination)
{
	NetBuffer buffer;
	disconnectClient(buffer, destination);
}

void NetBridge::disconnectClient(NetBuffer &buffer, unsigned int destination)
{
	sendMessageTypeDest(buffer, destination, 0, NetMessage::DisconnectMessage);
}

void NetBridge::sendMessageTypeDest(
	NetBuffer &buffer,
	unsigned int destination,
	unsigned int flags,
	NetMessage::MessageType type
)
{
	NetMessage *message = NetMessagePool::instance()->getFromPool(type, destination, 0, flags);
	message->getBuffer().allocate(buffer.getBufferUsed());
	if (buffer.getBufferUsed() > 0)
	{
		memcpy(message->getBuffer().getBuffer(), buffer.getBuffer(), buffer.getBufferUsed());
	}
	message->getBuffer().setBufferUsed(buffer.getBufferUsed());

	outgoingMessageHandler_.addMessage(message);
}

void NetBridge::processMessage(NetMessage &message)
{
	// Runs on the send thread: this is the outgoing queue being drained.
	if (message.getMessageType() == NetMessage::DisconnectAllMessage)
	{
		std::set<unsigned int> peers;
		pthread_mutex_lock(&peersMutex_);
		peers = peers_;
		pthread_mutex_unlock(&peersMutex_);

		for (std::set<unsigned int>::iterator itor = peers.begin(); itor != peers.end(); ++itor)
		{
			if (transport_) transport_->disconnect(*itor);
			peerGone(*itor, NetMessage::KickDisconnect);
		}

		// Same as NetServerTCP3: disconnecting everyone is also how this
		// interface is shut down, and stop() is waiting on exactly this.
		stopped_ = true;
		return;
	}

	if (!isKnownPeer(message.getDestinationId())) return;

	if (message.getMessageType() == NetMessage::DisconnectMessage)
	{
		if (transport_) transport_->disconnect(message.getDestinationId());
		peerGone(message.getDestinationId(), NetMessage::KickDisconnect);
		return;
	}

	NetBuffer &buffer = message.getBuffer();
	if (!buffer.getBuffer() || buffer.getBufferUsed() == 0) return;

	bool sent = transport_ && transport_->send(
		message.getDestinationId(),
		(const unsigned char *) buffer.getBuffer(),
		buffer.getBufferUsed());
	if (!sent)
	{
		Logger::log(S3D::formatStringBuffer(
			"NetBridge: send to destination %u failed, dropping it",
			message.getDestinationId()));
		peerGone(message.getDestinationId(), NetMessage::UserDisconnect);
		return;
	}
	NetInterface::getBytesOut() += buffer.getBufferUsed();
}

void NetBridge::onPeerConnected(unsigned int peerId)
{
	if (peerId == 0 || peerId == UINT_MAX)
	{
		// The id becomes a Scorched3D destination id verbatim, and both of
		// these mean something else there.
		Logger::log(S3D::formatStringBuffer("NetBridge: transport offered unusable peer id %u", peerId));
		return;
	}

	pthread_mutex_lock(&peersMutex_);
	bool isNew = peers_.insert(peerId).second;
	pthread_mutex_unlock(&peersMutex_);
	if (!isNew) return;

	// A client has exactly one peer and it is the host. Claimed by whoever
	// connects first rather than by a reserved id, because the transport
	// owns the id space (see BridgeTransport).
	if (!hosting_ && serverDestinationId_ == UINT_MAX) serverDestinationId_ = peerId;

	NetInterface::getConnects()++;
	NetMessage *message = NetMessagePool::instance()->
		getFromPool(NetMessage::ConnectMessage, peerId, 0);
	incomingMessageHandler_.addMessage(message);
}

void NetBridge::onPayload(unsigned int peerId, const unsigned char *bytes, unsigned int length)
{
	if (length == 0) return;
	if (!isKnownPeer(peerId)) return;

	NetMessage *message = NetMessagePool::instance()->getFromPool(
		NetMessage::BufferMessage,
		peerId,
		0,           // No IP address: see this class's header.
		0,           // Flags are a send-side request and never travel.
		SDL_GetTicks());
	message->getBuffer().allocate(length);
	memcpy(message->getBuffer().getBuffer(), bytes, length);
	message->getBuffer().setBufferUsed(length);

	NetInterface::getBytesIn() += length;
	incomingMessageHandler_.addMessage(message);
}

void NetBridge::onPeerDisconnected(unsigned int peerId)
{
	peerGone(peerId, NetMessage::UserDisconnect);
}

void NetBridge::onTransportFailed(const char *reason)
{
	Logger::log(S3D::formatStringBuffer("NetBridge: transport failed: %s", reason));

	std::set<unsigned int> peers;
	pthread_mutex_lock(&peersMutex_);
	peers = peers_;
	pthread_mutex_unlock(&peersMutex_);

	if (peers.empty())
	{
		// Nothing ever connected - a client that could not reach the host at
		// all. The engine still has to hear about it, or the join screen
		// waits for a connection that is not coming, so synthesise the
		// disconnect it would have got had the link ever been up. The id is
		// not read by ClientContext::clientDisconnected and only has to be a
		// destination that is not in use.
		NetMessage *message = NetMessagePool::instance()->
			getFromPool(NetMessage::DisconnectMessage, 1, 0, NetMessage::UnknownDisconnect);
		incomingMessageHandler_.addMessage(message);
		return;
	}

	for (std::set<unsigned int>::iterator itor = peers.begin(); itor != peers.end(); ++itor)
	{
		peerGone(*itor, NetMessage::UnknownDisconnect);
	}
}

void NetBridge::peerGone(unsigned int peerId, NetMessage::DisconnectFlags flags)
{
	pthread_mutex_lock(&peersMutex_);
	bool known = (peers_.erase(peerId) > 0);
	pthread_mutex_unlock(&peersMutex_);
	// Both the transport noticing a dropped link and this side kicking the
	// same client arrive here; the engine must be told once.
	if (!known) return;

	if (peerId == serverDestinationId_)
	{
		// The host has gone, so there is no game left to run - the same
		// conclusion NetServerTCP3 draws when its server destination dies.
		serverDestinationId_ = UINT_MAX;
		stopped_ = true;
	}

	NetMessage *message = NetMessagePool::instance()->
		getFromPool(NetMessage::DisconnectMessage, peerId, 0, (unsigned int) flags);
	incomingMessageHandler_.addMessage(message);
}

bool NetBridge::isKnownPeer(unsigned int peerId)
{
	pthread_mutex_lock(&peersMutex_);
	bool known = (peers_.find(peerId) != peers_.end());
	pthread_mutex_unlock(&peersMutex_);
	return known;
}
