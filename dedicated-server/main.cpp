// M5: a genuine standalone Linux dedicated server, built from the exact
// same portable scorched_common (see host-tests/CMakeLists.txt) the Android
// build and host-tests already use - real ScorchedServer, real
// NetServerTCP3 sockets, same pinned upstream commit, so an Android
// ScorchDroid client (or any other client on the same commit) can
// direct-connect to it exactly as it would a real PC's dedicated server -
// see the porting plan's M5 cross-play notes. Deliberately not upstream's
// own src/server/scorcheds/main.cpp: that pulls in the UDP-based
// ServerBrowserInfo/ServerWebServer (public server-list advertising, an
// explicitly out-of-scope separate feature - see the plan) and needs
// SDL/expat/etc as real system packages via upstream's meson build, neither
// of which this sandbox has (no root to apt-get install, no meson/ninja).
// This reuses the exact vendored/portable build host-tests already proved
// works, so no new dependencies are needed at all.
//
// Unlike engine_jni.cpp's startLocalGame(), this adds no tank of its own -
// it's a genuine headless server with no local player, so real connecting
// clients ride the actual connect/mod-check/load-level handshake
// (ServerConnectAuthHandler::addNextTank(), ServerLoadLevel) completely
// unmodified, no promotion workarounds needed (those exist only because
// engine_jni.cpp bypasses that handshake for its own in-process local
// player - see its promoteHumanToPlaying()/setLoaded() comments).

#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <server/ServerState.hpp>
#include <server/ServerFileServer.hpp>
#include <server/ServerChannelManager.hpp>
#include <server/ServerTimedMessage.hpp>
#include <server/ServerConnectAuthHandler.hpp>
#include <net/NetInterface.hpp>
#include <engine/Simulator.hpp>
#include <common/Defines.hpp>
#include <common/DefinesScorched.hpp>
#include <common/Clock.hpp>
#include <common/Logger.hpp>
#include <common/LoggerI.hpp>
#include <common/OptionsScorched.hpp>
#include <target/TargetContainer.hpp>
#include <target/TargetLife.hpp>
#include <tank/Tank.hpp>
#include <tank/TankState.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

namespace
{
	// Plain stdout logging - Logger::log() calls (connect/disconnect
	// events, handshake failures, ...) are otherwise silently queued and
	// never shown anywhere, exactly like the Android build before its own
	// AndroidLogLogger fix (see engine_jni.cpp) - found the hard way there
	// too.
	class StdoutLogger : public LoggerI
	{
	public:
		void logMessage(LoggerInfo &info) override
		{
			std::printf("[%s] %s\n", info.getTime(), info.getMessage());
			std::fflush(stdout);
		}
	};

	StdoutLogger g_stdoutLogger;
	volatile std::sig_atomic_t g_running = 1;

	void handleShutdownSignal(int)
	{
		g_running = 0;
	}
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, handleShutdownSignal);
	signal(SIGTERM, handleShutdownSignal);

	int port = (int) S3D::ScorchedPort;
	if (argc > 1) port = atoi(argv[1]);

	if (chdir(SCORCHED_SUBMODULE_ROOT) != 0)
	{
		std::fprintf(stderr, "chdir(%s) failed\n", SCORCHED_SUBMODULE_ROOT);
		return 1;
	}
	setenv("HOME", "/tmp/scorchdroid-dedicated-server-home", 1);
	S3D::setSettingsDir("scorchdroid-dedicated-server");

	Logger::addLogger(&g_stdoutLogger);

	ScorchedServerSettingsOptions settings(DEDICATED_SERVER_XML_PATH, false, false);

	// local=false -> real NetServerTCP3, not NetLoopBack - see
	// ScorchedServer::startServerInternal().
	if (!ScorchedServer::startServer(settings, false, nullptr))
	{
		std::fprintf(stderr, "ScorchedServer::startServer() failed\n");
		return 1;
	}

	ScorchedServer *server = ScorchedServer::instance();
	if (!server->getContext().getNetInterface().start(port))
	{
		std::fprintf(stderr, "Failed to bind port %d\n", port);
		return 1;
	}

	std::printf("ScorchDroid dedicated server listening on port %d (protocol version \"%s\", game version \"%s\")\n",
		port, S3D::ScorchedProtocolVersion.c_str(), S3D::ScorchedVersion.c_str());
	std::printf("Press Ctrl+C to stop.\n");
	std::fflush(stdout);

	Clock tickClock;
	unsigned int statusTickCounter = 0;
	while (g_running)
	{
		unsigned int ticksDifference = tickClock.getTicksDifference();
		fixed timeDifference(true, ((Sint64) ticksDifference) * 10);

		Logger::instance()->processLogEntries();

		server->getNetInterface().processMessages();
		server->getSimulator().simulate();
		server->getServerState().simulate(timeDifference);
		server->getServerConnectAuthHandler().processMessages();
		server->getServerFileServer().simulate();
		server->getServerChannelManager().simulate(timeDifference);
		server->getTimedMessage().simulate();

		// Once every ~5s (at the ~50Hz poll rate below), print a status
		// line so it's obvious from the terminal that this is actually
		// alive and whether anyone's connected - the real dedicated
		// server's own console output plays the same role.
		if ((statusTickCounter++ % 25) == 0)
		{
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

		usleep(20000);  // ~50Hz, matching the Android build's own tick rate.
	}

	std::printf("Shutting down.\n");
	ScorchedServer::stopServer();
	return 0;
}
