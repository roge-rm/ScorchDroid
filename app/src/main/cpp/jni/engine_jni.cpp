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
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <simactions/TankAddSimAction.hpp>
#include <simactions/TankAccessorySimAction.hpp>
#include <tankai/TankAIAdder.hpp>
#include <server/ServerSimulator.hpp>
#include <tank/TankScore.hpp>
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
        LANG_STRING("Player"), "");
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
Java_com_rm_scorchdroid_NativeBridge_startLocalGame(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (g_mode != EngineMode::kNone) {
        LOGE("startLocalGame: engine already started (mode=%d)", (int) g_mode);
        return JNI_FALSE;
    }
    ScorchedServerSettingsOptions settings("scorchdroid_server.xml", false, false);

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
                if (accessory->getNoBuy() || accessory->getAIOnly() || accessory->getBotOnly()) continue;

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
                    << tank->getAccessories().getAccessoryCount(accessory) << '|'
                    << (accessory == current ? 1 : 0) << '|'
                    << typeName;
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
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_fireWeapon(JNIEnv *env, jobject thiz, jint playerId,
                                                 jfloat angleDegrees, jfloat elevationDegrees,
                                                 jfloat power);

// M4 touch-fire: given a tap in the same normalized [-0.9, 0.9] landscape
// space the renderer draws in (see renderer_jni.cpp), fires "my tank" (see
// getMyTankId()/addHumanTank() above) aimed at the tapped landscape point,
// at the given elevation (touch-controlled via the elevation SeekBar - see
// MainActivity) and a fixed 0.7 power - a minimal "tap where you want to
// hit" aim gesture, not full drag-based power control for the tap case.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_handleTap(JNIEnv *env, jobject thiz, jfloat normX, jfloat normY, jfloat elevationDegrees) {
    unsigned int myId = 0;
    float angle;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        ScorchedContext *ctx = activeContext();
        if (!ctx) return JNI_FALSE;

        Tank *from = findMyTank();
        if (!from || !from->getAlive()) return JNI_FALSE;
        myId = from->getPlayerId();

        HeightMap &heightMap = ctx->getLandscapeMaps().getGroundMaps().getHeightMap();
        int mapW = heightMap.getMapWidth();
        int mapH = heightMap.getMapHeight();
        if (mapW <= 0) mapW = 1;
        if (mapH <= 0) mapH = 1;

        float tapLandscapeX = (normX + 0.9f) / 1.8f * (float) mapW;
        float tapLandscapeY = (normY + 0.9f) / 1.8f * (float) mapH;

        FixedVector &fromPos = from->getLife().getTargetPosition();
        float dx = tapLandscapeX - fromPos[0].asFloat();
        float dy = tapLandscapeY - fromPos[1].asFloat();
        // The engine's fire angle isn't the standard atan2(dy,dx) (CCW from
        // +X) - TankLib::getVelocityVector() gives
        // vx = -sin(xy), vy = cos(xy), i.e. a bearing measured
        // counterclockwise from +Y (xy=90 points to -X/"west", not
        // +X/"east" the way a normal clockwise compass would). For a
        // desired direction (dx,dy) that inverts to angle = atan2(-dx, dy).
        // Using plain atan2(dy,dx) here fired at a rotated/mirrored angle
        // relative to the tapped point.
        angle = (float) (atan2(-dx, dy) * 180.0 / M_PI);
        if (angle < 0) angle += 360.0f;
    }

    return Java_com_rm_scorchdroid_NativeBridge_fireWeapon(
        env, thiz, (jint) myId, (jfloat) angle, elevationDegrees, 0.7f);
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
    LOGI("fireWeapon: player=%d weapon=%u angle=%.1f elevation=%.1f power=%.2f ok=%d",
         playerId, weapon->getAccessoryId(), angleDegrees, elevationDegrees, power, ok);
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
