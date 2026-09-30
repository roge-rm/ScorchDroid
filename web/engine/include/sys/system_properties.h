#pragma once
// The browser build's <sys/system_properties.h>. The renderer reads a few
// debug switches from Android's system properties (adb shell setprop); a
// page has none, so every property reads as unset.

#define PROP_VALUE_MAX 92

inline int __system_property_get(const char *, char *value) {
    if (value != nullptr) value[0] = '\0';
    return 0;
}
