#include <ClientContext.hpp>
#include <PlayerProfile.h>

#include <cstring>
#include <net/NetServerTCP3.hpp>
#include <net/NetBuffer.hpp>
#include <net/NetMessage.hpp>
#include <target/TargetSpace.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <common/Defines.hpp>
#include <common/Logger.hpp>
#include <common/OptionsScorched.hpp>
#include <common/OptionsTransient.hpp>
#include <coms/ComsConnectMessage.hpp>
#include <coms/ComsConnectAuthMessage.hpp>
#include <coms/ComsConnectAcceptMessage.hpp>
#include <coms/ComsConnectRejectMessage.hpp>
#include <coms/ComsNetStatMessage.hpp>
#include <coms/ComsTankChangeMessage.hpp>
#include <tank/Tank.hpp>
#include <tanket/TanketType.hpp>
#include <tankai/TankAIStrings.hpp>
#include <target/TargetContainer.hpp>
#include <coms/ComsHaveModFilesMessage.hpp>
#include <coms/ComsInitializeModMessage.hpp>
#include <coms/ComsLoadLevelMessage.hpp>
#include <coms/ComsLevelLoadedMessage.hpp>
#include <coms/ComsChannelMessage.hpp>
#include <coms/ComsChannelTextMessage.hpp>
#include <lang/LangString.hpp>
#include <ChatStore.h>
#include <coms/ComsSimulateMessage.hpp>
#include <coms/ComsSimulateResultMessage.hpp>
#include <engine/ModFiles.hpp>
#include <weapons/AccessoryStore.hpp>
#include <tanket/TanketTypes.hpp>
#include <tank/TankModelStore.hpp>
#include <landscapedef/LandscapeDefinitions.hpp>

TargetSpace *ClientContext::targetSpace_ = new TargetSpace();

ClientContext::ClientContext() :
	ScorchedContext("Client"),
	clientSync_(new ClientSync()),
	state_(sIdle)
{
	targetSpace_->setContext(this);
	clientSync_->setScorchedContext(this);

	// This context is a second, independent ScorchedContext in the same
	// process (see host-tests/main.cpp's testClientJoin) or, on a real
	// device, in a wholly separate process on a different machine - either
	// way it needs its own copy of exactly the same static game data
	// ScorchedServer::startServerInternal() loads (accessories, tanket
	// types, tank models, landscape definitions - all name/id-referenced
	// by the Tank/Accessory state the host will send us in
	// ComsLoadLevelMessage), plus the mod-file listing needed to build a
	// ComsHaveModFilesMessage. Found the hard way: omitting
	// readLandscapeDefinitions() segfaulted inside OptionsScorched::
	// updateLevelOptions() (called from ComsLoadLevelMessage::loadState())
	// on a null LandscapeTex/LandscapeDefn lookup.
	S3D::setDataFileMod(getOptionsGame().getMod());
	getModFiles().loadModFiles(getOptionsGame().getMod(), false, nullptr);
	getAccessoryStore().parseFile(*this, nullptr);
	getTanketTypes().loadTanketTypes(*this);
	getTankModels().loadTankMeshes(*this, 2, nullptr);
	getOptionsTransient().reset();
	getLandscapes().readLandscapeDefinitions();
}

ClientContext::~ClientContext()
{
	targetSpace_->clear();
	delete clientSync_;
}

Simulator &ClientContext::getSimulator()
{
	return *clientSync_;
}

bool ClientContext::connectToServer(const char *host, int port)
{
	NetServerTCP3 *netInterface = new NetServerTCP3();
	setNetInterface(netInterface);
	netInterface->setMessageHandler(&getComsMessageHandler());
	getComsMessageHandler().setConnectionHandler(this);

	getComsMessageHandler().addHandler(ComsConnectAuthMessage::ComsConnectAuthMessageType, this);
	getComsMessageHandler().addHandler(ComsConnectAcceptMessage::ComsConnectAcceptMessageType, this);
	getComsMessageHandler().addHandler(ComsConnectRejectMessage::ComsConnectRejectMessageType, this);
	getComsMessageHandler().addHandler(ComsInitializeModMessage::ComsInitializeModMessageType, this);
	getComsMessageHandler().addHandler(ComsLoadLevelMessage::ComsLoadLevelMessageType, this);
	getComsMessageHandler().addHandler(ComsSimulateMessage::ComsSimulateMessageType, this);
	getComsMessageHandler().addHandler(ComsNetStatMessage::ComsNetStatMessageType, this);
	// Chat. Without this registration the host's channel text arrives and is
	// discarded by ComsMessageHandler as an unhandled type, well before
	// processMessage() above ever sees it.
	getComsMessageHandler().addHandler(ComsChannelTextMessage::ComsChannelTextMessageType, this);
	// ...and the registration's own reply. Registering for channels makes
	// the host call refreshDestination(), which sends a ComsChannelMessage
	// back; without a handler for it ComsMessageHandler treats it as an
	// unknown type and errors the connection, and the client then silently
	// stops receiving everything else - which is exactly what happened, and
	// what the host-tests' "client sees every tank" check caught.
	getComsMessageHandler().addHandler(ComsChannelMessage::ComsChannelMessageType, this);

	if (!netInterface->connect(host, port))
	{
		fail("Failed to open a socket to the host");
		return false;
	}

	state_ = sConnecting;
	return true;
}

void ClientContext::tick()
{
	if (state_ == sFailed || state_ == sIdle) return;
	if (!getNetInterfaceValid()) return;

	getNetInterface().processMessages();

	if (state_ == sJoined)
	{
		clientSync_->simulate();
		sendTankChangeIfNeeded();
	}
}

void ClientContext::sendTankChangeIfNeeded()
{
	if (tankChangeSent_) return;

	std::map<unsigned int, Tank *> &tanks = getTargetContainer().getTanks();
	std::map<unsigned int, Tank *>::iterator itor;
	for (itor = tanks.begin(); itor != tanks.end(); ++itor)
	{
		Tank *tank = itor->second;
		if (tank->getDestinationId() != myDestinationId_) continue;

		TankModel *tankModel = getTankModels().getRandomModel(
			tank->getTeam(), false, tank->getTanketType()->getName());
		// playerType must be "Human" (not "" - see engine_jni.cpp's
		// promoteHumanToPlaying() for the full story): the non-Human branch
		// of TankChangeSimAction::invokeAction() unconditionally overwrites
		// the tank's destinationId with the message's own, so getting this
		// wrong would reassign our own tank to look like a bot slot on the
		// host - destinationId must be this tank's real one for the same
		// reason, not a bot's literal 0.
		ComsTankChangeMessage tankChangeMessage(
			tank->getPlayerId(), tank->getTargetName(), tank->getColor(),
			tank->getTanketType()->getName(), tankModel->getName(),
			tank->getDestinationId(), tank->getTeam(), "Human", false);
		sendGameMessage(tankChangeMessage);
		tankChangeSent_ = true;
		break;
	}
}

void ClientContext::fail(const std::string &reason)
{
	// First failure wins - a real rejection (see the ComsConnectRejectMessage
	// handler above) is immediately followed by the host closing the socket,
	// which would otherwise overwrite a specific reason with
	// clientDisconnected()'s generic "Disconnected by host".
	if (state_ == sFailed) return;

	state_ = sFailed;
	failureReason_ = reason;
	Logger::log(S3D::formatStringBuffer("ClientContext: %s", reason.c_str()));
}

void ClientContext::clientConnected(NetMessage &message)
{
	// The transport (TCP accept/connect) succeeded - now start the real
	// application-level handshake (see ServerConnectHandler.cpp for the
	// server side of this exact exchange).
	ComsConnectMessage connectMessage;
	connectMessage.setVersion(S3D::ScorchedVersion.c_str());
	connectMessage.setProtocolVersion(S3D::ScorchedProtocolVersion.c_str());
	sendToServer(connectMessage);

	state_ = sWaitingAuthChallenge;
}

void ClientContext::clientDisconnected(NetMessage &message)
{
	fail("Disconnected by host");
}

void ClientContext::clientError(NetMessage &message, const std::string &errorString)
{
	fail(errorString);
}

bool ClientContext::processMessage(NetMessage &message, const char *messageType, NetBufferReader &reader)
{
	if (0 == strcmp(messageType, ComsConnectRejectMessage::ComsConnectRejectMessageType.getName().c_str()))
	{
		// The host can reject us at either handshake stage - right after
		// ComsConnectMessage (see ServerConnectHandler.cpp, e.g. a protocol
		// version mismatch) or after ComsConnectAuthMessage (see
		// ServerConnectAuthHandler.cpp, e.g. "server full" - found the hard
		// way testing a real two-emulator join with no free player slots).
		// Without this handler, the rejection still disconnects us, but as
		// an opaque "Failed to find RECV message type handler" instead of
		// the host's actual reason.
		ComsConnectRejectMessage rejectMessage;
		if (rejectMessage.readMessage(reader))
		{
			fail(rejectMessage.getText());
		}
		else
		{
			fail("Rejected by host");
		}
		return true;
	}

	if (0 == strcmp(messageType, ComsConnectAuthMessage::ComsConnectAuthMessageType.getName().c_str()))
	{
		if (state_ != sWaitingAuthChallenge) return true;

		// This is the server's auth challenge (see ServerConnectHandler.cpp
		// sending it in reply to our ComsConnectMessage) - reply with our
		// own filled-in ComsConnectAuthMessage naming this player/session
		// (see ServerConnectAuthHandler::processMessageInternal, already
		// investigated for engine_jni.cpp's addHumanTank()).
		ComsConnectAuthMessage reply;
		// The player's own name, not a constant: the host names the joining
		// tank from this, so "ScorchDroid" is what everyone in the game used
		// to be called.
		reply.setUserName(ScorchDroidProfile::name().c_str());
		reply.setPassword("");
		reply.setUniqueId("");
		reply.setSUI("");
		reply.setHostDesc("ScorchDroid");
		reply.setCompatabilityVer(0);
		reply.setNoPlayers(1);
		sendToServer(reply);

		state_ = sWaitingConnectAccept;
		return true;
	}

	if (0 == strcmp(messageType, ComsConnectAcceptMessage::ComsConnectAcceptMessageType.getName().c_str()))
	{
		if (state_ != sWaitingConnectAccept) return true;

		ComsConnectAcceptMessage acceptMessage;
		if (!acceptMessage.readMessage(reader)) return false;
		myDestinationId_ = acceptMessage.getDestinationId();
		Logger::log(S3D::formatStringBuffer("ClientContext: connected to \"%s\"", acceptMessage.getServerName()));

		// Mod-file handshake (see ServerHaveModFilesHandler.cpp): built
		// from the same getModFiles() data ScorchedServer::
		// startServerInternal() already computes under S3D_SERVER - since
		// we're joining a host on the same pinned commit/default mod, the
		// server-side diff should always come back empty, skipping the
		// file-transfer subsystem entirely. Every ServerDestination starts
		// in the sDownloadingMod state (see ServerDestination.cpp) - once
		// ServerFileServer::simulate() sees nothing left to send us (true
		// immediately here), *it* sends back a ComsInitializeModMessage of
		// its own; that's handled below, not sent proactively here (an
		// earlier version of this code sent one right away too, which
		// happened to also work but left this client with no handler
		// registered for the server's own copy, crashing once it arrived).
		ComsHaveModFilesMessage haveFilesMessage;
		std::map<std::string, ModFileEntry *> &files = getModFiles().getFiles();
		std::map<std::string, ModFileEntry *>::iterator itor;
		for (itor = files.begin(); itor != files.end(); ++itor)
		{
			ModFileEntry *entry = itor->second;
			haveFilesMessage.getFiles().push_back(ModIdentifierEntry(
				true, entry->getFileName(), entry->getUncompressedSize(), entry->getUncompressedCrc()));
		}
		sendToServer(haveFilesMessage);

		state_ = sWaitingLoadLevel;
		return true;
	}

	if (0 == strcmp(messageType, ComsInitializeModMessage::ComsInitializeModMessageType.getName().c_str()))
	{
		// The server's signal (via ServerFileServer::simulate(), see above)
		// that it's done serving us mod files - echo the same message type
		// back as an acknowledgment, which is what ServerInitializeModHandler
		// is actually waiting for before calling ServerLoadLevel::
		// destinationLoadLevel() to send us ComsLoadLevelMessage.
		if (state_ != sWaitingLoadLevel) return true;

		// Matches real upstream's own client - see
		// ClientInitializeModHandler.cpp, which loads this at exactly this
		// point in the real handshake too. Without it, attackLines_/
		// deathLines_/etc. stay at their default-constructed empty state
		// for this context's entire lifetime, and the first tank to fire
		// anywhere in a replayed ComsSimulateMessage (see
		// PlayMovesSimAction::tankFired() -> TankAIStrings::getAttackLine())
		// does `% attackLines_.getLines().size()` against that empty list -
		// a modulo-by-zero that reliably segfaults. Found the hard way on a
		// real device once a shot actually fired for the first time.
		getTankAIStrings().load();

		ComsInitializeModMessage ackMessage;
		sendToServer(ackMessage);
		return true;
	}

	if (0 == strcmp(messageType, ComsLoadLevelMessage::ComsLoadLevelMessageType.getName().c_str()))
	{
		// Also accepted once already sJoined, not just sWaitingLoadLevel:
		// ServerStateNewGame::newGame() (see that file) sends a fresh
		// ComsLoadLevelMessage to every destination at the start of *every*
		// round, not just the first one this connection ever sees - a
		// dedicated server that stays up across multiple matches will send
		// this again and again for as long as we stay connected. Rejecting
		// it once already joined (the original version of this guard) left
		// our destination server-side stuck at TankState::sLoading forever
		// after the very first round ended, since nothing ever answers
		// with the ComsLevelLoadedMessage that would let
		// ServerLoadLevel::setLoaded() promote it again - found the hard
		// way as "the shot fires but nothing ever happens" once a real
		// play session outlasted a single match.
		if (state_ != sWaitingLoadLevel && state_ != sJoined) return true;

		ComsLoadLevelMessage loadLevelMessage;
		if (!loadLevelMessage.readMessage(reader)) return false;

		if (!loadLevelMessage.loadState(*this)) return false;
		if (!loadLevelMessage.loadTanks(*this)) return false;

		Logger::log(S3D::formatStringBuffer("ClientContext: loading landscape %s",
			loadLevelMessage.getLandscapeDefinition().getName()));
		getLandscapeMaps().generateMaps(*this, loadLevelMessage.getLandscapeDefinition(), nullptr);

		clientSync_->newLevel();

		std::list<ComsSimulateMessage *> simulateMessages;
		if (!loadLevelMessage.getSimulations(simulateMessages)) return false;
		std::list<ComsSimulateMessage *>::iterator simItor;
		for (simItor = simulateMessages.begin(); simItor != simulateMessages.end(); ++simItor)
		{
			clientSync_->addComsSimulateMessage(**simItor);
			delete *simItor;
		}

		// Fast-forward to the host's current time, same one-fixed-second-
		// at-a-time approach as the real ClientLoadLevelHandler.cpp, so any
		// events already queued in the replayed sim messages above apply
		// in order rather than all at once.
		fixed actualTime = loadLevelMessage.getActualTime();
		fixed actualTimeCurrent = 0;
		for (;;)
		{
			actualTimeCurrent += fixed(1);
			if (actualTimeCurrent >= actualTime)
			{
				clientSync_->setSimulationTime(actualTime);
				break;
			}
			clientSync_->setSimulationTime(actualTimeCurrent);
		}

		ComsLevelLoadedMessage levelLoadedMessage;
		sendToServer(levelLoadedMessage);

		state_ = sJoined;
		subscribeToChatChannels();
		return true;
	}

	if (0 == strcmp(messageType, ComsChannelMessage::ComsChannelMessageType.getName().c_str()))
	{
		// The host confirming which channels this destination now has. There
		// is nothing to do with it here - the channel list this port speaks
		// on is fixed - but it must be read and accepted, not left unhandled.
		ComsChannelMessage channelMessage;
		channelMessage.readMessage(reader);
		return true;
	}

	if (0 == strcmp(messageType, ComsChannelTextMessage::ComsChannelTextMessageType.getName().c_str()))
	{
		// Chat, and the game's own running commentary. The host only sends
		// a channel's text to destinations that asked for it, which is what
		// subscribeToChatChannels() below does on joining - without that
		// registration this handler is simply never reached, which is why a
		// joined client saw no chat at all before.
		ComsChannelTextMessage textMessage;
		if (!textMessage.readMessage(reader)) return true;

		ChannelText &text = textMessage.getChannelText();
		ScorchDroidChat::Line line;
		line.channel = text.getChannel();
		line.text = LangStringUtil::convertFromLang(text.getMessage());

		// The message carries the speaker's player id, not their name.
		Tank *tank = getTargetContainer().getTankById(text.getSrcPlayerId());
		if (tank) line.who = tank->getCStrName();

		ScorchDroidChat::push(line);
		return true;
	}

	if (0 == strcmp(messageType, ComsNetStatMessage::ComsNetStatMessageType.getName().c_str()))
	{
		// Periodic ping/step-size info the host sends every couple of
		// seconds once this destination is loaded (see
		// ServerSimulator::processMessage, which only sends it in response
		// to the ComsSimulateResultMessage we return below - so this stops
		// arriving if we ever stop answering).
		//
		// The round-trip time is not just a display statistic: it is how
		// syncToServerTime() estimates how stale a just-arrived message
		// already is. Registering *a* handler at all also matters
		// independently - ComsMessageHandler::processMessage() errors out
		// and drops the connection on an unhandled type, found the hard way
		// testing a real two-emulator join.
		ComsNetStatMessage netStatMessage;
		if (!netStatMessage.readMessage(reader)) return false;
		clientSync_->setNetStat(
			netStatMessage.getRoundTripTime(), netStatMessage.getSendStepSize());
		return true;
	}

	if (0 == strcmp(messageType, ComsSimulateMessage::ComsSimulateMessageType.getName().c_str()))
	{
		if (state_ != sJoined) return true;

		ComsSimulateMessage simulateMessage;
		if (!simulateMessage.readMessage(reader)) return false;
		clientSync_->addComsSimulateMessage(simulateMessage);

		// Send back a ping response, same as the real ClientSimulator. This
		// is what makes the host measure our round-trip time and send back
		// the ComsNetStatMessage handled above, so it is load-bearing for
		// the clock correction below, not just politeness.
		ComsSimulateResultMessage resultMessage(simulateMessage.getServerTime());
		sendToServer(resultMessage);

		// Live message, so correct our clock towards the host's. Deliberately
		// not done for the buffered messages replayed during level load
		// (see the ComsLoadLevelMessage handler above) - those carry old
		// timestamps and would drag the clock backwards; upstream skips them
		// for the same reason via its loadingLevel_ flag.
		clientSync_->syncToServerTime(simulateMessage.getActualTime());
		return true;
	}

	return true;
}

bool ClientContext::sendGameMessage(ComsMessage &message)
{
	if (state_ != sJoined) return false;
	sendToServer(message);
	return true;
}

void ClientContext::sendToServer(ComsMessage &message, unsigned int flags)
{
	// Replicates ComsMessageSender::sendToServer() (excluded from this
	// build entirely by #ifndef S3D_SERVER, since it hardcodes
	// ScorchedClient::instance() - see ComsMessageSender.cpp) against this
	// context instead. Deliberately a local NetBuffer per call rather than
	// upstream's single shared static one - this class isn't on any hot
	// path where that matters, and a local buffer is simpler to reason
	// about here.
	NetBuffer buffer;
	message.writeTypeMessage(buffer);
	message.writeMessage(buffer);
	buffer.addToBuffer(false);  // Not compressed.
	getNetInterface().sendMessageServer(buffer, flags);
}

void ClientContext::subscribeToChatChannels()
{
	// The host's ServerChannelManager only forwards a channel's text to
	// destinations that have registered for it (see its registerClient and
	// the hasChannel() test in sendText), so without this a joined client is
	// silently deaf to all chat. Upstream's ClientChannelManager does the
	// same thing through its receiver-registration machinery; this is the
	// same message with a fixed local id, since this port has exactly one
	// local player per destination.
	//
	// The list is upstream's own player-visible set. "spam", "admin" and
	// "whisper" are deliberately left out: the first is noise, and the other
	// two need authentication this port does not implement.
	static const char *kChannels[] = { "general", "team", "info", "announce", "combat", "banner" };

	ComsChannelMessage message(ComsChannelMessage::eRegisterRequest, kChatLocalId);
	for (const char *channel : kChannels)
	{
		message.getChannels().push_back(ChannelDefinition(channel, 0));
	}
	sendToServer(message);
}
