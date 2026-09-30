#pragma once
// The browser build's <android/log.h>: the engine's log lines go to the
// console, which is where a web page's log is.

#include <cstdarg>
#include <cstdio>

enum {
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG = 3,
    ANDROID_LOG_INFO = 4,
    ANDROID_LOG_WARN = 5,
    ANDROID_LOG_ERROR = 6,
};

inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    FILE *out = prio >= ANDROID_LOG_WARN ? stderr : stdout;
    std::fprintf(out, "%s: ", tag);
    std::vfprintf(out, fmt, args);
    std::fputc('\n', out);
    va_end(args);
    return 0;
}
