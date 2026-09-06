#include <jni.h>
#include <android/log.h>
#include <string>
#include <unistd.h>
#include <cstdlib>

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
