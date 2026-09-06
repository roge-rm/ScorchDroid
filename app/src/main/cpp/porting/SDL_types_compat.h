#ifndef SCORCHDROID_SDL_TYPES_COMPAT_H
#define SCORCHDROID_SDL_TYPES_COMPAT_H

// Android build: minimal replacement for SDL's fixed-width integer typedefs
// (SDL is not part of this build - see the porting plan). Upstream's
// common/net code uses these purely as fixed-width integers, not for any
// actual SDL functionality, so a plain <cstdint>-based typedef set is a
// byte-for-byte equivalent stand-in.
#include <cstdint>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

#endif  // SCORCHDROID_SDL_TYPES_COMPAT_H
