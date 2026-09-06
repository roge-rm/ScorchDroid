#include <jni.h>
#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_com_rm_scorchdroid_NativeBridge_helloFromNative(JNIEnv *env, jobject /* this */) {
    std::string greeting = "Hello from the native engine (M0 toolchain proof)";
    return env->NewStringUTF(greeting.c_str());
}
