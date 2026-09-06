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
#include <server/ServerHandlers.hpp>
#include <target/TargetContainer.hpp>
#include <engine/Simulator.hpp>
#include <net/NetInterface.hpp>
#include <common/Defines.hpp>
#include <common/Clock.hpp>
#include <common/Logger.hpp>
#include <tank/Tank.hpp>
#include <tanket/TanketAccessories.hpp>
#include <tanket/TanketShotInfo.hpp>
#include <tanket/TanketWeapon.hpp>
#include <weapons/Accessory.hpp>
#include <coms/ComsPlayedMoveMessage.hpp>
#include <target/TargetLife.hpp>
#include <landscapemap/LandscapeMaps.hpp>
#include <landscapemap/GroundMaps.hpp>
#include <landscapemap/HeightMap.hpp>
#include <cmath>

#define LOG_TAG "ScorchDroidEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_startLocalGame(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    ScorchedServerSettingsOptions settings("scorchdroid_server.xml", false, false);
    bool started = ScorchedServer::startServer(settings, true, nullptr);
    LOGI("ScorchedServer::startServer -> %d", started);
    return started ? JNI_TRUE : JNI_FALSE;
}

static Clock tickClock;

// Drives the real game simulation forward - replicates ServerMain.cpp's
// serverLoop() (excluded from this build - see the porting plan - because
// it also pulls in the deferred UDP-based ServerBrowserInfo/ServerWebServer
// at the top of that same file). serverLoop() itself is what actually
// advances ServerState (waiting for players -> new level -> buying ->
// playing), not Simulator::simulate() alone, so all of these calls are
// needed, not just the simulator step.
extern "C" JNIEXPORT void JNICALL
Java_com_rm_scorchdroid_NativeBridge_tickEngine(JNIEnv *env, jobject /* this */) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (!ScorchedServer::serverStarted() || !ScorchedServer::instance()->getContext().getNetInterfaceValid()) {
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
    server->getServerConnectAuthHandler().processMessages();
    server->getServerFileServer().simulate();
    server->getServerChannelManager().simulate(timeDifference);
    server->getTimedMessage().simulate();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_fireWeapon(JNIEnv *env, jobject thiz, jint playerId,
                                                 jfloat angleDegrees, jfloat elevationDegrees,
                                                 jfloat power);

// M2 touch-fire, step 1: given a tap in the same normalized [-0.9, 0.9]
// landscape space the renderer draws in (see renderer_jni.cpp), finds the
// tank nearest the tap and fires it at whichever other tank is on the
// field, with a fixed 45-degree elevation/0.7 power - a minimal "tap a tank
// to have it take its shot" interaction, not real aim/power touch controls
// yet.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_handleTap(JNIEnv *env, jobject thiz, jfloat normX, jfloat normY) {
    unsigned int nearestId = 0;
    unsigned int targetId = 0;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        if (!ScorchedServer::serverStarted()) return JNI_FALSE;

        HeightMap &heightMap = ScorchedServer::instance()->getLandscapeMaps().getGroundMaps().getHeightMap();
        int mapW = heightMap.getMapWidth();
        int mapH = heightMap.getMapHeight();
        if (mapW <= 0) mapW = 1;
        if (mapH <= 0) mapH = 1;

        float tapLandscapeX = (normX + 0.9f) / 1.8f * (float) mapW;
        float tapLandscapeY = (normY + 0.9f) / 1.8f * (float) mapH;

        float bestDist = 1e18f;
        std::map<unsigned int, Tank *> &tanks = ScorchedServer::instance()->getTargetContainer().getTanks();
        for (auto &entry : tanks) {
            if (!entry.second->getAlive()) continue;
            FixedVector &pos = entry.second->getLife().getTargetPosition();
            float dx = pos[0].asFloat() - tapLandscapeX;
            float dy = pos[1].asFloat() - tapLandscapeY;
            float dist = dx * dx + dy * dy;
            if (dist < bestDist) {
                bestDist = dist;
                nearestId = entry.first;
            }
        }
        for (auto &entry : tanks) {
            if (entry.first != nearestId && entry.second->getAlive()) {
                targetId = entry.first;
                break;
            }
        }
    }

    if (nearestId == 0 || targetId == 0) return JNI_FALSE;

    float angle;
    {
        std::lock_guard<std::mutex> lock(g_engineMutex);
        Tank *from = ScorchedServer::instance()->getTargetContainer().getTankById(nearestId);
        Tank *to = ScorchedServer::instance()->getTargetContainer().getTankById(targetId);
        if (!from || !to) return JNI_FALSE;
        FixedVector &fromPos = from->getLife().getTargetPosition();
        FixedVector &toPos = to->getLife().getTargetPosition();
        float dx = toPos[0].asFloat() - fromPos[0].asFloat();
        float dy = toPos[1].asFloat() - fromPos[1].asFloat();
        angle = (float) (atan2(dy, dx) * 180.0 / M_PI);
        if (angle < 0) angle += 360.0f;
    }

    return Java_com_rm_scorchdroid_NativeBridge_fireWeapon(
        env, thiz, (jint) nearestId, (jfloat) angle, 45.0f, 0.7f);
}

// M2 touch-fire: directly submits a move for the given tank, exactly as a
// real client's ComsPlayedMoveMessage would (see ServerPlayedMoveHandler.cpp
// / ServerTurnsSimultaneous::internalMoveFinished) - the only difference is
// we construct and dispatch it in-process instead of over NetLoopBack's
// socket, since we're already running inside the same process as the
// "server" side of that message. angle/elevation are in degrees, power is
// 0..1 - all converted to the engine's fixed-point types here so the touch
// layer only ever deals in plain floats.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_fireWeapon(JNIEnv *env, jobject /* this */,
                                                 jint playerId, jfloat angleDegrees,
                                                 jfloat elevationDegrees, jfloat power) {
    std::lock_guard<std::mutex> lock(g_engineMutex);
    if (!ScorchedServer::serverStarted()) return JNI_FALSE;

    Tank *tank = ScorchedServer::instance()->getTargetContainer().getTankById((unsigned int) playerId);
    if (!tank || !tank->getAlive()) return JNI_FALSE;

    Accessory *weapon = tank->getAccessories().getWeapons().getCurrent();
    if (!weapon) return JNI_FALSE;

    unsigned int moveId = tank->getShotInfo().getMoveId();
    ComsPlayedMoveMessage message((unsigned int) playerId, moveId, ComsPlayedMoveMessage::eShot);
    message.setShot(
        weapon->getAccessoryId(),
        fixed::fromFloat(angleDegrees),
        fixed::fromFloat(elevationDegrees),
        fixed::fromFloat(power),
        0, 0
    );

    ScorchedServer::instance()->getServerState().moveFinished(message);
    LOGI("fireWeapon: player=%d weapon=%u angle=%.1f elevation=%.1f power=%.2f",
         playerId, weapon->getAccessoryId(), angleDegrees, elevationDegrees, power);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_getGameStateDebugString(JNIEnv *env, jobject /* this */) {
    std::string result = "not started";
    if (ScorchedServer::serverStarted()) {
        ScorchedServer *server = ScorchedServer::instance();
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "state=%d tanks=%u",
                 (int) server->getServerState().getState(),
                 server->getTargetContainer().getNoOfTanks());
        result = buffer;
    }
    return env->NewStringUTF(result.c_str());
}
