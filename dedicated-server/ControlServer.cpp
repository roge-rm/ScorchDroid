#include "ControlServer.hpp"
#include "Json.hpp"

#include <GameSetup.h>
#include <common/ChannelText.hpp>
#include <common/Defines.hpp>
#include <common/DefinesScorched.hpp>
#include <common/OptionEntry.hpp>
#include <common/OptionsGame.hpp>
#include <common/OptionsScorched.hpp>
#include <common/OptionsTransient.hpp>
#include <engine/ScorchedContext.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapedef/LandscapeDefinitionCache.hpp>
#include <server/ScorchedServer.hpp>
#include <server/ServerAdminCommon.hpp>
#include <server/ServerAdminSessions.hpp>
#include <server/ServerChannelManager.hpp>
#include <server/ServerState.hpp>
#include <tank/Tank.hpp>
#include <tank/TankScore.hpp>
#include <tank/TankState.hpp>
#include <target/TargetContainer.hpp>
#include <target/TargetLife.hpp>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
	// The reply line has to stay one line, and a request has to survive
	// arriving in pieces, so both are framed on '\n' and every field is
	// tab-separated. Anything a caller sends with a tab or a newline in it
	// arrives escaped; text is the only field where that comes up (chat and
	// a message of the day) and the web side escapes it the same way.
	std::string unescape(const std::string &field)
	{
		std::string out;
		out.reserve(field.size());
		for (std::size_t i = 0; i < field.size(); i++)
		{
			if (field[i] != '\\' || i + 1 >= field.size())
			{
				out += field[i];
				continue;
			}
			switch (field[++i])
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case '\\': out += '\\'; break;
			default: out += field[i]; break;
			}
		}
		return out;
	}

	std::vector<std::string> split(const std::string &line)
	{
		std::vector<std::string> fields;
		std::size_t start = 0;
		while (true)
		{
			const std::size_t tab = line.find('\t', start);
			if (tab == std::string::npos)
			{
				fields.push_back(unescape(line.substr(start)));
				break;
			}
			fields.push_back(unescape(line.substr(start, tab - start)));
			start = tab + 1;
		}
		return fields;
	}

	const char *stateName(int state)
	{
		switch (state)
		{
		case ServerState::ServerStartupState: return "Starting up";
		case ServerState::ServerWaitingForPlayersState: return "Waiting for players";
		case ServerState::ServerMatchCountDownState: return "Match countdown";
		case ServerState::ServerNewLevelState: return "Loading level";
		case ServerState::ServerBuyingState: return "Buying";
		case ServerState::ServerTankNewGameState: return "Starting round";
		case ServerState::ServerPlayingState: return "Playing";
		case ServerState::ServerFinishWaitState: return "Round over";
		case ServerState::ServerScoreState: return "Score";
		default: return "Unknown";
		}
	}

	const char *kindName(ScorchDroidSetup::Kind kind)
	{
		switch (kind)
		{
		case ScorchDroidSetup::eBoundedInt: return "boundedInt";
		case ScorchDroidSetup::eInt: return "int";
		case ScorchDroidSetup::eBool: return "bool";
		case ScorchDroidSetup::eEnum: return "enum";
		case ScorchDroidSetup::eString: return "string";
		case ScorchDroidSetup::eText: return "text";
		case ScorchDroidSetup::eStringEnum: return "stringEnum";
		case ScorchDroidSetup::eFloat: return "float";
		default: return "int";
		}
	}

	std::string arg(const std::vector<std::string> &args, std::size_t index)
	{
		return index < args.size() ? args[index] : std::string();
	}

	unsigned int uintArg(const std::vector<std::string> &args, std::size_t index)
	{
		const std::string value = arg(args, index);
		return value.empty() ? 0u : (unsigned int) strtoul(value.c_str(), nullptr, 10);
	}

	unsigned long long ullArg(const std::vector<std::string> &args, std::size_t index)
	{
		const std::string value = arg(args, index);
		return value.empty() ? 0ull : strtoull(value.c_str(), nullptr, 10);
	}

	bool serverUp() { return ScorchedServer::serverStarted() && ScorchedServer::instance(); }
}  // namespace

ControlServer::ControlServer(const Config &config, LogRing &log, Host host)
	: config_(config), log_(log), host_(std::move(host))
{
	startedAt_ = (unsigned long long) time(nullptr);
}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(std::string &error)
{
	if (config_.socketPath.empty())
	{
		error = "no control socket path";
		return false;
	}
	if (config_.socketPath.size() >= sizeof(((struct sockaddr_un *) nullptr)->sun_path))
	{
		error = "control socket path is too long for sockaddr_un";
		return false;
	}

	// A socket file outlives the process that made it, so in a container
	// every restart after the first would otherwise fail to bind. Removing
	// it is safe because only one server owns a given path.
	unlink(config_.socketPath.c_str());

	listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listenFd_ < 0)
	{
		error = std::string("socket(): ") + strerror(errno);
		return false;
	}

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, config_.socketPath.c_str(), sizeof(address.sun_path) - 1);

	if (bind(listenFd_, (struct sockaddr *) &address, sizeof(address)) != 0)
	{
		error = std::string("bind(") + config_.socketPath + "): " + strerror(errno);
		close(listenFd_);
		listenFd_ = -1;
		return false;
	}

	// The socket's permissions are the only access control there is, and
	// they are enough: the web container reaches it through a shared volume
	// as the same user, and nothing else can reach it at all. 0660 so a
	// group-mounted volume works without making it world-writable.
	chmod(config_.socketPath.c_str(), 0660);

	if (listen(listenFd_, 8) != 0)
	{
		error = std::string("listen(): ") + strerror(errno);
		close(listenFd_);
		listenFd_ = -1;
		return false;
	}

	fcntl(listenFd_, F_SETFL, fcntl(listenFd_, F_GETFL, 0) | O_NONBLOCK);
	return true;
}

void ControlServer::stop()
{
	for (Client &client : clients_)
	{
		if (client.fd >= 0) close(client.fd);
	}
	clients_.clear();
	if (listenFd_ >= 0)
	{
		close(listenFd_);
		listenFd_ = -1;
		unlink(config_.socketPath.c_str());
	}
}

void ControlServer::poll()
{
	if (listenFd_ < 0) return;

	while (true)
	{
		const int fd = accept(listenFd_, nullptr, nullptr);
		if (fd < 0) break;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		Client client;
		client.fd = fd;
		clients_.push_back(client);
	}

	for (std::size_t i = 0; i < clients_.size();)
	{
		serve(clients_[i]);
		if (clients_[i].fd < 0) clients_.erase(clients_.begin() + (long) i);
		else i++;
	}
}

void ControlServer::closeClient(Client &client)
{
	if (client.fd >= 0) close(client.fd);
	client.fd = -1;
}

void ControlServer::serve(Client &client)
{
	char buffer[4096];
	while (true)
	{
		const ssize_t read = recv(client.fd, buffer, sizeof(buffer), 0);
		if (read > 0)
		{
			client.in.append(buffer, (std::size_t) read);
			continue;
		}
		if (read == 0)
		{
			closeClient(client);
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) break;
		closeClient(client);
		return;
	}

	std::size_t newline;
	while ((newline = client.in.find('\n')) != std::string::npos)
	{
		std::string line = client.in.substr(0, newline);
		client.in.erase(0, newline + 1);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		client.out += handle(line);
		client.out += '\n';
	}

	// options requests run to tens of kilobytes, well past a socket
	// buffer, so what will not go now is kept and finished on a later
	// tick rather than blocking the engine thread on a slow reader.
	while (!client.out.empty())
	{
		const ssize_t written = send(client.fd, client.out.data(), client.out.size(), MSG_NOSIGNAL);
		if (written > 0)
		{
			client.out.erase(0, (std::size_t) written);
			continue;
		}
		if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
		closeClient(client);
		return;
	}
}

std::string ControlServer::handle(const std::string &line)
{
	const std::vector<std::string> fields = split(line);
	if (fields.empty() || fields[0].empty()) return Json::error("empty command");

	const std::string command = fields[0];
	const std::vector<std::string> args(fields.begin() + 1, fields.end());

	if (command == "ping")
	{
		return Json::Object()
			.set("ok", true)
			.set("gameVersion", S3D::ScorchedVersion)
			.set("protocolVersion", S3D::ScorchedProtocolVersion)
			.text();
	}
	if (command == "status") return handleStatus(args);
	if (command == "log") return handleLog(args);
	if (command == "chat") return handleChat(args);
	if (command == "say") return handleSay(args);
	if (command == "admin") return handleAdmin(args);
	if (command == "options" || command == "options.set" || command == "options.save" ||
		command == "options.reset" || command == "apply")
	{
		return handleOptions(std::vector<std::string>(fields.begin(), fields.end()));
	}
	if (command == "mods" || command == "setmod" || command == "landscapes" ||
		command == "setlandscapes" || command == "bots" || command == "setbots" ||
		command == "presets" || command == "loadpreset")
	{
		return handleSetup(command, args);
	}
	if (command == "restart")
	{
		if (!host_.restart) return Json::error("restart is not available");
		const bool restarted = host_.restart();
		return Json::Object().set("ok", restarted).set("accepted", restarted).text();
	}
	if (command == "shutdown")
	{
		if (!host_.shutdown) return Json::error("shutdown is not available");
		host_.shutdown();
		return Json::Object().set("ok", true).text();
	}

	return Json::error("unknown command: " + command);
}

std::string ControlServer::handleStatus(const std::vector<std::string> &)
{
	Json::Object out;
	out.set("ok", true);
	// The live value when there is one, so a name changed and applied shows
	// up without waiting for the process to be restarted; the configured
	// one only while the server is down.
	out.set("serverName", serverUp()
		? ScorchedServer::instance()->getOptionsGame().getServerName()
		: config_.serverName.c_str());
	out.set("port", config_.port);
	out.set("configPath", config_.configPath);
	out.set("uptime", (long long) ((unsigned long long) time(nullptr) - startedAt_));
	out.set("logSeq", log_.lastSeq());
	out.set("gameVersion", S3D::ScorchedVersion);
	out.set("protocolVersion", S3D::ScorchedProtocolVersion);

	if (!serverUp())
	{
		out.set("running", false);
		out.raw("tanks", "[]");
		return out.text();
	}

	ScorchedServer *server = ScorchedServer::instance();
	OptionsScorched &options = server->getOptionsGame();

	out.set("running", true);
	out.set("state", (int) server->getServerState().getState());
	out.set("stateName", stateName((int) server->getServerState().getState()));
	out.set("round", server->getOptionsTransient().getCurrentRoundNo());
	out.set("rounds", options.getNoRounds());
	out.set("turn", server->getOptionsTransient().getCurrentTurnNo());
	out.set("turns", options.getNoTurns());
	out.set("mod", options.getMod());
	out.set("maxPlayers", options.getNoMaxPlayers());
	out.set("minPlayers", options.getNoMinPlayers());
	out.set("landscape", server->getLandscapeMaps().getDefinitions().getDefinition().getName());
	out.set("chatId", (long long) (server->getServerChannelManager().getLastMessages().empty()
		? 0
		: server->getServerChannelManager().getLastMessages().back().messageid));

	int humans = 0, bots = 0;
	Json::Array tanks;
	std::map<unsigned int, Tank *> &all = server->getTargetContainer().getTanks();
	for (std::map<unsigned int, Tank *>::iterator itor = all.begin(); itor != all.end(); ++itor)
	{
		Tank *tank = itor->second;
		const bool bot = tank->getTankAI() != nullptr;
		bot ? bots++ : humans++;
		tanks.raw(Json::Object()
			.set("playerId", tank->getPlayerId())
			.set("destinationId", tank->getDestinationId())
			.set("name", tank->getCStrName())
			.set("bot", bot)
			.set("team", (int) tank->getTeam())
			.set("state", tank->getState().getSmallStateString())
			.set("playing", tank->getState().getTankPlaying())
			.set("lives", tank->getState().getLives())
			.set("life", tank->getLife().getLife().asInt())
			.set("score", tank->getScore().getScore())
			.set("kills", tank->getScore().getKills())
			.set("wins", tank->getScore().getWins())
			.set("money", tank->getScore().getMoney())
			.set("ping", tank->getScore().getPing())
			.text());
	}
	out.raw("tanks", tanks.text());
	out.set("humans", humans);
	out.set("bots", bots);
	return out.text();
}

std::string ControlServer::handleLog(const std::vector<std::string> &args)
{
	const unsigned long long after = ullArg(args, 0);
	const std::size_t limit = (std::size_t) ullArg(args, 1);

	Json::Array lines;
	for (const LogRing::Line &line : log_.since(after, limit))
	{
		lines.raw(Json::Object()
			.set("seq", line.seq)
			.set("time", line.time)
			.set("message", line.message)
			.text());
	}
	return Json::Object().set("ok", true).raw("lines", lines.text()).set("lastSeq", log_.lastSeq()).text();
}

std::string ControlServer::handleChat(const std::vector<std::string> &args)
{
	if (!serverUp()) return Json::error("server is not running");
	const unsigned long long after = ullArg(args, 0);

	Json::Array messages;
	unsigned int lastId = 0;
	// getLastMessages() is upstream's own ring of recent channel text, ids
	// included - the same thing its web admin showed. Nothing new is
	// recorded here to make chat visible.
	std::list<ServerChannelManager::MessageEntry> &entries =
		ScorchedServer::instance()->getServerChannelManager().getLastMessages();
	for (std::list<ServerChannelManager::MessageEntry>::iterator itor = entries.begin();
		itor != entries.end();
		++itor)
	{
		lastId = itor->messageid;
		if (itor->messageid <= after) continue;
		messages.raw(Json::Object().set("id", (long long) itor->messageid).set("text", itor->message).text());
	}
	return Json::Object().set("ok", true).raw("messages", messages.text()).set("lastId", (long long) lastId).text();
}

std::string ControlServer::handleSay(const std::vector<std::string> &args)
{
	if (!serverUp()) return Json::error("server is not running");
	const std::string channel = arg(args, 0).empty() ? std::string("general") : arg(args, 0);
	const std::string text = arg(args, 1);
	if (text.empty()) return Json::error("nothing to say");

	// ServerAdminCommon::adminSay in all but the name. That labels the line
	// with the credential's username, which upstream hardcodes as
	// "localaccount" for the local account - accurate, and a strange thing
	// for a player to be spoken to by. Players already know this server by
	// its name, so that is what the operator speaks as.
	//
	// Nothing upstream changes: setAdminPlayer is a field upstream leaves to
	// its caller, and the message goes out through the same
	// ServerChannelManager::sendText with the same filtering and logging.
	std::string who = ScorchedServer::instance()->getOptionsGame().getServerName();
	if (who.empty()) who = config_.serverName;
	if (who.empty()) who = "Server";

	ChannelText channelText(channel, LANG_STRING(text));
	channelText.setAdminPlayer(who);
	ScorchedServer::instance()->getServerChannelManager().sendText(channelText, true);
	return Json::Object().set("ok", true).set("accepted", true).text();
}

std::string ControlServer::handleAdmin(const std::vector<std::string> &args)
{
	if (!serverUp()) return Json::error("server is not running");

	const std::string verb = arg(args, 0);
	if (verb.empty()) return Json::error("no admin verb");

	ServerAdminSessions::Credential &credential =
		ScorchedServer::instance()->getServerAdminSessions().getLocalUserCredentials();
	const unsigned int player = uintArg(args, 1);
	const std::string reason = arg(args, 2);

	bool accepted = false;
	if (verb == "kick") accepted = ServerAdminCommon::kickPlayer(credential, player);
	else if (verb == "ban") accepted = ServerAdminCommon::banPlayer(credential, player, reason.c_str());
	else if (verb == "flag") accepted = ServerAdminCommon::flagPlayer(credential, player, reason.c_str());
	else if (verb == "mute") accepted = ServerAdminCommon::mutePlayer(credential, player, true);
	else if (verb == "unmute") accepted = ServerAdminCommon::mutePlayer(credential, player, false);
	else if (verb == "permmute") accepted = ServerAdminCommon::permMutePlayer(credential, player, reason.c_str());
	else if (verb == "unpermmute") accepted = ServerAdminCommon::unpermMutePlayer(credential, player);
	else if (verb == "poor") accepted = ServerAdminCommon::poorPlayer(credential, player);
	else if (verb == "kill") accepted = ServerAdminCommon::killPlayer(credential, player);
	// Upstream's own admin dialog slaps for ten, and engine_jni.cpp matches
	// it; this is the same command, so it does too.
	else if (verb == "slap") accepted = ServerAdminCommon::slapPlayer(credential, player, 10.0f);
	else if (verb == "changename")
	{
		accepted = ServerAdminCommon::changeNamePlayer(credential, player, LANG_STRING(reason));
	}
	else if (verb == "newgame") accepted = ServerAdminCommon::newGame(credential);
	else if (verb == "killall") accepted = ServerAdminCommon::killAll(credential);
	else if (verb == "stopwhenempty") accepted = ServerAdminCommon::stopServerWhenEmpty(credential);
	else if (verb == "setlogging")
	{
		accepted = ServerAdminCommon::setLogging(credential, arg(args, 1) == "1" || arg(args, 1) == "true");
	}
	else if (verb == "addbot")
	{
		// The same refusal engine_jni.cpp makes, for the same reason:
		// ServerStateEnoughPlayers::ballanceBots() counts humans and AIs
		// together against RemoveBotsAtPlayers and would auto-kick the new
		// bot within a tick or two, which looks like the command silently
		// doing nothing.
		if (ScorchedServer::instance()->getOptionsGame().getRemoveBotsAtPlayers() != 0)
		{
			return Json::Object()
				.set("ok", false)
				.set("error", "Bot balancing is on (RemoveBotsAtPlayers is not 0), so an added bot "
					"would be removed again straight away. Raise the player count instead.")
				.text();
		}
		const std::string type = arg(args, 1).empty() ? std::string("Random") : arg(args, 1);
		accepted = ServerAdminCommon::addPlayer(credential, type.c_str());
	}
	else return Json::error("unknown admin verb: " + verb);

	// ServerAdminCommon refuses a command against a player who has already
	// gone rather than failing loudly, so whether it was accepted is worth
	// reporting on its own - the same reason the Android UI toasts it.
	return Json::Object().set("ok", true).set("accepted", accepted).text();
}

std::string ControlServer::handleOptions(const std::vector<std::string> &fields)
{
	const std::string command = fields[0];

	if (command == "options")
	{
		Json::Array options;
		for (const ScorchDroidSetup::Option &option : ScorchDroidSetup::allOptions())
		{
			Json::Array choices;
			for (const ScorchDroidSetup::Choice &choice : option.choices)
			{
				choices.raw(Json::Object().set("label", choice.label).set("value", choice.value).text());
			}
			options.raw(Json::Object()
				.set("name", option.name)
				.set("group", option.group)
				.set("advanced", option.advanced)
				.set("description", option.description)
				.set("kind", kindName(option.kind))
				.set("value", option.value)
				.set("default", option.defaultValue)
				.set("min", option.minValue)
				.set("max", option.maxValue)
				.set("step", option.stepValue)
				.set("deprecated", option.deprecated)
				.set("restricted", option.restricted)
				.raw("choices", choices.text())
				.text());
		}
		return Json::Object().set("ok", true).raw("options", options.text()).text();
	}

	if (command == "options.set")
	{
		const std::string name = arg(fields, 1);
		const std::string value = arg(fields, 2);
		if (name.empty()) return Json::error("no option named");

		// Checked here, before upstream sees it, only for the enums.
		// OptionEntryEnum::setValueFromString() answers an unrecognised
		// string by quietly resetting the option to its default and
		// reporting success - fine for a config file written by hand, but a
		// web form that said "saved" while putting back the default would
		// be lying. Nothing about how the value is stored changes; this
		// only decides what the caller is told.
		for (const ScorchDroidSetup::Option &option : ScorchDroidSetup::allOptions())
		{
			if (option.name != name) continue;
			if (option.kind != ScorchDroidSetup::eEnum && option.kind != ScorchDroidSetup::eStringEnum) break;
			bool known = false;
			for (const ScorchDroidSetup::Choice &choice : option.choices)
			{
				// An integer enum takes either its label or its number,
				// which is what a config file written by upstream holds.
				if (choice.label == value || std::to_string(choice.value) == value) known = true;
			}
			if (!known)
			{
				return Json::Object()
					.set("ok", true)
					.set("accepted", false)
					.set("name", name)
					.set("error", "\"" + value + "\" is not one of " + name + "'s values")
					.text();
			}
			break;
		}

		// Everything else is upstream's own validation - a bounded int
		// refuses a value outside its range, a string enum one that is not
		// in its list.
		const bool accepted = ScorchDroidSetup::setAny(name, value);
		return Json::Object().set("ok", true).set("accepted", accepted).set("name", name).text();
	}

	if (command == "options.save")
	{
		const bool written = ScorchDroidSetup::writeSessionFile(config_.configPath);
		return Json::Object().set("ok", written).set("path", config_.configPath).text();
	}

	if (command == "options.reset")
	{
		ScorchDroidSetup::reset();
		return Json::Object().set("ok", true).text();
	}

	if (command == "apply")
	{
		if (!serverUp()) return Json::error("server is not running");
		ScorchedServer *server = ScorchedServer::instance();
		ScorchDroidSetup::applyAllTo(server->getOptionsGame());
		// Without this every value is silently undone at the next round:
		// OptionsScorched snapshots the main options during startServer()
		// and ServerStateNewGame::commitChanges() copies that snapshot back
		// over them. See GameSetup.h's note on applyTo().
		server->getOptionsGame().updateChangeSet();
		return Json::Object().set("ok", true).text();
	}

	return Json::error("unknown options command: " + command);
}

std::string ControlServer::handleSetup(const std::string &command, const std::vector<std::string> &args)
{
	if (command == "mods")
	{
		Json::Array mods;
		for (const std::string &mod : ScorchDroidSetup::mods(config_.dataRoot)) mods.add(mod);
		return Json::Object()
			.set("ok", true)
			.raw("mods", mods.text())
			.set("selected", ScorchDroidSetup::mod())
			.text();
	}
	if (command == "setmod")
	{
		const bool accepted = ScorchDroidSetup::setMod(arg(args, 0));
		// A mod brings its own bot definitions, and asking for one it does
		// not define produces a game that loads its landscape and then waits
		// forever for a bot that can never be created.
		int substituted = 0;
		if (accepted) substituted = ScorchDroidSetup::ensureBotsValidForMod(config_.dataRoot);
		return Json::Object().set("ok", true).set("accepted", accepted).set("botsSubstituted", substituted).text();
	}
	if (command == "landscapes")
	{
		Json::Array all, selected;
		for (const std::string &name : ScorchDroidSetup::landscapes(config_.dataRoot)) all.add(name);
		for (const std::string &name : ScorchDroidSetup::selectedLandscapes()) selected.add(name);
		return Json::Object().set("ok", true).raw("landscapes", all.text()).raw("selected", selected.text()).text();
	}
	if (command == "setlandscapes")
	{
		// An empty list is upstream's own "all of them", not an error - see
		// LandscapeDefinitionsBase::landscapeEnabled.
		std::vector<std::string> names;
		for (const std::string &name : args)
		{
			if (!name.empty()) names.push_back(name);
		}
		return Json::Object().set("ok", true).set("accepted", ScorchDroidSetup::setLandscapes(names)).text();
	}
	if (command == "bots")
	{
		Json::Array all, selected;
		for (const ScorchDroidSetup::Bot &bot : ScorchDroidSetup::bots(config_.dataRoot))
		{
			all.raw(Json::Object().set("name", bot.name).set("description", bot.description).text());
		}
		for (const std::string &name : ScorchDroidSetup::botTypes()) selected.add(name);
		return Json::Object().set("ok", true).raw("bots", all.text()).raw("selected", selected.text()).text();
	}
	if (command == "setbots")
	{
		std::vector<std::string> names;
		for (const std::string &name : args)
		{
			if (!name.empty()) names.push_back(name);
		}
		if (names.empty()) return Json::error("a game needs at least one kind of bot");
		return Json::Object().set("ok", true).set("accepted", ScorchDroidSetup::setBotTypes(names)).text();
	}
	if (command == "presets")
	{
		Json::Array presets;
		for (const ScorchDroidSetup::Preset &preset :
			ScorchDroidSetup::presets(config_.dataRoot, ScorchDroidSetup::mod()))
		{
			presets.raw(Json::Object()
				.set("mod", preset.mod)
				.set("name", preset.name)
				.set("description", preset.description)
				.set("gamefile", preset.gamefile)
				.text());
		}
		return Json::Object().set("ok", true).raw("presets", presets.text()).text();
	}
	if (command == "loadpreset")
	{
		return Json::Object().set("ok", true).set("accepted", ScorchDroidSetup::loadPreset(arg(args, 0))).text();
	}

	return Json::error("unknown setup command: " + command);
}
