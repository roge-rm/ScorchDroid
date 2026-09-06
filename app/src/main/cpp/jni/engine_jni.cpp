#include <jni.h>
#include <android/log.h>
#include <string>
#include <unistd.h>
#include <cstdlib>

#include <server/ScorchedServer.hpp>
#include <server/ScorchedServerSettings.hpp>
#include <common/Defines.hpp>

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
// upstream ScorchedServer bootstrap path, using its own default local-game
// settings file, rather than a hand-rolled harness - see the porting plan
// for why this was chosen over stubbing the weapon/data system.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_rm_scorchdroid_NativeBridge_startLocalGame(JNIEnv *env, jobject /* this */) {
    ScorchedServerSettingsOptions settings("data/server.xml", false, false);
    bool started = ScorchedServer::startServer(settings, true, nullptr);
    LOGI("ScorchedServer::startServer -> %d", started);
    return started ? JNI_TRUE : JNI_FALSE;
}
