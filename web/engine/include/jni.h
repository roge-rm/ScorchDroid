#pragma once
// Just enough JNI for the browser build to compile jni/engine_jni.cpp and
// jni/renderer_jni.cpp to WebAssembly unchanged, so the page calls the same
// functions NativeBridge and GameRenderer do on a phone. Lifted from
// Acidulous's platform/web/jni.h, minus its threads: this build has one.
//
// A jstring is a std::string, a jintArray a std::vector<jint> and so on.
// Everything made during a call, by the engine or by the page passing
// arguments in, goes into one arena. The page reads the results and then
// frees the arena (sd_jni_release in web_bridge.cpp), so DeleteLocalRef frees
// nothing.

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

#define JNI_FALSE 0
#define JNI_TRUE 1
#define JNI_ABORT 2
#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006
#define JNIEXPORT __attribute__((used, visibility("default")))
#define JNICALL

struct _jobject {
    virtual ~_jobject() = default;
};
struct _jclass : _jobject {};
struct _jstring : _jobject {
    std::string text;
};
struct _jarray : _jobject {
    virtual jsize size() const = 0;
};
template <class T> struct _jtypedArray : _jarray {
    std::vector<T> items;
    jsize size() const override { return static_cast<jsize>(items.size()); }
};
struct _jfloatArray : _jtypedArray<jfloat> {};
struct _jintArray : _jtypedArray<jint> {};
struct _jbyteArray : _jtypedArray<jbyte> {};
struct _jobjectArray : _jarray {
    std::vector<_jobject *> items;
    jsize size() const override { return static_cast<jsize>(items.size()); }
};

typedef _jobject *jobject;
typedef _jclass *jclass;
typedef _jstring *jstring;
typedef _jarray *jarray;
typedef _jfloatArray *jfloatArray;
typedef _jintArray *jintArray;
typedef _jbyteArray *jbyteArray;
typedef _jobjectArray *jobjectArray;
typedef struct _jmethodID *jmethodID;

namespace jniweb {
/** What calls have made since the page last released them. */
using Arena = std::vector<std::unique_ptr<_jobject>>;
inline Arena &arena() {
    static Arena own;
    return own;
}
template <class T> T *keep(T *made) {
    arena().emplace_back(made);
    return made;
}
} // namespace jniweb

struct JNIEnv_ {
    const char *GetStringUTFChars(jstring s, jboolean *copy) {
        if (copy != nullptr) *copy = JNI_FALSE;
        return s != nullptr ? s->text.c_str() : nullptr;
    }
    void ReleaseStringUTFChars(jstring, const char *) {}
    jstring NewStringUTF(const char *chars) {
        auto *s = jniweb::keep(new _jstring());
        s->text = chars != nullptr ? chars : "";
        return s;
    }

    jsize GetArrayLength(jarray a) { return a != nullptr ? a->size() : 0; }

#define SD_JNI_ARRAY(Name, T, A)                                                     \
    T *Get##Name##ArrayElements(A *a, jboolean *copy) {                              \
        if (copy != nullptr) *copy = JNI_FALSE;                                      \
        return a != nullptr ? a->items.data() : nullptr;                             \
    }                                                                                \
    void Release##Name##ArrayElements(A *, T *, jint) {}                             \
    void Get##Name##ArrayRegion(A *a, jsize start, jsize len, T *buf) {              \
        std::memcpy(buf, a->items.data() + start, static_cast<size_t>(len) * sizeof(T)); \
    }                                                                                \
    void Set##Name##ArrayRegion(A *a, jsize start, jsize len, const T *buf) {        \
        std::memcpy(a->items.data() + start, buf, static_cast<size_t>(len) * sizeof(T)); \
    }                                                                                \
    A *New##Name##Array(jsize len) {                                                 \
        auto *a = jniweb::keep(new A());                                             \
        a->items.assign(static_cast<size_t>(len), T{});                              \
        return a;                                                                    \
    }
    SD_JNI_ARRAY(Float, jfloat, _jfloatArray)
    SD_JNI_ARRAY(Int, jint, _jintArray)
    SD_JNI_ARRAY(Byte, jbyte, _jbyteArray)
#undef SD_JNI_ARRAY

    jclass FindClass(const char *) {
        static _jclass any;
        return &any;
    }
    jobjectArray NewObjectArray(jsize len, jclass, jobject initial) {
        auto *a = jniweb::keep(new _jobjectArray());
        a->items.assign(static_cast<size_t>(len), initial);
        return a;
    }
    jobject GetObjectArrayElement(jobjectArray a, jsize i) { return a->items[static_cast<size_t>(i)]; }
    void SetObjectArrayElement(jobjectArray a, jsize i, jobject value) { a->items[static_cast<size_t>(i)] = value; }
    void DeleteLocalRef(jobject) {}
};
typedef JNIEnv_ JNIEnv;

/** Only JNI_OnLoad's signature needs it: nothing here has a VM. */
struct JavaVM_ {};
typedef JavaVM_ JavaVM;
