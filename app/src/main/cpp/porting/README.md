# Android porting layer

New code (not vendored from upstream Scorched3D) that lets unmodified
upstream logic keep using APIs/types it was written against, without
actually depending on SDL/wxWidgets/etc. on Android.

- `SDL_keysym_compat.h` - the `SDLKey` enum and `SDLK_*` constants, copied
  verbatim from SDL 1.2 (`include/SDL_keysym.h`,
  https://github.com/libsdl-org/SDL-1.2, LGPL-2.1, see
  `COPYING.SDL_keysym_compat.LGPL-2.1`). Upstream's `common/KeyTranslate.hpp`
  and `common/KeyboardHistory.hpp` use these purely as symbolic key-name
  constants for parsing/storing key-binding config - they don't need any
  actual SDL library code, just the enum values, so this lets those files
  (and their data tables) stay byte-for-byte unmodified rather than
  rewriting the key-binding system this early in the port.
