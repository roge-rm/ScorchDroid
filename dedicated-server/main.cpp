// A genuine standalone Linux dedicated server, built from the exact same
// portable scorched_common (see cmake/ScorchedCommon.cmake) the Android
// build and host-tests use - real ScorchedServer, real NetServerTCP3
// sockets, same pinned upstream commit, so an Android ScorchDroid client
// (or any other client on the same commit) can direct-connect to it exactly
// as it would a real PC's dedicated server.
//
// Deliberately not upstream's own src/server/scorcheds/main.cpp: that pulls
// in the UDP-based ServerBrowserInfo/ServerWebServer (public server-list
// advertising, an explicitly out-of-scope separate feature) and needs
// SDL/expat/etc as real system packages via upstream's meson build. This
// reuses the exact vendored/portable build host-tests already proved works,
// so no new dependencies are needed at all.
//
// Unlike engine_jni.cpp's startLocalGame(), this adds no tank of its own -
// it's a genuine headless server with no local player, so real connecting
// clients ride the actual connect/mod-check/load-level handshake
// (ServerConnectAuthHandler::addNextTank(), ServerLoadLevel) completely
// unmodified, no promotion workarounds needed (those exist only because
// engine_jni.cpp bypasses that handshake for its own in-process local
// player - see its promoteHumanToPlaying()/setLoaded() comments).
//
// It began as a cross-play test rig with its paths compiled in. Everything
// it needs is now a flag or an environment variable, because the same
// binary is what docker/server.Dockerfile ships and a container cannot be
// recompiled to be told where its data is.

#include "ControlServer.hpp"
#include "LogRing.hpp"

#include <GameSetup.h>
#include <common/Clock.hpp>
#include <common/Defines.hpp>
#include <common/DefinesScorched.hpp>
#include <common/Logger.hpp>
#include <common/LoggerI.hpp>
#include <common/OptionsScorched.hpp>
#include <engine/Simulator.hpp>
#include <net/NetInterface.hpp>
#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <server/ServerChannelManager.hpp>
#include <server/ServerConnectAuthHandler.hpp>
#include <server/ServerFileServer.hpp>
#include <server/ServerState.hpp>
#include <server/ServerTimedMessage.hpp>
#include <tank/Tank.hpp>
#include <tank/TankState.hpp>
#include <target/TargetContainer.hpp>
#include <target/TargetLife.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
	LogRing g_log;
	volatile std::sig_atomic_t g_running = 1;

	void handleShutdownSignal(int) { g_running = 0; }

	struct Settings
	{
		std::string dataRoot     = SCORCHED_SUBMODULE_ROOT;
		std::string configPath   = DEDICATED_SERVER_XML_PATH;
		std::string stateDir     = "/tmp/scorchdroid-dedicated-server-home";
		// What a missing --config is written from. Separate from the config
		// itself because the two live in different places in a container:
		// the seed ships in the image, the config on a volume.
		std::string seedConfig   = DEDICATED_SERVER_XML_PATH;
		std::string controlSocket;
		std::string serverName;
		// Whether the name came from --name rather than the environment.
		// An explicit flag always wins; the environment only seeds a
		// config that did not exist yet, so a rename from the web admin is
		// not undone at the next restart by a variable in a .env file
		// nobody has looked at since installing.
		bool        serverNameIsExplicit = false;
		int         port         = 0;  // 0 = whatever the config says
		bool        dumpOptions  = false;
		bool        quiet        = false;
	};

	// A flag beats an environment variable beats the compiled-in default.
	// The defaults are what they always were, so running this from a
	// developer build with no arguments still serves the submodule checkout.
	std::string fromEnv(const char *name, const std::string &fallback)
	{
		const char *value = getenv(name);
		return (value && *value) ? std::string(value) : fallback;
	}

	void usage(const char *program)
	{
		std::printf(
			"ScorchDroid dedicated server\n"
			"\n"
			"Usage: %s [options] [port]\n"
			"\n"
			"  --port N                 Port to listen on (SCORCHDROID_PORT).\n"
			"                           Default: the config's PortNo, else %u.\n"
			"  --data-root DIR          Directory holding data/ (SCORCHDROID_DATA_ROOT).\n"
			"  --config FILE            Server options XML (SCORCHDROID_CONFIG). Written\n"
			"                           with upstream's defaults if it does not exist.\n"
			"  --state-dir DIR          Ban list, saves and settings (SCORCHDROID_STATE_DIR).\n"
			"  --seed FILE              Config to seed a missing --config from\n"
			"                           (SCORCHDROID_SEED_CONFIG).\n"
			"  --control-socket PATH    Unix socket for the web admin\n"
			"                           (SCORCHDROID_CONTROL_SOCKET). Off unless set.\n"
			"  --name NAME              Server name. The environment's\n"
			"                           SCORCHDROID_SERVER_NAME only names a config\n"
			"                           being created; this always wins.\n"
			"  --quiet                  Do not print the periodic status line.\n"
			"  --dump-options           Print every server option as JSON and exit.\n"
			"  --help                   This text.\n",
			program, S3D::ScorchedPort);
	}

	bool parseArguments(int argc, char **argv, Settings &settings)
	{
		settings.dataRoot      = fromEnv("SCORCHDROID_DATA_ROOT", settings.dataRoot);
		settings.configPath    = fromEnv("SCORCHDROID_CONFIG", settings.configPath);
		settings.stateDir      = fromEnv("SCORCHDROID_STATE_DIR", settings.stateDir);
		settings.seedConfig    = fromEnv("SCORCHDROID_SEED_CONFIG", settings.seedConfig);
		settings.controlSocket = fromEnv("SCORCHDROID_CONTROL_SOCKET", settings.controlSocket);
		settings.serverName    = fromEnv("SCORCHDROID_SERVER_NAME", settings.serverName);
		settings.port          = atoi(fromEnv("SCORCHDROID_PORT", "0").c_str());

		for (int i = 1; i < argc; i++)
		{
			const std::string flag = argv[i];
			const bool hasValue = (i + 1 < argc);
			const std::string value = hasValue ? argv[i + 1] : "";

			if (flag == "--help" || flag == "-h") { usage(argv[0]); return false; }
			else if (flag == "--dump-options") settings.dumpOptions = true;
			else if (flag == "--quiet") settings.quiet = true;
			else if (flag == "--port" && hasValue) { settings.port = atoi(value.c_str()); i++; }
			else if (flag == "--data-root" && hasValue) { settings.dataRoot = value; i++; }
			else if (flag == "--config" && hasValue) { settings.configPath = value; i++; }
			else if (flag == "--state-dir" && hasValue) { settings.stateDir = value; i++; }
			else if (flag == "--seed" && hasValue) { settings.seedConfig = value; i++; }
			else if (flag == "--control-socket" && hasValue) { settings.controlSocket = value; i++; }
			else if (flag == "--name" && hasValue)
			{
				settings.serverName = value;
				settings.serverNameIsExplicit = true;
				i++;
			}
			else if (flag[0] != '-' && atoi(flag.c_str()) > 0)
			{
				// The original interface was a bare port number as argv[1].
				// Still honoured, because the cross-play test notes and
				// muscle memory both use it.
				settings.port = atoi(flag.c_str());
			}
			else
			{
				std::fprintf(stderr, "Unrecognised argument: %s\n\n", flag.c_str());
				usage(argv[0]);
				return false;
			}
		}
		return true;
	}

	bool fileExists(const std::string &path)
	{
		struct stat info;
		return stat(path.c_str(), &info) == 0;
	}

	// A fresh volume has no config in it. Rather than refusing to start,
	// write out every option upstream defines with its default value and
	// carry on - which also means the file a server operator opens is a
	// complete, self-documenting list rather than the eight-line seed.
	bool seedConfig(const std::string &path, const std::string &seedFrom)
	{
		OptionsGame defaults;
		if (!seedFrom.empty() && fileExists(seedFrom)) defaults.readOptionsFromFile(seedFrom);
		return defaults.writeOptionsToFile(path, true);
	}

	void printStatusLine()
	{
		if (!ScorchedServer::serverStarted()) return;
		ScorchedServer *server = ScorchedServer::instance();
		std::printf("state=%d tanks=%u",
			(int) server->getServerState().getState(),
			server->getTargetContainer().getNoOfTanks());
		for (auto &entry : server->getTargetContainer().getTanks())
		{
			Tank *tank = entry.second;
			std::printf(" | id=%u dest=%u state=%s life=%d",
				tank->getPlayerId(), tank->getDestinationId(),
				tank->getState().getSmallStateString(),
				tank->getLife().getLife().asInt());
		}
		std::printf("\n");
		std::fflush(stdout);
	}

	// Brings the server up from whatever ScorchDroidSetup currently holds.
	// Used both at startup and by the control channel's restart, which is
	// the only way a mod change can take effect: startServerInternal()
	// calls setDataFileMod() and loadModFiles() partway through its own
	// startup, long before anything could be applied afterwards.
	bool startServerFrom(const Settings &settings, int &boundPort)
	{
		const std::string sessionPath = settings.stateDir + "/scorchdroid_session.xml";
		if (!ScorchDroidSetup::writeSessionFile(sessionPath))
		{
			Logger::log(S3D::formatStringBuffer("Failed to write session config %s", sessionPath.c_str()));
			return false;
		}

		ScorchedServerSettingsOptions serverSettings(sessionPath, false, false);
		// local=false -> real NetServerTCP3, not NetLoopBack - see
		// ScorchedServer::startServerInternal().
		if (!ScorchedServer::startServer(serverSettings, false, nullptr))
		{
			Logger::log("ScorchedServer::startServer() failed");
			return false;
		}

		ScorchedServer *server = ScorchedServer::instance();
		int port = settings.port;
		if (port <= 0) port = server->getOptionsGame().getPortNo();
		if (port <= 0) port = (int) S3D::ScorchedPort;

		if (!server->getContext().getNetInterface().start(port))
		{
			Logger::log(S3D::formatStringBuffer("Failed to bind port %d", port));
			ScorchedServer::stopServer();
			return false;
		}

		boundPort = port;
		Logger::log(S3D::formatStringBuffer(
			"ScorchDroid dedicated server listening on port %d (protocol \"%s\", game \"%s\")",
			port, S3D::ScorchedProtocolVersion.c_str(), S3D::ScorchedVersion.c_str()));
		return true;
	}

	void stopServerNow()
	{
		if (!ScorchedServer::serverStarted()) return;
		// Close the listening socket before dropping the server:
		// stopServer() deletes the NetInterface along with everything else,
		// and a socket freed only by its destructor is exactly the kind of
		// thing that leaves the next start unable to bind. Learned in
		// engine_jni.cpp's stopGame().
		ScorchedServer::instance()->getContext().getNetInterface().stop();
		ScorchedServer::stopServer();
	}
}  // namespace

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, handleShutdownSignal);
	signal(SIGTERM, handleShutdownSignal);

	Settings settings;
	if (!parseArguments(argc, argv, settings)) return 0;

	if (chdir(settings.dataRoot.c_str()) != 0)
	{
		std::fprintf(stderr, "chdir(%s) failed - is --data-root right?\n", settings.dataRoot.c_str());
		return 1;
	}

	mkdir(settings.stateDir.c_str(), 0755);
	setenv("HOME", settings.stateDir.c_str(), 1);
	S3D::setSettingsDir("scorchdroid-dedicated-server");

	// Plain stdout logging plus a ring buffer the control channel can tail.
	// Logger::log() calls (connect/disconnect events, handshake failures,
	// ...) are otherwise silently queued and never shown anywhere, exactly
	// like the Android build before its own AndroidLogLogger fix (see
	// engine_jni.cpp) - found the hard way there too.
	Logger::addLogger(&g_log);

	const bool freshConfig = !fileExists(settings.configPath);
	if (freshConfig)
	{
		if (!seedConfig(settings.configPath, settings.seedConfig))
		{
			std::fprintf(stderr, "Could not write a default config to %s\n", settings.configPath.c_str());
			return 1;
		}
		// stderr, not stdout: --dump-options has to emit nothing but JSON,
		// and a fresh run is exactly when both happen at once.
		std::fprintf(stderr, "Wrote a default server config to %s\n", settings.configPath.c_str());
	}

	ScorchDroidSetup::ensureLoaded(settings.configPath);
	// A mod defines its own bots, and a config naming one the chosen mod
	// does not provide produces a game that loads its landscape and then
	// waits forever for a bot that can never be created.
	ScorchDroidSetup::ensureBotsValidForMod(settings.dataRoot);
	// Only on a config this run just created, unless --name said so
	// outright. Otherwise the config file - which the web admin writes -
	// is what names the server.
	if (!settings.serverName.empty() && (freshConfig || settings.serverNameIsExplicit))
	{
		ScorchDroidSetup::setAny("ServerName", settings.serverName);
		ScorchDroidSetup::writeSessionFile(settings.configPath);
	}

	if (settings.dumpOptions)
	{
		ControlServer::Config dumpConfig;
		dumpConfig.configPath = settings.configPath;
		dumpConfig.dataRoot   = settings.dataRoot;
		ControlServer dumper(dumpConfig, g_log, ControlServer::Host());
		std::printf("%s\n", dumper.handle("options").c_str());
		return 0;
	}

	int boundPort = 0;
	if (!startServerFrom(settings, boundPort))
	{
		Logger::instance()->processLogEntries();
		return 1;
	}

	ControlServer::Config controlConfig;
	controlConfig.socketPath = settings.controlSocket;
	controlConfig.configPath = settings.configPath;
	controlConfig.dataRoot   = settings.dataRoot;
	controlConfig.serverName = ScorchedServer::instance()->getOptionsGame().getServerName();
	controlConfig.port       = boundPort;

	ControlServer::Host controlHost;
	controlHost.shutdown = []() { g_running = 0; };
	controlHost.restart  = [&settings, &boundPort]() {
		Logger::log("Restarting the server at the web admin's request");
		stopServerNow();
		const bool started = startServerFrom(settings, boundPort);
		if (!started) Logger::log("Restart failed - the server is now down");
		return started;
	};

	ControlServer control(controlConfig, g_log, controlHost);
	if (!settings.controlSocket.empty())
	{
		std::string error;
		if (!control.start(error))
		{
			std::fprintf(stderr, "Control socket unavailable: %s\n", error.c_str());
			return 1;
		}
		Logger::log(S3D::formatStringBuffer("Control socket listening on %s", settings.controlSocket.c_str()));
	}

	std::printf("Press Ctrl+C to stop.\n");
	std::fflush(stdout);

	Clock tickClock;
	unsigned int statusTickCounter = 0;
	while (g_running)
	{
		unsigned int ticksDifference = tickClock.getTicksDifference();
		fixed timeDifference(true, ((Sint64) ticksDifference) * 10);

		Logger::instance()->processLogEntries();

		if (ScorchedServer::serverStarted())
		{
			ScorchedServer *server = ScorchedServer::instance();
			server->getNetInterface().processMessages();
			server->getSimulator().simulate();
			server->getServerState().simulate(timeDifference);
			server->getServerConnectAuthHandler().processMessages();
			server->getServerFileServer().simulate();
			server->getServerChannelManager().simulate(timeDifference);
			server->getTimedMessage().simulate();
		}

		// After the engine, so a restart command tears down and rebuilds
		// between ticks rather than underneath one.
		control.setPort(boundPort);
		control.poll();

		// Once every ~5s (at the ~50Hz poll rate below), print a status
		// line so it's obvious from the terminal that this is actually
		// alive and whether anyone's connected - the real dedicated
		// server's own console output plays the same role.
		if (!settings.quiet && (statusTickCounter++ % 250) == 0) printStatusLine();

		usleep(20000);  // ~50Hz, matching the Android build's own tick rate.
	}

	std::printf("Shutting down.\n");
	control.stop();
	stopServerNow();
	Logger::instance()->processLogEntries();
	return 0;
}
