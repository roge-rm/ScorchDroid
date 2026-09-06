#include <jni.h>
#include <android/log.h>
#include <string>
#include <unistd.h>
#include <cstdlib>

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

    LOGI("Engine data root: %s", path);
    env->ReleaseStringUTFChars(dataRoot, path);
    return ok ? JNI_TRUE : JNI_FALSE;
}
