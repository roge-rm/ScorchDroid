// Browser-only exports. The engine and renderer calls themselves are
// engine_jni.cpp and renderer_jni.cpp compiled against include/jni.h; the page
// calls those Java_* functions directly. This file adds what a phone gets from
// Android instead: the JNI environment and its objects, the GL context, and
// the browser storage saves and settings live in.

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <jni.h>

extern "C" {

// --- JNI objects, for the page ----------------------------------------------

EMSCRIPTEN_KEEPALIVE JNIEnv *sd_jni_env() {
    static JNIEnv env;
    return &env;
}
/** Free everything made for and by the calls since the last release. */
EMSCRIPTEN_KEEPALIVE void sd_jni_release() { jniweb::arena().clear(); }

EMSCRIPTEN_KEEPALIVE jstring sd_jni_string(const char *utf8) { return sd_jni_env()->NewStringUTF(utf8); }
EMSCRIPTEN_KEEPALIVE const char *sd_jni_chars(jstring s) { return s != nullptr ? s->text.c_str() : nullptr; }
EMSCRIPTEN_KEEPALIVE int sd_jni_length(jarray a) { return a != nullptr ? a->size() : 0; }

EMSCRIPTEN_KEEPALIVE jobjectArray sd_jni_objects(int n) { return sd_jni_env()->NewObjectArray(n, nullptr, nullptr); }
EMSCRIPTEN_KEEPALIVE jobject sd_jni_object_at(jobjectArray a, int i) { return a->items[static_cast<size_t>(i)]; }
EMSCRIPTEN_KEEPALIVE void sd_jni_set_object(jobjectArray a, int i, jobject v) { a->items[static_cast<size_t>(i)] = v; }

EMSCRIPTEN_KEEPALIVE jintArray sd_jni_ints(int n) { return sd_jni_env()->NewIntArray(n); }
/** Where an int array's items are, so the page can read or fill them in one go. */
EMSCRIPTEN_KEEPALIVE jint *sd_jni_int_data(jintArray a) { return a != nullptr ? a->items.data() : nullptr; }

EMSCRIPTEN_KEEPALIVE jfloatArray sd_jni_floats(int n) { return sd_jni_env()->NewFloatArray(n); }
EMSCRIPTEN_KEEPALIVE jfloat *sd_jni_float_data(jfloatArray a) { return a != nullptr ? a->items.data() : nullptr; }

// --- GL -----------------------------------------------------------------------

/**
 * A WebGL 2 context on [selector]'s canvas, made current. What the phone's
 * GLSurfaceView asks EGL for: RGBA8 and a depth buffer, no stencil. Returns 0
 * when the browser has no WebGL 2.
 */
EMSCRIPTEN_KEEPALIVE int sd_gl_create(const char *selector) {
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    attrs.alpha = false;
    attrs.depth = true;
    attrs.stencil = false;
    attrs.antialias = false;
    attrs.premultipliedAlpha = false;
    attrs.preserveDrawingBuffer = false;
    attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_create_context(selector, &attrs);
    if (context <= 0) return 0;
    emscripten_webgl_make_context_current(context);
    // The sea's float tile is filtered and, at full water detail, rendered
    // into. Both are extensions in WebGL 2 where ES 3 on a phone has them
    // in the driver.
    emscripten_webgl_enable_extension(context, "OES_texture_float_linear");
    emscripten_webgl_enable_extension(context, "EXT_color_buffer_half_float");
    emscripten_webgl_enable_extension(context, "EXT_color_buffer_float");
    return static_cast<int>(context);
}

} // extern "C"
