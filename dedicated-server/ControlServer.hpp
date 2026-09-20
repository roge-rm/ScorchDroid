#ifndef SCORCHDROID_CONTROL_SERVER_HPP
#define SCORCHDROID_CONTROL_SERVER_HPP

#include "LogRing.hpp"

#include <deque>
#include <functional>
#include <string>
#include <vector>

// The dedicated server's local control channel: how the web admin sees what
// the server is doing and tells it what to do.
//
// It is a Unix domain socket and nothing else - never a TCP port, never
// reachable from the network, and completely invisible to a connected game
// client. It carries no Scorched3D message and adds nothing to common/coms,
// so the game's wire protocol, and with it desktop-Scorched3D
// compatibility, is exactly what it was.
//
// The protocol is asymmetric on purpose. A request is one line of
// tab-separated fields, which needs no parser. A reply is one line of JSON,
// which the Python side gets for free. See docs/dedicated-server.md for the
// command table.
//
// poll() is called from the server's own 50Hz loop, so every handler runs on
// the engine thread between simulation ticks and needs no locking against
// it. The price is that a handler must never block; none does.
class ControlServer
{
public:
	struct Config
	{
		std::string socketPath;
		// Where options.save writes. The same file the server reads at
		// startup, so a save followed by a restart is how a settings change
		// actually lands.
		std::string configPath;
		std::string dataRoot;
		std::string serverName;
		int         port = 0;
	};

	// The two things the control channel must not decide for itself,
	// because only the process that owns the main loop can: restarting the
	// server in place, and ending the process.
	struct Host
	{
		std::function<bool()> restart;
		std::function<void()> shutdown;
	};

	ControlServer(const Config &config, LogRing &log, Host host);
	~ControlServer();

	// Binds and listens. Removes a stale socket file left by a process that
	// died without cleaning up, which in a container is the normal case.
	bool start(std::string &error);

	// Accepts new connections and serves whatever is ready. Never blocks.
	void poll();

	void stop();

	// Runs one request line and returns the reply, without any socket
	// involved. This is the whole command surface, and it is what
	// host-tests drives.
	std::string handle(const std::string &line);

	const Config &config() const { return config_; }

	void setPort(int port) { config_.port = port; }

private:
	struct Client
	{
		int         fd = -1;
		std::string in;
		std::string out;
	};

	Config             config_;
	LogRing &          log_;
	Host               host_;
	int                listenFd_ = -1;
	std::vector<Client> clients_;
	unsigned long long  startedAt_ = 0;

	void serve(Client &client);
	void closeClient(Client &client);

	std::string handleStatus(const std::vector<std::string> &args);
	std::string handleLog(const std::vector<std::string> &args);
	std::string handleChat(const std::vector<std::string> &args);
	std::string handleSay(const std::vector<std::string> &args);
	std::string handleAdmin(const std::vector<std::string> &args);
	std::string handleOptions(const std::vector<std::string> &args);
	std::string handleSetup(const std::string &command, const std::vector<std::string> &args);
};

#endif  // SCORCHDROID_CONTROL_SERVER_HPP
