#include <jni.h>
#include <android/log.h>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <mutex>

// Guards ScorchedServer state shared between this simulation thread and the
// GL render thread (see renderer_jni.cpp), which reads live tank/landscape
// state every frame.
std::mutex g_engineMutex;

#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <server/ServerState.hpp>
#include <server/ServerFileServer.hpp>
#include <server/ServerChannelManager.hpp>
#include <server/ServerTimedMessage.hpp>
#include <server/ServerConnectAuthHandler.hpp>
#include <server/ServerLoadLevel.hpp>
#include <server/ServerHandlers.hpp>
#include <server/ServerDestinations.hpp>
#include <target/TargetContainer.hpp>
#include <engine/Simulator.hpp>
#include <net/NetInterface.hpp>
#include <common/Defines.hpp>
#include <common/Clock.hpp>
#include <common/Logger.hpp>
#include <common/LoggerI.hpp>
#include <tank/Tank.hpp>
#include <tanket/TanketAccessories.hpp>
#include <tanket/TanketShotInfo.hpp>
#include <tanket/TanketWeapon.hpp>
#include <weapons/Accessory.hpp>
#include <coms/ComsPlayedMoveMessage.hpp>
#include <SoundEventQueue.h>
#include <target/TargetLife.hpp>
#include <landscapemap/LandscapeMaps.hpp>
// M6 tank movement: upstream fires a WeaponMoveTank as an ordinary shot
// with a selected landscape position, and MovementMap decides what is
// reachable - see firePositionSelect()/refreshMovementMask() below.
#include <landscapemap/MovementMap.hpp>
#include <weapons/WeaponMoveTank.hpp>
#include <weapons/AccessoryStore.hpp>
#include <target/TargetState.hpp>
#include <MovementStore.h>
#include <TracerStore.h>
#include <TargetModelStore.h>
#include <DeformEventQueue.h>
#include <EffectEventQueue.h>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <simactions/TankAddSimAction.hpp>
#include <simactions/TankAccessorySimAction.hpp>
#include <tankai/TankAIAdder.hpp>
#include <server/ServerSimulator.hpp>
#include <tank/TankScore.hpp>
#include <common/OptionsTransient.hpp>
#include <common/ChannelText.hpp>
#include <coms/ComsChannelTextMessage.hpp>
#include <ChatStore.h>
#include <ScoreboardState.h>
#include <GameSetup.h>
#include <PlayerProfile.h>
#include <weapons/AccessoryStore.hpp>
#include <weapons/AccessoryPart.hpp>
#include <coms/ComsBuyAccessoryMessage.hpp>
#include <coms/ComsTankChangeMessage.hpp>
#include <simactions/TankChangeSimAction.hpp>
// M6 parity: defense accessories (shields/parachutes/batteries) and the
// wind indicator - see useDefense()/getWindInfo() below.
#include <coms/ComsDefenseMessage.hpp>
#include <simactions/TankDefenseSimAction.hpp>
#include <engine/Wind.hpp>
#include <target/TargetShield.hpp>
#include <target/TargetParachute.hpp>
#include <tank/TankModelStore.hpp>
#include <tanket/TanketType.hpp>
#include <tank/TankState.hpp>
#include <common/OptionsScorched.hpp>
#include <ClientContext.hpp>
#include <EngineState.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <sstream>

#define LOG_TAG "ScorchDroidEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Upstream's own Logger::log() (used throughout common/coms and
// server/server - e.g. ServerConnectHandler's version-mismatch messages,
// ClientContext::fail()'s disconnect reasons) only ever queues messages;
// Logger::processLogEntries() (already called every tickEngine(), see
// below) just hands each one to whatever LoggerI instances are registered
// via Logger::addLogger() - and until now, nothing ever registered one, so
// every one of those messages was silently dropped. Found the hard way
// debugging an M5 Phase 2 "Disconnected by host" failure with no visible
// reason anywhere. This makes them show up in logcat like any other engine
// log line, permanently - not a temporary debug hook.
class AndroidLogLogger : public LoggerI {
public:
    void logMessage(LoggerInfo &info) override {
        LOGI("%s", info.getMessage());
    }
};
static AndroidLogLogger g_androidLogLogger;

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_helloFromNative(JNIEnv *env, jobject /* this */) {
    std::string greeting = "Hello from the native engine (M0 toolchain proof)";
    return env->NewStringUTF(greeting.c_str());
}

// Upstream's file I/O (S3D::getDataFile/getHomeFile/etc.) resolves paths
// relative to the process's current working directory and $HOME - see
// AssetDataExtractor.kt and the porting plan. Pointing both at the
// extracted data root here means that code runs completely unmodified.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_initEngine(JNIEnv *env, jobject /* this */, jstring dataRoot) {
    Logger::addLogger(&g_androidLogLogger);

    const char *path = env->GetStringUTFChars(dataRoot, nullptr);

    bool ok = true;
    if (chdir(path) != 0) {
        LOGE("chdir(%s) failed", path);
        ok = false;
    }
    setenv("HOME", path, 1);

    // S3D::getSettingsFile() (used for save games, key bindings, mod/lua
    // hook directories, etc.) asserts if this is never set - on desktop
    // it's set from main() before anything else runs.
    S3D::setSettingsDir("scorchdroid");

    LOGI("Engine data root: %s", path);
    env->ReleaseStringUTFChars(dataRoot, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// M2 vertical slice: boot a real local (loopback) game via the actual
// upstream ScorchedServer bootstrap path, using a ScorchDroid-owned
// settings file (not upstream's data/server.xml, kept pristine) that turns
// on bots so a practice-vs-AI game can actually start on a single device -
// see the porting plan for why this was chosen over stubbing the
// weapon/data system.
// Any non-zero destinationId marks a tank as a "real" (non-bot) player
// throughout the upstream state machine (ServerStateEnoughPlayers::countBots/
// addBots/removeBots, TargetDamage, SaveGame, ...) - see TankAddSimAction's
// invokeAction(), which only calls setTankAI() when the server passes a
// non-empty aiName, and only does that for destinationId==0 bot slots (see
// ServerStateEnoughPlayers::addBots -> TankAIAdder::addTankAI). A real
// network client is never destinationId 0, so this constant just needs to be
// any single fixed non-zero value, since ScorchDroid never has more than one
// local human seat.
static const unsigned int kHumanDestinationId = 1;

// M5: set by startLocalGame(), read by isHostingOnNetwork()/getServerPort()
// below so the UI can show real hosting status instead of assuming success.
static bool g_hostingListening = false;
static int g_hostingPort = 0;

// Set true once promoteHumanToPlaying() (below) has run - reset alongside
// g_hostingListening/g_hostingPort in startLocalGame().
static bool g_humanPromoted = false;

// Guards the per-round "local destination finished loading" self-resolution
// below (see the comment where it's used, in tickEngine()) against queuing
// a redundant TankLoadedSimAction every tick while one is already in flight.
static bool g_humanLoadPending = false;

// M5 Phase 2: which of the two mutually-exclusive game roles this process
// is playing - decided once, by whichever of startLocalGame()/startJoinGame()
// the UI calls first (see MainActivity's host/join chooser). Every function
// below that used to reach straight for ScorchedServer::instance() now goes
// through activeContext()/findMyTank() instead, so the same JNI surface
// (tickEngine, fireWeapon, getWeaponShop, buyAccessory, ...) works
// unchanged in either role - see the porting plan's M5 Phase 2 notes.
enum class EngineMode { kNone, kHost, kClient };
static EngineMode g_mode = EngineMode::kNone;
static ClientContext *g_clientContext = nullptr;

// The single ScorchedContext this process is driving, or nullptr if neither
// role has reached a usable state yet (host: ScorchedServer constructed;
// client: handshake has reached sJoined - see ClientContext.hpp). Every
// query below (target container, accessory store, landscape maps, ...)
// lives on the shared ScorchedContext base, which is exactly why Phase 1
// built ClientContext as one - see the porting plan.
static ScorchedContext *activeContext() {
    if (g_mode == EngineMode::kHost) {
        return ScorchedServer::serverStarted() ? &ScorchedServer::instance()->getContext() : nullptr;
    }
    if (g_mode == EngineMode::kClient) {
        return (g_clientContext && g_clientContext->getState() == ClientContext::sJoined)
               ? static_cast<ScorchedContext *>(g_clientContext) : nullptr;
    }
    return nullptr;
}

// Exposes activeContext() to renderer_jni.cpp (see EngineState.hpp) - the
// GL thread needs the exact same "which role, if any, is actually usable
// right now" logic as the simulation thread above, not a copy of it.
ScorchedContext *engineActiveContext() {
    return activeContext();
}

unsigned int engineMyDestinationId() {
    if (g_mode == EngineMode::kClient && g_clientContext) return g_clientContext->getMyDestinationId();
    return kHumanDestinationId;
}

// Adds "my tank": constructed exactly the way ServerConnectAuthHandler::
// addNextTank() builds a tank for a real, freshly-connected network client
// (same TankAddSimAction constructor, same empty aiName so no TankAI ever
// gets attached, same non-zero destinationId) - see the porting plan's
// investigation of TankAddSimAction/TankAIAdder/ServerConnectAuthHandler.
// addNextTank() itself is protected and reached only via the real
// ComsConnectAuthMessage network handshake, so this calls the same public
// building blocks it uses (TankAIAdder::getNextTankId + the public
// TankAddSimAction constructor + ServerSimulator::addSimulatorAction)
// in-process instead, without needing a real handshake or NetLoopBack pair.
//
// Also registers a ServerDestination for kHumanDestinationId first - a real
// client gets one from ServerMessageHandler::clientConnected() when it
// physically connects (see ServerDestinations::addDestination). Without it,
// TankAddSimAction::invokeAction() itself notices there's no
// ServerDestination for a non-zero destinationId ("the destination was
// closed during tank creation") and immediately destroys the tank again on
// the very next tick - found via host-tests/main.cpp when a longer-running
// regression test (buy/select, which needs several simulated seconds to
// resolve) kept losing the tank partway through, unlike the single-tick
// smoke test that first validated this mechanism.
static void addHumanTank() {
    ScorchedServer *server = ScorchedServer::instance();
    server->getServerDestinations().addDestination(kHumanDestinationId, 0);

    std::set<unsigned int> takenPlayerIds;
    unsigned int tankId = TankAIAdder::getNextTankId("", server->getContext(), takenPlayerIds);

    TankAddSimAction *simAction = new TankAddSimAction(
        tankId, kHumanDestinationId,
        "", "", "", 0,
        LANG_STRING(ScorchDroidProfile::name()), "");
    server->getServerSimulator().addSimulatorAction(simAction);
    LOGI("addHumanTank: queued player id=%u", tankId);
}

// A freshly-added tank sits in TankState::sLoading (not "playing" - see
// TankState::getTankPlaying()) until something sends a
// ComsTankChangeMessage(spectate=false) for it. TankAddSimAction does this
// itself for a bot (destinationId==0, see its own queued TankChangeSimAction),
// but never for a real destination - a real client is expected to send this
// once its own "choose your tank" step happens, which ScorchDroid's UI
// (host or joined client) never has. Without it,
// ServerStateEnoughPlayers::enoughPlayers() can never count this tank as
// playing and the match never leaves ServerWaitingForPlayersState - only
// its 900-real-second-default IdleCycleTime fallback would ever move it,
// found the hard way chasing why a fired shot never arrived anywhere.
// Called once tickEngine() sees the tank actually exists (addHumanTank()'s
// own TankAddSimAction needs a simulate() pass first) - see g_humanPromoted.
static void promoteHumanToPlaying(ScorchedServer *server, Tank *tank) {
    // TankChangeSimAction::invokeAction()'s "promote to playing" branch only
    // fires if the tank is already TankState::sSpectator (see
    // TankChangeSimAction.cpp) - true for a bot immediately, since
    // TankAddSimAction sets that synchronously for any destinationId==0
    // tank right in its own invokeAction(). Our human tank has no such
    // shortcut (addHumanTank() never goes through the real connect/
    // load-level handshake a network client's tank rides to get there via
    // TankLoadedSimAction - see ServerLoadLevel::setLoaded()), so it's
    // still sLoading here and the TankChangeSimAction queued below would
    // otherwise silently no-op. Mirrors TankAddSimAction's own bot-case
    // exactly: a direct, synchronous transition, not itself a broadcast
    // action (matches how a fresh tank's initial state is never itself
    // something other clients need a live update about - anyone joining
    // afterward sees it correctly via the normal full-snapshot
    // ComsLoadLevelMessage).
    if (tank->getState().getState() == TankState::sLoading) {
        tank->getState().setState(TankState::sSpectator);
    }

    // playerType must be exactly "Human" here, not "" (which the bot's own
    // TankAddSimAction-queued TankChangeSimAction call uses) -
    // TankChangeSimAction::invokeAction() branches its *entire* handling on
    // strcmp(playerType, "Human") != 0, and the non-Human branch
    // unconditionally overwrites the tank's destinationId with the
    // message's own (0 for a bot) - copying the bot's "" literal here
    // silently reassigned our human tank's destinationId to 0, making it
    // look like a second bot slot with no TankAI attached, which then
    // segfaulted the very next tick in ServerStateEnoughPlayers::countBots()
    // (unconditional current->getTankAI()->removedPlayer() for any
    // destinationId==0 tank). destinationId must also be this tank's own
    // (not the bot's literal 0) for the same reason.
    TankModel *tankModel = server->getTankModels().getRandomModel(
        (int) tank->getTeam(), false, tank->getTanketType()->getName());
    ComsTankChangeMessage tankChangeMessage(
        tank->getPlayerId(), tank->getTargetName(), tank->getColor(),
        tank->getTanketType()->getName(), tankModel->getName(),
        tank->getDestinationId(), tank->getTeam(), "Human", false);
    TankChangeSimAction *changeAction = new TankChangeSimAction(tankChangeMessage);
    server->getServerSimulator().addSimulatorAction(changeAction);
    LOGI("promoteHumanToPlaying: queued for player id=%u", tank->getPlayerId());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_startLocalGame(
        JNIEnv *env, jobject /* this */, jboolean debugBuild) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kNone) {
        LOGE("startLocalGame: engine already started (mode=%d)", (int) g_mode);
        return JNI_FALSE;
    }
    // M10: the server reads the player's own setup rather than the shipped
    // config directly. Written out first, because some options - the mod above
    // all - are consumed *inside* startServerInternal(), which calls
    // setDataFileMod() and loadModFiles() partway through its own startup.
    // Anything applied afterwards would be too late for those, and a mod that
    // silently failed to load would look like the mod being broken rather than
    // never selected. Options that arrive this way are also what
    // OptionsScorched snapshots, so they cannot be reverted by commitChanges()
    // at the first round.
    //
    // Falls back to the shipped config if the file can't be written, so a
    // read-only or full data directory costs the player their choices rather
    // than the game.
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    // A mod brings its own set of bot AIs, and the shipped config names one
    // ("Moron") that the Apocalypse mod does not provide - five of its
    // definitions, that one included, sit inside an XML comment. A slot naming
    // an AI the engine cannot create never fills, and the game then loads its
    // landscape and waits forever. Done before the config is written, since
    // that is what the server reads.
    const int botsChanged = ScorchDroidSetup::ensureBotsValidForMod(".");
    if (botsChanged > 0) {
        LOGI("Mod \"%s\" does not provide the configured bot(s); substituted %d",
             ScorchDroidSetup::mod().c_str(), botsChanged);
    }
    const char *kSessionFile = "scorchdroid_session.xml";
    const bool wroteSession = ScorchDroidSetup::writeSessionFile(kSessionFile);
    if (!wroteSession) {
        LOGE("Could not write %s - starting with the shipped config instead", kSessionFile);
    }
    ScorchedServerSettingsOptions settings(
        wroteSession ? kSessionFile : "scorchdroid_server.xml", false, false);

    // M5: local=false makes ScorchedServer::startServerInternal() set up a
    // real NetServerTCP3 instead of NetLoopBack (see ScorchedServer.cpp) -
    // the same class ServerMain.cpp's dedicated-server binary uses, since
    // "server" here just means "the side that owns ScorchedServer's game
    // state", not "not this device". A device practicing solo against bots
    // is still fully playable this way (nothing else about addHumanTank()/
    // the bot-balancing config changes) - it now *also* listens for real
    // incoming connections, which is what turns this into a real LAN host.
    bool started = ScorchedServer::startServer(settings, false, nullptr);
    LOGI("ScorchedServer::startServer -> %d", started);
    if (!started) return JNI_FALSE;

    OptionsScorched &options = ScorchedServer::instance()->getOptionsGame();

    // Debug builds start rich, purely so testing does not have to play
    // several rounds to afford the thing being tested - a nuke to see the
    // mushroom cloud, a shield to see a shield hit. Gated on BuildConfig.DEBUG
    // from the caller and applied *after* the config is read, so the shipped
    // scorchdroid_server.xml keeps upstream's own starting money and a
    // release build is unaffected. This is the one place this port touches a
    // gameplay number, and it must never reach a release.
    if (debugBuild) {
        const int kDebugStartMoney = 100000;
        const bool ok = options.getMainOptions().getStartMoneyEntry()
            .setValue(kDebugStartMoney);
        LOGI("Debug build: starting money set to %d (accepted=%d)",
             kDebugStartMoney, ok ? 1 : 0);
    }

    // Once, after every write above. The setup screen's own choices no longer
    // need it - they arrive through the session config, before the snapshot is
    // taken - but the debug money flag above is still written after startup
    // and does.
    //
    // updateChangeSet() re-takes the snapshot OptionsScorched keeps of the
    // main options. Without it these writes are undone the moment the first
    // round starts: ServerStateNewGame calls commitChanges(), which copies
    // that snapshot *back* over the main options, and the snapshot was taken
    // inside startServer() - before any of this ran. The entries then still
    // report themselves as "changed" while holding the file's values, which
    // is as confusing as it sounds. testGameSetup fails without this line.
    options.updateChangeSet();
    LOGI("Game options applied: rounds=%d turns=%d money=%d",
         options.getNoRounds(), options.getNoTurns(),
         options.getStartMoney());

    int port = ScorchedServer::instance()->getOptionsGame().getPortNo();
    g_hostingPort = port;
    g_hostingListening = ScorchedServer::instance()->getContext().getNetInterface().start(port);
    LOGI("NetInterface::start(%d) -> %d", port, g_hostingListening);
    if (!g_hostingListening) {
        // Not fatal - matches upstream's own single-player-vs-loopback
        // fallback in spirit: local practice against bots still works
        // without a real listening socket (e.g. port already in use by
        // another instance on this device), it just isn't joinable over
        // the network. ServerMain.cpp's own dialogExit() on this same
        // failure is too harsh for an interactive app.
        LOGE("Failed to bind port %d - continuing without LAN hosting", port);
    }

    addHumanTank();
    g_humanPromoted = false;
    g_humanLoadPending = false;
    g_mode = EngineMode::kHost;
    return JNI_TRUE;
}

// M5 Phase 2: the "join" counterpart to startLocalGame() - opens a real
// socket to a host (found via NSD or typed in manually, see MainActivity)
// and starts the ClientContext handshake (see ClientContext.hpp). Like
// startLocalGame(), this only kicks the handshake off; tickEngine() below
// drives it (and, once joined, the simulation) forward one poll at a time.
// Mutually exclusive with startLocalGame() - a process is either the host
// or a client, never both (matches the real Scorched3D architecture: even
// a host's own local player is just an in-process shortcut for what a real
// client's connection would do, see addHumanTank() above).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_startJoinGame(JNIEnv *env, jobject /* this */,
                                                    jstring jHost, jint port) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kNone) {
        LOGE("startJoinGame: engine already started (mode=%d)", (int) g_mode);
        return JNI_FALSE;
    }

    const char *host = env->GetStringUTFChars(jHost, nullptr);
    auto *client = new ClientContext();
    bool connecting = client->connectToServer(host, (int) port);
    LOGI("ClientContext::connectToServer(%s, %d) -> %d", host, (int) port, connecting);
    env->ReleaseStringUTFChars(jHost, host);

    if (!connecting) {
        delete client;
        return JNI_FALSE;
    }

    g_clientContext = client;
    g_mode = EngineMode::kClient;
    return JNI_TRUE;
}

// M5 Phase 2: polls the client handshake's progress - see ClientContext::
// State. Returns -1 if this process isn't in client mode at all (e.g. it's
// hosting, or nothing has started yet), so the UI can tell "not a client"
// apart from "still connecting" (state 0).
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getClientJoinState(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kClient || !g_clientContext) return -1;
    return (jint) g_clientContext->getState();
}

// M5 Phase 2: human-readable reason the join failed (see ClientContext::
// fail()) - only meaningful once getClientJoinState() reports sFailed.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getClientFailureReason(JNIEnv *env, jobject /* this */) {
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        if (g_mode == EngineMode::kClient && g_clientContext) {
            reason = g_clientContext->getFailureReason();
        }
    }
    return env->NewStringUTF(reason.c_str());
}

// M5: lets the UI show "Hosting on <local-ip>:<port>" (the local-ip part is
// looked up on the Kotlin side, see MainActivity.getLocalIpAddress - plain
// Android API, no JNI needed for that half) without hardcoding the default
// port (data/server.xml or scorchdroid_server.xml could in principle
// override PortNo, even though ScorchDroid's own config doesn't today).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_isHostingOnNetwork(JNIEnv *env, jobject /* this */) {
    return g_hostingListening ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getServerPort(JNIEnv *env, jobject /* this */) {
    return (jint) g_hostingPort;
}

// Shared by the economy/fire JNI functions below - not thread-safe on its
// own, callers must already hold g_engineMutex. Identifies "my tank" the
// same way in either role: the one tank whose destinationId matches this
// process's own destination - kHumanDestinationId when hosting (assigned by
// addHumanTank() above), or the id the host handed back in
// ComsConnectAcceptMessage when joined as a client (see ClientContext::
// getMyDestinationId()). Returns nullptr if that tank hasn't shown up in
// the target container yet - either because addHumanTank()'s
// TankAddSimAction hasn't been simulated yet, or (client mode) because of
// the known Phase 1 "own tank" promotion-timing gap - see the porting plan.
static Tank *findMyTank() {
    ScorchedContext *ctx = activeContext();
    if (!ctx) return nullptr;

    unsigned int myDestinationId = (g_mode == EngineMode::kClient)
                                    ? g_clientContext->getMyDestinationId() : kHumanDestinationId;

    std::map<unsigned int, Tank *> &tanks = ctx->getTargetContainer().getTanks();
    for (auto &entry : tanks) {
        if (entry.second->getDestinationId() == myDestinationId) {
            return entry.second;
        }
    }
    return nullptr;
}

// "My tank"'s playerId (see findMyTank() above), or 0 (never a valid
// playerId) if it isn't in the target container yet.
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getMyTankId(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    return tank ? (jint) tank->getPlayerId() : 0;
}

// M4 economy: current money for "my tank" (see TankScore::getMoney), or -1
// if the tank doesn't exist yet.
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getMyMoney(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return -1;
    return (jint) tank->getScore().getMoney();
}

// M4 economy, M6 parity: the shop list for "my tank" - every purchasable
// accessory (not noBuy/aiOnly/botOnly - see Accessory.hpp), one string per
// entry, pipe-delimited:
// "accessoryId|name|price|ownedCount|isCurrentWeapon(0/1)|type".
// Deliberately a flat delimited string array rather than a richer JNI object
// graph - the upstream data itself has no characters needing escaping
// (XML-sourced names), and this is the same "engine parses XML/Lua, UI just
// displays" split described in the porting plan for the eventual Compose HUD.
//
// M6: this used to filter to AccessoryPart::AccessoryWeapon only, which made
// 4 of upstream's 5 accessory types (parachutes, shields, auto-defense,
// batteries - see AccessoryPart::AccessoryType) completely unreachable: not
// buyable here and not usable anywhere, even though the engine fully
// supports them. That was the single biggest gameplay-parity gap found in
// the M6 control audit (see the porting plan). The type is now reported per
// row so the UI can group/label them and offer the right action (buy vs.
// raise a shield vs. use a battery - see useDefense() below).
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_getWeaponShop(JNIEnv *env, jobject /* this */) {
    std::vector<std::string> rows;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        Tank *tank = findMyTank();
        if (ctx && tank) {
            Accessory *current = tank->getAccessories().getWeapons().getCurrent();
            std::list<Accessory *> accessories =
                    ctx->getAccessoryStore().getAllAccessories(AccessoryStore::SortName);
            for (Accessory *accessory: accessories) {
                // Upstream's own test, rather than a hand-rolled subset of
                // it: TanketAccessories::accessoryAllowed is what its buy
                // dialog calls (BuyAccessoryDialog::addAccessory) and it
                // covers several rules this list was missing - most
                // visibly `maximumNumber == 0`, which is how the data marks
                // an accessory a tank may never hold. That is what every
                // $0 oddity in the shop was: BoidsLaser, Bomb At Tank,
                // DriveOverDestroy, Fire At Tank, GroupWin and friends are
                // internal actions fired by landscape events and collision
                // rules, not things to buy. It also brings in the arms
                // level (weapons unlock as the game escalates), the tank
                // type's own disabled list, and "already hold an infinite
                // one".
                //
                // ...but "not buyable" is not "not worth showing": this
                // shop doubles as the weapon selector, and an owned
                // infinite weapon (your starting Baby Missile) fails
                // accessoryAllowed precisely because you cannot buy another.
                // So anything already held stays listed.
                const int owned = tank->getAccessories().getAccessoryCount(accessory);
                const bool buyable = tank->getAccessories().accessoryAllowed(accessory, 0);
                if (!buyable && owned == 0) continue;

                const char *typeName = "other";
                switch (accessory->getType()) {
                    case AccessoryPart::AccessoryWeapon:      typeName = "weapon"; break;
                    case AccessoryPart::AccessoryParachute:   typeName = "parachute"; break;
                    case AccessoryPart::AccessoryShield:      typeName = "shield"; break;
                    case AccessoryPart::AccessoryAutoDefense: typeName = "autodefense"; break;
                    case AccessoryPart::AccessoryBattery:     typeName = "battery"; break;
                }

                std::ostringstream row;
                row << accessory->getAccessoryId() << '|'
                    << accessory->getName() << '|'
                    << accessory->getPrice() << '|'
                    << owned << '|'
                    << (accessory == current ? 1 : 0) << '|'
                    << typeName << '|'
                    // Which shop tab upstream files this under, which is not
                    // always what its type implies: Fuel and Rocket Fuel are
                    // weapons by type but carry <tabgroup>defense</tabgroup>
                    // in accessories.xml, because you buy them alongside
                    // shields and parachutes rather than alongside missiles.
                    // Accessory::parseXML defaults the field from the type
                    // when the data doesn't say, so reading it is always at
                    // least as good as inferring it.
                    << accessory->getTabGroupName();
                rows.push_back(row.str());
            }
        }
    }

    jobjectArray result = env->NewObjectArray((jsize) rows.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < rows.size(); i++) {
        env->SetObjectArrayElement(result, (jsize) i, env->NewStringUTF(rows[i].c_str()));
    }
    return result;
}

// M4 economy, M5 Phase 2: buys (buy=true) or sells (buy=false) one unit of
// the given accessory for "my tank" - constructs the same
// ComsBuyAccessoryMessage a real client's shop UI would send. Host mode
// queues the resulting TankAccessorySimAction directly in-process, exactly
// like fireWeapon()/addHumanTank() above; client mode instead sends the
// message to the real host over the real socket (see ClientContext::
// sendGameMessage()) and waits for the resulting TankAccessorySimAction to
// come back (like anyone else's) via ComsSimulateMessage -
// ServerBuyAccessoryHandler.cpp was already wired up server-side since M1,
// so nothing server-side needed to change for this. Either way,
// TankAccessorySimAction::invokeAction() itself re-checks money/ownership/
// noBuy once simulated, so an invalid request is simply a no-op, not a
// crash - this function only reports whether the request was queued/sent.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_buyAccessory(JNIEnv *env, jobject /* this */,
                                                   jint accessoryId, jboolean buy) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return JNI_FALSE;

    ComsBuyAccessoryMessage message((unsigned int) tank->getPlayerId(),
                                     (unsigned int) accessoryId, buy == JNI_TRUE);
    if (g_mode == EngineMode::kHost) {
        TankAccessorySimAction *simAction = new TankAccessorySimAction(message);
        ScorchedServer::instance()->getServerSimulator().addSimulatorAction(simAction);
        return JNI_TRUE;
    }
    return g_clientContext->sendGameMessage(message) ? JNI_TRUE : JNI_FALSE;
}

// M6 parity: activates a defense accessory for "my tank" - raising/lowering
// a shield, enabling/disabling parachutes, or using a battery (repair).
// [change] matches ComsDefenseMessage::DefenseChange (1=shieldUp,
// 2=shieldDown, 3=parachutesUp, 4=parachutesDown, 5=batteryUse) and
// [accessoryId] is the message's infoId - which shield/parachute/battery to
// act on (unused for the *Down cases, pass 0).
//
// Same construct-the-real-message-then-queue-or-send pattern as
// fireWeapon()/buyAccessory() above: host mode queues the
// TankDefenseSimAction directly in-process, client mode sends the message
// for the real host's ServerDefenseHandler to handle. Either way
// TankDefenseSimAction::invokeAction() re-validates (tank alive/playing,
// battery available, accessory exists), so an invalid request is a no-op
// rather than a crash - this only reports whether it was queued/sent.
//
// Before M6 none of this was reachable at all: the shop filtered to weapons
// only, so shields/parachutes/batteries could be neither bought nor used -
// see the getWeaponShop() comment above and the porting plan's M6 audit.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_useDefense(JNIEnv *env, jobject /* this */,
                                                 jint accessoryId, jint change) {
    if (change < ComsDefenseMessage::eShieldUp || change > ComsDefenseMessage::eBatteryUse) {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return JNI_FALSE;

    ComsDefenseMessage message((unsigned int) tank->getPlayerId(),
                               (ComsDefenseMessage::DefenseChange) change,
                               (unsigned int) accessoryId);
    if (g_mode == EngineMode::kHost) {
        TankDefenseSimAction *simAction = new TankDefenseSimAction(message);
        ScorchedServer::instance()->getServerSimulator().addSimulatorAction(simAction);
        return JNI_TRUE;
    }
    return g_clientContext->sendGameMessage(message) ? JNI_TRUE : JNI_FALSE;
}

// M6 parity: "my tank"'s currently-active defenses, as
// "shieldName|parachuteName" (either side empty if none active) - lets the
// HUD/defense dialog show what's already up rather than making the player
// guess. Shield/parachute state lives on Target, not TanketAccessories (see
// Target::getShield()/getParachute()).
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getActiveDefenses(JNIEnv *env, jobject /* this */) {
    std::string result;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        Tank *tank = findMyTank();
        if (tank) {
            Accessory *shield = tank->getShield().getCurrentShield();
            Accessory *parachute = tank->getParachute().getCurrentParachute();
            result += shield ? shield->getName() : "";
            result += '|';
            result += parachute ? parachute->getName() : "";
        }
    }
    return env->NewStringUTF(result.c_str());
}

// M6 parity: current wind, as "speed|angleDegrees" (empty if no context
// yet). Wind really does perturb shots (see TankLib.cpp's windoffsetFB,
// which dot-products the shot direction against Wind::getWindDirection()),
// so with no indicator at all a player had no way to account for it - one
// of the gaps found in the M6 control audit (upstream binds a wind dialog
// to SHOW_WIND_DIALOG).
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getWindInfo(JNIEnv *env, jobject /* this */) {
    std::string result;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        if (ctx) {
            Wind &wind = ctx->getSimulator().getWind();
            std::ostringstream out;
            out << wind.getWindSpeed().asFloat() << '|' << wind.getWindAngle().asFloat();
            result = out.str();
        }
    }
    return env->NewStringUTF(result.c_str());
}

// M4 economy: selects the given accessory as "my tank"'s current weapon
// (see TanketWeapon::setWeapon) - a plain synchronous state change, not a
// simulator action, since it only affects local selection state (matches
// upstream's own client-side weapon-select UI, which calls this directly
// too, in either role - no network round-trip needed here). Fails (returns
// false) if the tank doesn't own/can't use that accessory - see
// TanketAccessories::canUse().
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_selectWeapon(JNIEnv *env, jobject /* this */, jint accessoryId) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    ScorchedContext *ctx = activeContext();
    Tank *tank = findMyTank();
    if (!ctx || !tank) return JNI_FALSE;

    Accessory *accessory = ctx->getAccessoryStore().findByAccessoryId((unsigned int) accessoryId);
    if (!accessory) return JNI_FALSE;

    return tank->getAccessories().getWeapons().setWeapon(accessory) ? JNI_TRUE : JNI_FALSE;
}

// M4 weapon HUD: "my tank"'s current weapon name (see TanketWeapon::
// getCurrent), or "" if not set/tank not found yet - a lighter-weight query
// than getWeaponShop() for a value the HUD wants to poll every tick, since
// it skips walking the whole AccessoryStore.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getCurrentWeaponName(JNIEnv *env, jobject /* this */) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        Tank *tank = findMyTank();
        if (tank) {
            Accessory *current = tank->getAccessories().getWeapons().getCurrent();
            if (current) {
                // M5: ammo count alongside the name - "how many do I have
                // left" was previously only visible inside the Shop
                // dialog's row list, not on the HUD button a player
                // actually looks at while deciding whether to fire.
                // getAccessoryCount() returns -1 for "unlimited" (see
                // TanketAccessories.cpp).
                int count = tank->getAccessories().getAccessoryCount(current);
                char buffer[128];
                if (count < 0) {
                    snprintf(buffer, sizeof(buffer), "%s", current->getName());
                } else {
                    snprintf(buffer, sizeof(buffer), "%s (%d)", current->getName(), count);
                }
                name = buffer;
            }
        }
    }
    return env->NewStringUTF(name.c_str());
}

// M5: a plain-language phase/turn indicator for the status line - this
// engine has no strict turn order in ScorchDroid's own config
// (TurnType=TurnSimultaneous, see scorchdroid_server.xml/
// dedicated_server.xml), so "when it is my turn" really means "is my tank
// currently able to act", which maps directly onto its own TankState (see
// TankState::getSmallStateString()) - the same field engine_jni.cpp's own
// debug string already surfaced as a raw label, just never translated for
// a real player. Returns "" if this process has no tank of its own yet
// (not started, or not yet added to the target container).
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getMyStatusLabel(JNIEnv *env, jobject /* this */) {
    std::string label;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        Tank *tank = findMyTank();
        if (tank) {
            const char *state = tank->getState().getSmallStateString();
            if (0 == strcmp(state, "Loading")) label = "Joining game...";
            else if (0 == strcmp(state, "Spectator")) label = "Spectating";
            else if (0 == strcmp(state, "Buying")) label = "Buying phase - visit the Shop";
            else if (0 == strcmp(state, "Alive")) label = "Fire when ready!";
            else if (0 == strcmp(state, "Dead")) label = "You're out - waiting for next round";
            else label = state;
        }
    }
    return env->NewStringUTF(label.c_str());
}

// M6 HUD: how long is left in the current phase. The server states time
// themselves out internally (ServerStateBuying::totalTime_ vs BuyingTime,
// ServerStatePlaying's shot timer) but keep those counters protected with
// no accessor, and upstream's client learns the deadline a different way
// again - through the timeout carried on the move messages it receives.
//
// Rather than patch the submodule to expose a counter, this mirrors the
// accumulation here: the tick loop already knows the same frameTime the
// server state machine is given, so adding it up alongside and comparing
// against the configured duration tracks the real timer to within a tick.
// Reset whenever the state changes, which is the only event that matters.
static int g_lastServerState = -1;
static unsigned int g_lastMoveId = 0;
static fixed g_phaseElapsed(0);

static void trackPhaseTime(ScorchedServer *server, fixed frameTime)
{
    const int state = (int) server->getServerState().getState();

    // The shot clock is per *move*, not per playing-phase: ServerTurns::
    // playMove arms a timeout of getShotTime() each time it hands a tanket
    // a move, and the playing state spans many of them. Timing from the
    // state change alone counted down once and then sat at zero for the
    // rest of the round, which is what it looked like on device.
    Tank *tank = findMyTank();
    const unsigned int moveId = tank ? tank->getShotInfo().getMoveId() : 0;

    if (state != g_lastServerState || moveId != g_lastMoveId) {
        g_lastServerState = state;
        g_lastMoveId = moveId;
        g_phaseElapsed = 0;
    } else {
        g_phaseElapsed += frameTime;
    }
}

// M6 tap-to-aim (upstream's AUTO_AIM, "Aim at point"). Given a landscape
// point - from the renderer's terrain pick - swing "my tank"'s turret to
// face it and report the resulting angle so the HUD dial can follow.
//
// The angle is upstream's own arithmetic, lifted verbatim from
// TankKeyboardControlUtil::autoAim:
//     angle = degrees(atan2(dir.y, dir.x)) - 90
// rather than re-derived. Angle conventions in this port have been got
// wrong several ways by reasoning from first principles; copying the line
// upstream uses against the same unmodified engine is the cheaper
// correctness argument.
//
// Returns the new angle in degrees, or -1 if there is no tank to aim.
extern "C" JNIEXPORT jfloat JNICALL
Java_com_rm_scorchdroid_NativeBridge_aimAtPoint(JNIEnv *, jobject, jfloat landscapeX, jfloat landscapeY) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank || !tank->getAlive()) return -1.0f;

    FixedVector &position = tank->getLife().getTargetPosition();
    const float dirX = landscapeX - position[0].asFloat();
    const float dirY = landscapeY - position[1].asFloat();
    if (fabsf(dirX) < 0.001f && fabsf(dirY) < 0.001f) return -1.0f;

    // Upstream's own sign, unaltered. It is consistent with the engine by
    // construction: TankLib::getVelocityVector fires along
    // (-sin(xy), cos(xy)), and `degrees(atan2(dy, dx)) - 90` is exactly
    // `degrees(atan2(-dx, dy))`, its inverse.
    //
    // This briefly carried a `+ 90` instead, because on device it aimed the
    // turret away from the tapped point. That was not an angle-convention
    // divergence at all: the renderer's pick ray was built from a reversed
    // camera right/up basis, so the tap resolved to the landscape point
    // opposite the one under the finger. Fixed at source in renderer_jni's
    // PickCamera publish; do not re-add a turn here.
    float angle = (float) (atan2((double) dirY, (double) dirX) * 180.0 / M_PI) - 90.0f;
    angle = fmodf(fmodf(angle, 360.0f) + 360.0f, 360.0f);

    tank->getShotInfo().rotateGunXY(fixed::fromFloat(angle), false);
    return angle;
}

// M6 tank movement (and every other "click the ground to use it" weapon).
//
// Upstream has no separate move action: moving is firing a WeaponMoveTank
// accessory - Fuel, Rocket Fuel - as an ordinary eShot whose selected
// landscape position is the destination (see TankAICurrentMove::
// makeMoveShot for the bot doing exactly this, and WeaponSelectPosition::
// fireWeapon, which swaps the shot's start position for the selected one
// before handing off to the aimed weapon). The server-side action,
// TanketMovement, then re-runs MovementMap itself and walks the tank there
// a square at a time, spending one fuel per square.
//
// So the whole feature is a property of the *current weapon*, exactly as
// upstream models it: an accessory whose getPositionSelect() is not
// ePositionSelectNone turns the battlefield tap from "aim" into "choose a
// destination", and the normal fire button stops working while it is
// selected (TankKeyboardControlUtil::keyboardCheck refuses the fire key for
// the same reason). Nothing here is movement-specific - Teleport works
// through the same path, which is why this is named for position select
// rather than for movement.

// The current weapon if it needs a landscape position, else nullptr.
static Accessory *currentPositionSelectWeapon(Tank *tank) {
    if (!tank) return nullptr;
    Accessory *weapon = tank->getAccessories().getWeapons().getCurrent();
    if (!weapon) return nullptr;
    if (weapon->getPositionSelect() == Accessory::ePositionSelectNone) return nullptr;
    return weapon;
}

// How far the tank may travel with [weapon] selected. Fuel spends one unit
// per square and is capped by the weapon's own maximum range (upstream's
// MovementMap::getFuel); the limit variants carry a fixed distance instead.
static fixed positionSelectRange(ScorchedContext &ctx, Tank *tank, Accessory *weapon) {
    switch (weapon->getPositionSelect()) {
        case Accessory::ePositionSelectFuel: {
            WeaponMoveTank *moveWeapon = (WeaponMoveTank *)
                ctx.getAccessoryStore().findAccessoryPartByAccessoryId(
                    weapon->getAccessoryId(), "WeaponMoveTank");
            if (!moveWeapon) return fixed(0);
            MovementMap map(tank, ctx);
            return map.getFuel(moveWeapon);
        }
        case Accessory::ePositionSelectFuelLimit:
        case Accessory::ePositionSelectLimit:
            return fixed(weapon->getPositionSelectLimit());
        default:
            return fixed(0);
    }
}

// Can this tank actually use [weapon] on that square? Upstream asks exactly
// this before firing (TargetCamera::landIntersect): a flood fill limited by
// the fuel for the two fuel types, a plain distance for the limit type.
static bool positionSelectAllowed(ScorchedContext &ctx, Tank *tank, Accessory *weapon,
                                  int posX, int posY) {
    GroundMaps &ground = ctx.getLandscapeMaps().getGroundMaps();
    const int arenaX = ground.getArenaX(), arenaY = ground.getArenaY();
    const int arenaW = ground.getArenaWidth(), arenaH = ground.getArenaHeight();
    if (posX <= arenaX || posX >= arenaX + arenaW ||
        posY <= arenaY || posY >= arenaY + arenaH) {
        return false;
    }

    const Accessory::PositionSelectType type = weapon->getPositionSelect();
    if (type == Accessory::ePositionSelectLimit) {
        FixedVector target(fixed(posX), fixed(posY), fixed(0));
        return (tank->getLife().getTargetPosition() - target).Magnitude() <=
               fixed(weapon->getPositionSelectLimit());
    }
    if (type == Accessory::ePositionSelectGeneric) return true;

    fixed range = positionSelectRange(ctx, tank, weapon);
    MovementMap map(tank, ctx);
    FixedVector target(fixed(posX), fixed(posY), fixed(0));
    map.calculatePosition(target, range);
    MovementMap::MovementMapEntry &entry = map.getEntry(posX, posY);

    // The distance test is ours, and it matters. Upstream checks the type
    // alone, but calculatePosition's fuel limit gates *expansion*, not
    // insertion: squares past the range still get marked eMovement on the
    // way through the queue, so the type by itself is not a range check.
    // On a touch screen that shows up immediately - an oblique camera turns
    // a tap near the top of the screen into a point most of a map away, and
    // it was accepted. Checking the path distance as well makes what a tap
    // accepts agree with the area painted on the ground, which is the only
    // thing the player can see.
    return entry.type == MovementMap::eMovement && entry.dist <= range;
}

// Keeps ScorchDroidMovement's published mask in step with the current
// weapon, for the renderer to paint over the ground. Called every tick from
// tickEngine() with the engine lock already held.
//
// Upstream does this once, in TankWeaponSwitcher::switchWeapon - but that
// whole function is inside `#ifndef S3D_SERVER`, so in this build it is an
// empty stub, and hooking it would mean a submodule patch for something the
// simulation side can work out for itself. Recomputing on a change of
// weapon, tank position or fuel count covers every case that hook would
// have, and a few it wouldn't (buying more fuel mid-phase widens the area
// immediately).
static void refreshMovementMask(ScorchedContext &ctx, Tank *tank) {
    static unsigned int lastWeaponId = 0;
    static unsigned int lastLandscape = 0xffffffffu;
    static int lastTankX = -1, lastTankY = -1;
    static int lastCount = -2;

    Accessory *weapon = currentPositionSelectWeapon(tank);
    if (!weapon || !tank->getAlive()) {
        ScorchDroidMovement::clear();
        lastWeaponId = 0;
        return;
    }

    // Never while the tank is actually being walked somewhere: its position
    // changes every tick, so this would re-run the flood fill every frame
    // of the move, and the answer is about to be stale anyway.
    if (tank->getTargetState().getMoving()) return;

    FixedVector &pos = tank->getLife().getTargetPosition();
    const int tankX = pos[0].asInt(), tankY = pos[1].asInt();
    const int count = tank->getAccessories().getAccessoryCount(weapon);
    const unsigned int landscape =
        ctx.getLandscapeMaps().getDefinitions().getDefinition().getDefinitionNumber();
    if (weapon->getAccessoryId() == lastWeaponId && landscape == lastLandscape &&
        tankX == lastTankX && tankY == lastTankY && count == lastCount) {
        return;
    }
    lastWeaponId = weapon->getAccessoryId();
    lastLandscape = landscape;
    lastTankX = tankX; lastTankY = tankY; lastCount = count;

    GroundMaps &ground = ctx.getLandscapeMaps().getGroundMaps();
    const int width = ground.getLandscapeWidth();
    const int height = ground.getLandscapeHeight();
    if (width <= 0 || height <= 0) return;

    std::vector<unsigned char> reachable((size_t) width * height, 0);
    // Not const: fixed::asFloat() isn't a const member.
    fixed range = positionSelectRange(ctx, tank, weapon);

    if (weapon->getPositionSelect() == Accessory::ePositionSelectLimit) {
        // A plain radius, which is what upstream's MovementMap::limitTexture
        // draws for this type - no pathfinding, so terrain doesn't matter.
        const float limit = range.asFloat();
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                const float dx = (float) x - (float) tankX;
                const float dy = (float) y - (float) tankY;
                if (dx * dx + dy * dy <= limit * limit) {
                    reachable[(size_t) y * width + x] = 1;
                }
            }
        }
    } else {
        MovementMap map(tank, ctx);
        map.calculateAllPositions(range);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                if (map.getEntry(x, y).type == MovementMap::eMovement) {
                    reachable[(size_t) y * width + x] = 1;
                }
            }
        }
    }

    int reachCount = 0, minX = width, minY = height, maxX = -1, maxY = -1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (!reachable[(size_t) y * width + x]) continue;
            reachCount++;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
    }
    LOGI("Movement mask: weapon=%s range=%.0f tank=(%d,%d) reachable=%d box=[%d,%d]-[%d,%d]",
         weapon->getName(), range.asFloat(), tankX, tankY, reachCount, minX, minY, maxX, maxY);
    ScorchDroidMovement::publish(width, height, std::move(reachable));
}

// M6: the current weapon's position-select mode, for the HUD, as
// "type|weaponName|range" - or "" when the current weapon is an ordinary
// one and the battlefield tap should keep aiming. [type] is upstream's own
// name for it (fuel / fuellimit / limit / generic).
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getPositionSelect(JNIEnv *env, jobject /* this */) {
    std::string result;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        Tank *tank = findMyTank();
        Accessory *weapon = currentPositionSelectWeapon(tank);
        if (ctx && tank && weapon) {
            const char *typeName = "generic";
            switch (weapon->getPositionSelect()) {
                case Accessory::ePositionSelectFuel:      typeName = "fuel"; break;
                case Accessory::ePositionSelectFuelLimit: typeName = "fuellimit"; break;
                case Accessory::ePositionSelectLimit:     typeName = "limit"; break;
                default: break;
            }
            std::ostringstream out;
            out << typeName << '|' << weapon->getName() << '|'
                << positionSelectRange(*ctx, tank, weapon).asFloat();
            result = out.str();
        }
    }
    return env->NewStringUTF(result.c_str());
}

// M6: uses the current position-select weapon on a landscape point - moving
// the tank there, for the fuel weapons. Returns false if the point is out
// of reach, which is what upstream's click handler does too (it simply
// returns and nothing happens); the HUD says so rather than leaving the tap
// looking ignored.
//
// The message is the same eShot every other weapon sends, carrying the
// selected position - see the block comment above. Aim and power go along
// unchanged because the engine records them for everyone else's benefit.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_firePositionSelect(JNIEnv *env, jobject /* this */,
                                                        jfloat landscapeX, jfloat landscapeY) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    ScorchedContext *ctx = activeContext();
    Tank *tank = findMyTank();
    if (!ctx || !tank || !tank->getAlive()) return JNI_FALSE;

    Accessory *weapon = currentPositionSelectWeapon(tank);
    if (!weapon) return JNI_FALSE;

    const int posX = (int) landscapeX, posY = (int) landscapeY;
    if (!positionSelectAllowed(*ctx, tank, weapon, posX, posY)) {
        FixedVector &at = tank->getLife().getTargetPosition();
        LOGI("firePositionSelect: (%d,%d) out of reach for %s from (%.0f,%.0f)",
             posX, posY, weapon->getName(), at[0].asFloat(), at[1].asFloat());
        return JNI_FALSE;
    }

    tank->getShotInfo().setSelectPosition(posX, posY);

    ComsPlayedMoveMessage message((unsigned int) tank->getPlayerId(),
                                  tank->getShotInfo().getMoveId(),
                                  ComsPlayedMoveMessage::eShot);
    message.setShot(
        weapon->getAccessoryId(),
        tank->getShotInfo().getRotationGunXY(),
        tank->getShotInfo().getRotationGunYZ(),
        tank->getShotInfo().getPower(),
        posX, posY
    );

    bool ok;
    if (g_mode == EngineMode::kHost) {
        ScorchedServer::instance()->getServerState().moveFinished(message);
        ok = true;
    } else {
        ok = g_clientContext->sendGameMessage(message);
    }
    FixedVector &from = tank->getLife().getTargetPosition();
    LOGI("firePositionSelect: player=%u weapon=%s from=(%.0f,%.0f) to=(%d,%d) ok=%d",
         tank->getPlayerId(), weapon->getName(),
         from[0].asFloat(), from[1].asFloat(), posX, posY, ok);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// M6 HUD: the move id the server has currently granted "my tank", or 0 if
// none is outstanding. ServerTurns::playMove sets it when a tanket is given
// a move and playMoveFinished clears it once that move is submitted, so a
// live tank with 0 here has locked its shot in and is waiting on everyone
// else - which is exactly what the Fire button wants to show.
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getMyMoveId(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    return tank ? (jint) tank->getShotInfo().getMoveId() : 0;
}

// M6 HUD: seconds left in the current timed phase, or -1 where a countdown
// would be meaningless (no game yet, joined as a client - the host owns the
// clock there and does not send it - or a phase that ends on an event
// rather than a deadline, like waiting for players).
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getPhaseSecondsRemaining(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kHost || !ScorchedServer::serverStarted()) return -1;

    ScorchedServer *server = ScorchedServer::instance();
    int duration = 0;
    switch (server->getServerState().getState()) {
    case ServerState::ServerBuyingState:
        duration = server->getOptionsGame().getBuyingTime();
        break;
    case ServerState::ServerPlayingState:
        duration = server->getOptionsGame().getShotTime();
        break;
    default:
        return -1;
    }
    // A duration of 0 means "no limit" in upstream's options, not "already
    // expired" - see ServerStateBuying::simulate, which skips its whole
    // timeout branch in that case.
    if (duration <= 0) return -1;

    const int remaining = duration - g_phaseElapsed.asInt();
    return remaining > 0 ? remaining : 0;
}

static Clock tickClock;

// M6 parity: in-game chat, host side.
//
// This needs no patch, unlike the effect/sound/deform hooks, because chat
// never goes through a client-only drawing path. Typed chat reaches
// ServerChannelManager::sendText(), which is ordinary src/server code this
// build compiles, and which already keeps its own rolling log of the last
// 25 messages for upstream's server console - each with a monotonically
// increasing id. Polling that is the whole hosting-side implementation.
//
// (ChannelManager::showText() - the other obvious hook - is *not* the right
// place: it carries the sim-action notices like "player joined" and, in this
// build, only reaches the Logger. Typed chat does not go through it at all.)
//
// The entries are pre-formatted for that console as
//   [channel][name] : "message"
// so they are unpicked back into their parts here rather than shown raw,
// which is what lets the HUD colour by channel and tell a player's line from
// the server's own.
static unsigned int g_lastChatMessageId = 0;

static void pollServerChat(ScorchedServer *server) {
    std::list<ServerChannelManager::MessageEntry> &messages =
        server->getServerChannelManager().getLastMessages();
    for (auto &entry : messages) {
        if (entry.messageid <= g_lastChatMessageId) continue;
        g_lastChatMessageId = entry.messageid;

        ScorchDroidChat::Line line;
        line.text = entry.message;

        // "[channel][name] : "message"" or "[channel] : "message"" when the
        // server itself is speaking. Parsed defensively - a channel filter
        // or a mod could produce something else, and an unrecognised line is
        // better shown whole than dropped.
        const std::string &raw = entry.message;
        size_t open = raw.find('[');
        size_t close = raw.find(']');
        if (open == 0 && close != std::string::npos) {
            line.channel = raw.substr(1, close - 1);
            size_t rest = close + 1;
            if (rest < raw.size() && raw[rest] == '[') {
                size_t nameEnd = raw.find(']', rest);
                if (nameEnd != std::string::npos) {
                    line.who = raw.substr(rest + 1, nameEnd - rest - 1);
                    rest = nameEnd + 1;
                }
            }
            // Drop the ` : "` and the trailing quote the console format adds.
            size_t quote = raw.find('"', rest);
            if (quote != std::string::npos) {
                size_t endQuote = raw.rfind('"');
                line.text = (endQuote > quote)
                    ? raw.substr(quote + 1, endQuote - quote - 1)
                    : raw.substr(quote + 1);
            }
        }
        ScorchDroidChat::push(line);
    }
}

// Drives the real game simulation forward. Host mode replicates
// ServerMain.cpp's serverLoop() (excluded from this build - see the
// porting plan - because it also pulls in the deferred UDP-based
// ServerBrowserInfo/ServerWebServer at the top of that same file) -
// serverLoop() itself is what actually advances ServerState (waiting for
// players -> new level -> buying -> playing), not Simulator::simulate()
// alone, so all of these calls are needed, not just the simulator step.
// Client mode just pumps ClientContext::tick() (see ClientContext.hpp),
// which internally does the client-side equivalent (advance the handshake,
// then simulate once joined) - the host on the other end of the socket is
// the one running the ServerState machine above.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_tickEngine(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);

    if (g_mode == EngineMode::kClient) {
        Logger::instance()->processLogEntries();
        if (g_clientContext) g_clientContext->tick();
        if (ScorchedContext *ctx = activeContext()) refreshMovementMask(*ctx, findMyTank());
        return;
    }

    if (g_mode != EngineMode::kHost ||
        !ScorchedServer::serverStarted() || !ScorchedServer::instance()->getContext().getNetInterfaceValid()) {
        return;
    }

    // ServerMain.cpp: fixed timeDifference(true, ticksDifference * 10) -
    // FIXED_RESOLUTION represents 1.0 == 1 second, so ms * 10 == ms/1000 * FIXED_RESOLUTION.
    unsigned int ticksDifference = tickClock.getTicksDifference();
    fixed timeDifference(true, ((Sint64) ticksDifference) * 10);

    Logger::instance()->processLogEntries();

    ScorchedServer *server = ScorchedServer::instance();
    server->getNetInterface().processMessages();
    server->getSimulator().simulate();
    server->getServerState().simulate(timeDifference);
    trackPhaseTime(server, timeDifference);

    // Must run after getServerState().simulate() above, not before: on the
    // very first tick, that call is what runs ServerStartupState's own
    // newGame() pass (see ServerStateNewGame.cpp), which resets *any* tank
    // it finds not already TankState::sLoading back to sLoading - it's
    // designed to restart a level for tanks left over from a previous
    // round, but it can't tell "leftover from a previous round" apart from
    // "we just promoted this tank a moment ago on this same tick", so
    // promoting first just gets immediately clobbered. Running after means
    // this tick's newGame() (if any) sees our tank still genuinely
    // sLoading, its actual state at that point, and leaves it alone -
    // found the hard way watching TankChangeSimAction's queued promotion
    // silently no-op because the tank was back to sLoading by the time it
    // executed.
    if (!g_humanPromoted) {
        Tank *tank = findMyTank();
        if (tank) {
            promoteHumanToPlaying(server, tank);
            g_humanPromoted = true;
        }
    }

    pollServerChat(server);

    // ServerStateNewGame::newGame() (see ServerStateNewGame.cpp) resets
    // *any* tank it finds not already TankState::sLoading back to sLoading
    // at the start of every round (not just server startup) and calls
    // ServerLoadLevel::destinationLoadLevel() for it - self-resolving
    // instantly for a bot (destinationId==0) but, for our human tank's
    // fake non-zero destination, sending a real ComsLoadLevelMessage that
    // no real client will ever answer, leaving it stuck at sLoading (never
    // sBuying/sNormal, never getAlive()) for the rest of the round - found
    // the hard way watching a full match play out (with real damage/a real
    // winner) while the human tank simply never got to do anything.
    // Mirrors destinationId==0's own self-resolution manually, via a small
    // access-modifier patch exposing ServerLoadLevel::setLoaded() (see
    // patches/scorched3d/0008-...) - g_humanLoadPending guards against
    // queuing a redundant TankLoadedSimAction every tick while one is
    // already in flight, which would otherwise force even an actively
    // sBuying/sNormal tank back to sDead (sNormal->sDead is an allowed
    // TankState transition).
    {
        Tank *tank = findMyTank();
        if (tank) {
            bool loading = tank->getState().getState() == TankState::sLoading;
            if (loading && !g_humanLoadPending) {
                server->getServerLoadLevel().setLoaded(kHumanDestinationId);
                g_humanLoadPending = true;
            } else if (!loading) {
                g_humanLoadPending = false;
            }
        }
    }

    server->getServerConnectAuthHandler().processMessages();
    server->getServerFileServer().simulate();
    server->getServerChannelManager().simulate(timeDifference);
    server->getTimedMessage().simulate();

    // Where "my tank" may move to, for the renderer to paint over the
    // ground - cheap unless the answer actually changed.
    refreshMovementMask(server->getContext(), findMyTank());
}


// M2 touch-fire, M5 Phase 2: submits a move for the given tank, exactly as
// a real client's ComsPlayedMoveMessage would (see
// ServerPlayedMoveHandler.cpp / ServerTurnsSimultaneous::
// internalMoveFinished). Host mode dispatches it directly in-process
// (still standing in for a real client connecting over NetLoopBack, as
// before); client mode sends the same message to the real host over the
// real socket (see ClientContext::sendGameMessage()) and waits for the
// resulting shot to come back like anyone else's, via ComsSimulateMessage -
// ServerPlayedMoveHandler.cpp was already wired up server-side since M1,
// so nothing server-side needed to change for this. angle/elevation are in
// degrees, power is 0..1 - all converted to the engine's fixed-point types
// here so the touch layer only ever deals in plain floats.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_fireWeapon(JNIEnv *env, jobject /* this */,
                                                 jint playerId, jfloat angleDegrees,
                                                 jfloat elevationDegrees, jfloat power) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    ScorchedContext *ctx = activeContext();
    if (!ctx) return JNI_FALSE;

    Tank *tank = ctx->getTargetContainer().getTankById((unsigned int) playerId);
    if (!tank || !tank->getAlive()) return JNI_FALSE;

    Accessory *weapon = tank->getAccessories().getWeapons().getCurrent();
    if (!weapon) return JNI_FALSE;

    unsigned int moveId = tank->getShotInfo().getMoveId();
    // "power" here is the touch layer's 0..1 fraction (drag distance, or a
    // fixed value for a plain tap) - the engine's own power scale is
    // nothing like 0..1 (TanketShotInfo::power_ defaults to 1000, and
    // tanktypes.xml's default tank type sets a max of 1000 too; velocity
    // itself is computed as `baseVelocity * (power + 1)` - see
    // PlayMovesSimAction::tankFired()), so sending the raw 0..1 fraction
    // as "power" directly asks for essentially no thrust at all: every
    // shot landed right on top of the firing tank, never visibly
    // traveling anywhere. Scale by this tank's actual max power so 1.0
    // means "full power for this tank", matching what the drag gesture's
    // distance is meant to represent.
    fixed maxPower = tank->getShotInfo().getMaxPower();
    ComsPlayedMoveMessage message((unsigned int) playerId, moveId, ComsPlayedMoveMessage::eShot);
    message.setShot(
        weapon->getAccessoryId(),
        fixed::fromFloat(angleDegrees),
        fixed::fromFloat(elevationDegrees),
        fixed::fromFloat(power) * maxPower,
        0, 0
    );

    bool ok;
    if (g_mode == EngineMode::kHost) {
        ScorchedServer::instance()->getServerState().moveFinished(message);
        ok = true;
    } else {
        ok = g_clientContext->sendGameMessage(message);
    }
    // The firing position is logged alongside the angle because the only
    // way to check aim against the world is to compare where the shot
    // actually landed (the renderer's "Terrain deformed" line) with the
    // direction it was sent in - and both tanks fire in the same phase, so
    // without knowing which crater started where, the bot's is easy to
    // mistake for your own. See the aim-direction verification notes.
    FixedVector &firedFrom = tank->getLife().getTargetPosition();
    LOGI("fireWeapon: player=%d weapon=%u from=(%.1f,%.1f) angle=%.1f elevation=%.1f power=%.2f ok=%d",
         playerId, weapon->getAccessoryId(), firedFrom[0].asFloat(), firedFrom[1].asFloat(),
         angleDegrees, elevationDegrees, power, ok);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// M6: "my tank"'s current aim as "angleDegrees|elevationDegrees|powerFraction"
// (empty if there's no tank yet), straight from TanketShotInfo. Used to
// seed the aiming sliders at the start of a game so they show where the
// tank is actually pointing rather than an arbitrary 0 - the engine gives
// every tank a real starting turret rotation, and having the UI disagree
// with it meant the very first shot always went somewhere other than
// where the sliders claimed.
//
// The angle is returned in the engine's own convention (counterclockwise
// from world +Y); the Kotlin side mirrors it into the player-facing
// clockwise-from-up dial the same way fireFromSliders() mirrors it back.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getMyAim(JNIEnv *env, jobject /* this */) {
    std::string result;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        Tank *tank = findMyTank();
        if (tank) {
            TanketShotInfo &shotInfo = tank->getShotInfo();
            fixed maxPower = shotInfo.getMaxPower();
            float powerFraction = (maxPower > fixed(0))
                    ? (shotInfo.getPower() / maxPower).asFloat() : 0.5f;
            std::ostringstream out;
            out << shotInfo.getRotationGunXY().asFloat() << '|'
                << shotInfo.getRotationGunYZ().asFloat() << '|'
                << powerFraction;
            result = out.str();
        }
    }
    return env->NewStringUTF(result.c_str());
}

// M6: applies the aiming sliders to "my tank"'s actual turret state.
//
// Without this the sliders were purely a Kotlin-side value passed at fire
// time, so the tank's turret - and therefore the rendered gun and the aim
// sight, both of which read TanketShotInfo - never moved until a shot was
// actually fired. Upstream's own aiming controls do exactly this
// (TankKeyboardControlUtil rotates the gun as you adjust), which is what
// makes its sight track the player's aim.
//
// [angleDegrees] is the player-facing clockwise-from-up dial and is
// mirrored into the engine's counterclockwise-from-+Y convention here, the
// same way fireWeapon()'s caller does; power is the 0..1 fraction scaled by
// the tank's own max. Purely local turret state - the authoritative values
// still travel with the shot message when firing.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_setAim(JNIEnv *env, jobject /* this */,
                                             jfloat angleDegrees, jfloat elevationDegrees,
                                             jfloat power) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return JNI_FALSE;

    TanketShotInfo &shotInfo = tank->getShotInfo();
    shotInfo.rotateGunXY(fixed::fromFloat(angleDegrees), false);
    shotInfo.rotateGunYZ(fixed::fromFloat(elevationDegrees), false);
    shotInfo.changePower(fixed::fromFloat(power) * shotInfo.getMaxPower(), false);
    return JNI_TRUE;
}

// M6 parity: submits one of the non-shot player moves for "my tank" -
// [moveType] matches ComsPlayedMoveMessage::MoveType (2=resign, 3=skip,
// 4=finishedBuy). Upstream binds all three to its own UI
// (SHOW_RESIGN_DIALOG/SHOW_SKIP_DIALOG in data/keys.xml, and the buy
// screen's "done" button); ScorchDroid could only ever send eShot before
// M6, so a player could neither skip a turn, resign a round, nor end the
// buying phase early - they just had to wait out the timers.
//
// Same submission path as fireWeapon() above, including the moveId the
// server matches against, and the same host-vs-client split. The one
// difference is server-side routing: eFinishedBuy goes to
// buyingFinished() rather than moveFinished() (see
// ServerPlayedMoveHandler.cpp) - mirrored here for host mode, while
// client mode just sends the message and lets the real host's handler do
// that branching itself.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_submitMove(JNIEnv *env, jobject /* this */, jint moveType) {
    if (moveType != ComsPlayedMoveMessage::eResign &&
        moveType != ComsPlayedMoveMessage::eSkip &&
        moveType != ComsPlayedMoveMessage::eFinishedBuy) {
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return JNI_FALSE;

    unsigned int moveId = tank->getShotInfo().getMoveId();
    ComsPlayedMoveMessage message((unsigned int) tank->getPlayerId(), moveId,
                                  (ComsPlayedMoveMessage::MoveType) moveType);

    bool ok;
    if (g_mode == EngineMode::kHost) {
        if (moveType == ComsPlayedMoveMessage::eFinishedBuy) {
            ScorchedServer::instance()->getServerState().buyingFinished(message);
        } else {
            ScorchedServer::instance()->getServerState().moveFinished(message);
        }
        ok = true;
    } else {
        ok = g_clientContext->sendGameMessage(message);
    }
    LOGI("submitMove: player=%u type=%d ok=%d", tank->getPlayerId(), moveType, ok);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getGameStateDebugString(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    std::string result = "not started";
    char buffer[256];
    if (g_mode == EngineMode::kHost && ScorchedServer::serverStarted()) {
        ScorchedServer *server = ScorchedServer::instance();
        snprintf(buffer, sizeof(buffer), "state=%d tanks=%u",
                 (int) server->getServerState().getState(),
                 server->getTargetContainer().getNoOfTanks());
        result = buffer;
    } else if (g_mode == EngineMode::kClient && g_clientContext) {
        if (g_clientContext->getState() == ClientContext::sFailed) {
            snprintf(buffer, sizeof(buffer), "join failed: %s", g_clientContext->getFailureReason().c_str());
        } else {
            snprintf(buffer, sizeof(buffer), "client state=%d tanks=%u",
                     (int) g_clientContext->getState(),
                     g_clientContext->getTargetContainer().getNoOfTanks());
        }
        result = buffer;
    }
    return env->NewStringUTF(result.c_str());
}

// M3: drains sound events queued by SoundAction::simulate() (see
// SoundEventQueue.h) since the last call, returning each as an absolute
// file path the Kotlin side can hand straight to a MediaPlayer/SoundPool -
// no vendored OGG/Vorbis/Oboe needed, since Android's own media stack
// already decodes the .ogg files bundled in data/ (see the porting plan's
// note on the audio-approach revisit).
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_pollSoundEvents(JNIEnv *env, jobject /* this */) {
    std::vector<std::string> events;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        events = ScorchDroidAudio::drainSoundEvents();
    }

    jobjectArray result = env->NewObjectArray((jsize) events.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < events.size(); i++) {
        env->SetObjectArrayElement(result, (jsize) i, env->NewStringUTF(events[i].c_str()));
    }
    return result;
}

// M6 parity: the score / player list (upstream's SHOW_SCORE_DIALOG). Every
// number here is ordinary src/common state that this build already keeps -
// TankScore has score, kills, wins, money, rank, skill and ping - so this is
// purely a matter of reading it out; nothing is simulated or inferred.
//
// One pipe-delimited row per player, same convention as getWeaponShop():
//   "playerId|name|isBot|team|score|kills|wins|money|alive|ping|r,g,b|isMe"
// Sorted by score descending, which is the order the list is useful in.
// Upstream's own dialog also shows per-team totals and the round/turn
// counters; those come from getRoundInfo() below rather than being wedged
// into the same rows.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_getPlayerList(JNIEnv *env, jobject /* this */) {
    std::vector<std::string> rows;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        if (ctx) {
            Tank *myTank = findMyTank();
            const unsigned int myId = myTank ? myTank->getPlayerId() : 0;

            std::vector<Tank *> sorted;
            std::map<unsigned int, Tank *> &tanks = ctx->getTargetContainer().getTanks();
            for (auto &entry : tanks) sorted.push_back(entry.second);
            std::sort(sorted.begin(), sorted.end(), [](Tank *a, Tank *b) {
                if (a->getScore().getScore() != b->getScore().getScore()) {
                    return a->getScore().getScore() > b->getScore().getScore();
                }
                // A stable tie-break, so the list doesn't reshuffle itself
                // between polls while several players sit on zero.
                return a->getPlayerId() < b->getPlayerId();
            });

            for (Tank *tank : sorted) {
                Vector &colour = tank->getColor();
                std::ostringstream row;
                row << tank->getPlayerId() << "|"
                    << tank->getCStrName() << "|"
                    << (tank->getTankAI() ? 1 : 0) << "|"
                    << tank->getTeam() << "|"
                    << tank->getScore().getScore() << "|"
                    << tank->getScore().getKills() << "|"
                    << tank->getScore().getWins() << "|"
                    << tank->getScore().getMoney() << "|"
                    // "Alive" as the HUD means it: in the game and not dead.
                    // TankState::sNormal alone isn't enough - a spectator is
                    // present but not playing.
                    << (tank->getState().getTankPlaying() ? 1 : 0) << "|"
                    << tank->getScore().getPing() << "|"
                    << (int) (colour[0] * 255.0f) << ","
                    << (int) (colour[1] * 255.0f) << ","
                    << (int) (colour[2] * 255.0f) << "|"
                    << (tank->getPlayerId() == myId ? 1 : 0);
                rows.push_back(row.str());
            }
        }
    }

    jobjectArray result = env->NewObjectArray((jsize) rows.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < rows.size(); i++) {
        env->SetObjectArrayElement(result, (jsize) i, env->NewStringUTF(rows[i].c_str()));
    }
    return result;
}

// The round/turn counters the score dialog heads itself with, as
// "round|totalRounds|turn|totalTurns". Upstream reads exactly these four off
// OptionsTransient and OptionsGame (see ScoreDialog.cpp).
// M9: end the current game and put the engine back where startLocalGame()
// and startJoinGame() will accept a new one.
//
// Until the main menu there was no such thing: the mode was chosen once at
// launch and the only way out was to kill the app, so both start functions
// simply refused a second call and nothing ever cleared g_mode. A menu makes
// "quit, start something else" ordinary, so this is the other half of that.
//
// host-tests' testServerRestart() is the evidence this is safe at all: it
// proves a second startServer() in the same process comes up with a genuinely
// live game, and - the part that actually decides it - that a real listening
// socket from the previous game is released, so hosting twice on one port
// works. It also pins the rule this function has to respect:
// ScorchedServer::instance() is null between the two, so nothing may hold a
// pointer across the gap.
//
// Everything below is state that outlives a round but must not outlive a
// *game*. The stores are the easy ones to forget: several are keyed by
// playerId, and the next game hands the same ids to entirely different tanks
// and targets, so a survivor doesn't look like stale data - it looks like the
// wrong model in the wrong place.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_stopGame(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode == EngineMode::kNone) return;   // idempotent: the menu may ask twice

    LOGI("stopGame: tearing down (mode=%d)", (int) g_mode);

    if (g_clientContext) {
        delete g_clientContext;
        g_clientContext = nullptr;
    }
    if (ScorchedServer::serverStarted()) {
        // Close the listening socket before dropping the server: stopServer()
        // deletes the NetInterface along with everything else, and a socket
        // freed only by its destructor is exactly the kind of thing that
        // leaves the next host unable to bind.
        ScorchedServer::instance()->getContext().getNetInterface().stop();
        ScorchedServer::stopServer();
    }

    g_mode = EngineMode::kNone;
    g_humanPromoted = false;
    g_humanLoadPending = false;
    g_hostingListening = false;
    g_hostingPort = 0;
    g_lastServerState = -1;
    g_lastMoveId = 0;
    g_phaseElapsed = fixed(0);
    g_lastChatMessageId = 0;

    ScorchDroidChat::clear();
    ScorchDroidTracer::clearAll();
    ScorchDroidMovement::clear();
    ScorchDroidTargets::clear();
    ScorchDroidScoreboard::hide();
    // Drained rather than cleared: these are queues with no clear() of their
    // own, and draining is exactly as complete.
    ScorchDroidEffects::drain();
    ScorchDroidAudio::drainSoundEvents();
    int a = 0, b = 0, c = 0, d = 0;
    ScorchDroidLandscape::takeDirtyRegion(a, b, c, d);

    LOGI("stopGame: engine is idle, ready for a new game");
}

// M10: the game setup screen's options.
//
// Rows are "name|kind|value|min|max|step|choices|description", where choices is
// a comma-separated list of "value=label" pairs for an enum and empty
// otherwise, and description is last so it may contain anything. The labels are
// upstream's own identifiers ("WallConcrete"); making them presentable is the
// UI's job, not this layer's.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_getSetupOptions(JNIEnv *env, jobject /* this */) {
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    std::vector<ScorchDroidSetup::Option> options = ScorchDroidSetup::options();

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray((jsize) options.size(), stringClass, nullptr);
    for (size_t i = 0; i < options.size(); i++) {
        const ScorchDroidSetup::Option &option = options[i];
        std::ostringstream choices;
        for (size_t c = 0; c < option.choices.size(); c++) {
            if (c > 0) choices << ",";
            choices << option.choices[c].value << "=" << option.choices[c].label;
        }
        std::ostringstream row;
        row << option.name << "|" << (int) option.kind << "|" << option.value << "|"
            << option.minValue << "|" << option.maxValue << "|" << option.stepValue << "|"
            << choices.str() << "|" << option.description;
        jstring value = env->NewStringUTF(row.str().c_str());
        env->SetObjectArrayElement(result, (jsize) i, value);
        env->DeleteLocalRef(value);
    }
    return result;
}

// False when the option isn't one the setup screen offers, or when upstream's
// own validation rejects the value - a bounded int outside its range, an enum
// value that isn't one of its choices. The UI should treat that as "upstream
// says no" rather than retrying.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_setSetupOption(
        JNIEnv *env, jobject /* this */, jstring jName, jstring jValue) {
    const char *nameChars = env->GetStringUTFChars(jName, nullptr);
    const char *valueChars = env->GetStringUTFChars(jValue, nullptr);
    std::string name(nameChars ? nameChars : "");
    std::string value(valueChars ? valueChars : "");
    if (nameChars) env->ReleaseStringUTFChars(jName, nameChars);
    if (valueChars) env->ReleaseStringUTFChars(jValue, valueChars);

    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    return ScorchDroidSetup::set(name, value) ? JNI_TRUE : JNI_FALSE;
}

// M12: loads a preset options file over the current setup - upstream's own
// tutorial configuration, in the only case that uses this. False if the file
// could not be read, in which case nothing changed.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_loadSetupPreset(
        JNIEnv *env, jobject /* this */, jstring jPath) {
    const char *pathChars = env->GetStringUTFChars(jPath, nullptr);
    std::string path(pathChars ? pathChars : "");
    if (pathChars) env->ReleaseStringUTFChars(jPath, pathChars);
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    return ScorchDroidSetup::loadPreset(path) ? JNI_TRUE : JNI_FALSE;
}

// The mods available to choose. "none" (upstream's base game) is always first;
// the rest are whatever directories sit in data/globalmods, so a mod dropped in
// alongside upstream's own appears with no code change.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_getAvailableMods(JNIEnv *env, jobject /* this */) {
    // "." is the data root: initEngine() chdir'd there, which is also why
    // every config path in this file is relative.
    std::vector<std::string> mods = ScorchDroidSetup::mods(".");
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray((jsize) mods.size(), stringClass, nullptr);
    for (size_t i = 0; i < mods.size(); i++) {
        jstring value = env->NewStringUTF(mods[i].c_str());
        env->SetObjectArrayElement(result, (jsize) i, value);
        env->DeleteLocalRef(value);
    }
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getSelectedMod(JNIEnv *env, jobject /* this */) {
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    return env->NewStringUTF(ScorchDroidSetup::mod().c_str());
}

// Not validated here: any string is a legal mod name as far as upstream's
// option is concerned, and a name with no directory behind it would fail at
// load time. The UI only offers names from getAvailableMods(), which is where
// the guarantee comes from.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_setSelectedMod(
        JNIEnv *env, jobject /* this */, jstring jName) {
    const char *nameChars = env->GetStringUTFChars(jName, nullptr);
    std::string name(nameChars ? nameChars : "none");
    if (nameChars) env->ReleaseStringUTFChars(jName, nameChars);
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    return ScorchDroidSetup::setMod(name) ? JNI_TRUE : JNI_FALSE;
}

// Back to what the shipped config says.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_resetSetupOptions(JNIEnv *env, jobject /* this */) {
    ScorchDroidSetup::ensureLoaded("scorchdroid_server.xml");
    ScorchDroidSetup::reset();
}

// M11: the name this player's tank carries, both when hosting and when
// joining someone else's game. Returns the name actually in force, which is
// the previous one if the given name was empty.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_setPlayerName(
        JNIEnv *env, jobject /* this */, jstring jName) {
    const char *nameChars = env->GetStringUTFChars(jName, nullptr);
    std::string name(nameChars ? nameChars : "");
    if (nameChars) env->ReleaseStringUTFChars(jName, nameChars);
    return env->NewStringUTF(ScorchDroidProfile::setName(name).c_str());
}

// The end-of-round scoreboard upstream raises by itself (ShowScoreAction,
// patch 0017). 0 = not showing, 1 = showing the round score, 2 = showing
// the final score of the match. The UI polls this on its existing tick.
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getScoreboardState(JNIEnv *env, jobject /* this */) {
    ScorchDroidScoreboard::State state = ScorchDroidScoreboard::get();
    if (!state.showing) return 0;
    return state.finalScore ? 2 : 1;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getRoundInfo(JNIEnv *env, jobject /* this */) {
    std::ostringstream out;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        if (!ctx) return env->NewStringUTF("");
        out << ctx->getOptionsTransient().getCurrentRoundNo() << "|"
            << ctx->getOptionsGame().getNoRounds() << "|"
            << ctx->getOptionsTransient().getCurrentTurnNo() << "|"
            << ctx->getOptionsGame().getNoTurns();
    }
    return env->NewStringUTF(out.str().c_str());
}

// M6 parity: send a chat message on a channel ("general" or "team" - the two
// upstream exposes to a player; the rest are read-only or admin).
//
// The two modes take genuinely different routes, because a host has no
// socket to itself:
//  - Hosting, this is a direct ServerChannelManager::sendText(), the same
//    call the server makes when it receives a client's message. That both
//    logs it (so pollServerChat above picks it up and the sender sees their
//    own line) and fans it out to every subscribed client.
//  - Joined, it is a ComsChannelTextMessage to the host, which is exactly
//    what upstream's ClientChannelManager::sendText does - including setting
//    srcPlayerId, without which ServerChannelManager rejects the message as
//    not provably from this tank.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_sendChat(
        JNIEnv *env, jobject /* this */, jstring jChannel, jstring jText) {
    const char *channelChars = env->GetStringUTFChars(jChannel, nullptr);
    const char *textChars = env->GetStringUTFChars(jText, nullptr);
    std::string channel(channelChars ? channelChars : "general");
    std::string text(textChars ? textChars : "");
    if (channelChars) env->ReleaseStringUTFChars(jChannel, channelChars);
    if (textChars) env->ReleaseStringUTFChars(jText, textChars);

    if (text.empty()) return JNI_FALSE;

    std::lock_guard<std::mutex> lock(g_engineMutex);
    Tank *tank = findMyTank();
    if (!tank) return JNI_FALSE;

    ChannelText channelText(channel, LANG_STRING(text));
    channelText.setSrcPlayerId(tank->getPlayerId());

    if (g_mode == EngineMode::kHost) {
        if (!ScorchedServer::serverStarted()) return JNI_FALSE;
        ScorchedServer::instance()->getServerChannelManager().sendText(channelText, true);
        return JNI_TRUE;
    }

    if (g_mode == EngineMode::kClient && g_clientContext) {
        ComsChannelTextMessage message(channelText);
        // sendGameMessage rather than the private sendToServer: it also
        // refuses to send before the handshake has reached sJoined, which is
        // the right answer for chat typed while still connecting.
        if (!g_clientContext->sendGameMessage(message)) return JNI_FALSE;
        // Shown locally at once rather than waiting for the host to echo it
        // back: the round trip is a send boundary away, and a chat box that
        // appears to swallow what you typed reads as broken. If the host
        // rejects or filters it, the only cost is a line the others never
        // saw - the same trade the shop's optimistic "buying..." makes.
        ScorchDroidChat::Line line;
        line.channel = channel;
        line.who = tank->getCStrName();
        line.text = text;
        ScorchDroidChat::push(line);
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

// The chat log, oldest first, as "id|channel|who|text". Text is last so it
// may contain pipes without needing escaping.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_rm_scorchdroid_NativeBridge_getChatLines(
        JNIEnv *env, jobject /* this */, jint afterId) {
    std::vector<ScorchDroidChat::Line> lines =
        ScorchDroidChat::since((unsigned int) std::max(afterId, 0));

    jobjectArray result = env->NewObjectArray(
        (jsize) lines.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < lines.size(); i++) {
        std::ostringstream row;
        row << lines[i].id << "|" << lines[i].channel << "|"
            << lines[i].who << "|" << lines[i].text;
        env->SetObjectArrayElement(result, (jsize) i, env->NewStringUTF(row.str().c_str()));
    }
    return result;
}

// Bumped on every new chat line, so the HUD can poll one int rather than
// rebuilding the list every frame to discover nothing arrived.
extern "C" JNIEXPORT jint JNICALL
Java_com_rm_scorchdroid_NativeBridge_getChatVersion(JNIEnv *env, jobject /* this */) {
    return (jint) ScorchDroidChat::version();
}

// M6 parity: simulation speed (upstream's SIMULATION_SPEED_* keys - eighth,
// quarter, half, normal, x2, x4, x8). Simulator::setFast() is ordinary
// src/common state, so this is upstream's own mechanism, not a re-timing of
// the tick loop here.
//
// Host only, deliberately. Upstream's SpeedChange sets the *client*
// simulator's speed and only touches the server's when not connected to one
// (SpeedChange.cpp) - because in a real game the host sets the pace and a
// client that ran its own simulation faster would drift out of step with it.
// This port's joined client is a ClientSync slaved to the host's clock, so
// the same reasoning applies with more force: it returns false rather than
// desynchronising.
//
// speedNumerator/speedDenominator rather than a float, because the engine's
// clock is fixed-point: 1/8 has an exact fixed representation and 0.125f
// converted through a float does not necessarily land on it.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_setSimulationSpeed(
        JNIEnv *env, jobject /* this */, jint numerator, jint denominator) {
    if (numerator <= 0 || denominator <= 0) return JNI_FALSE;

    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kHost || !ScorchedServer::serverStarted()) return JNI_FALSE;

    ScorchedServer::instance()->getSimulator().setFast(fixed(numerator) / fixed(denominator));
    return JNI_TRUE;
}

// The current speed multiplier, as "numerator|denominator" - so the UI can
// show which setting is in effect without keeping its own copy that could
// drift from the engine's.
extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getSimulationSpeed(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    ScorchedContext *ctx = activeContext();
    if (!ctx) return env->NewStringUTF("1|1");

    // getFast() is a fixed; recovering the original fraction exactly means
    // comparing against the seven upstream offers rather than dividing.
    fixed speed = ctx->getSimulator().getFast();
    const int fractions[][2] = { {1,8}, {1,4}, {1,2}, {1,1}, {2,1}, {4,1}, {8,1} };
    for (auto &f : fractions) {
        if (speed == fixed(f[0]) / fixed(f[1])) {
            std::ostringstream out;
            out << f[0] << "|" << f[1];
            return env->NewStringUTF(out.str().c_str());
        }
    }
    return env->NewStringUTF("1|1");
}
