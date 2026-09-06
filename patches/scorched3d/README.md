# patches/scorched3d

Android-portability patches applied to the pinned `third_party/scorched3d`
submodule checkout by `scripts/apply_patches.sh` (invoked automatically from
the CMake configure step). Applied in filename order onto the exact pinned
commit - never edit the submodule checkout directly; add a new numbered
patch instead (see the porting plan for why: this keeps "upstream commit +
these patches" as a clean, reproducible GPLv2+ corresponding-source story).

- `0001-android-libcxx-portability-fixes.patch` - libc++ vs. legacy-GCC
  differences (`fixed.hpp`'s SDL-only `Sint64` typedef, `LangString`'s
  `basic_string<unsigned int>` needing an explicit `char_traits`
  specialization under libc++).
- `0002-android-common-common-module.patch` - `src/common/common` compiling
  for Android (`S3D_SERVER=1`, SDL mutex/timer/byte-order calls replaced).
- `0003-android-lang-and-net-modules.patch` - `src/common/lang` and
  `src/common/net`; the latter's real socket/thread portability work is
  mostly in `app/src/main/cpp/porting/SDL_{net,thread}_compat.*` instead of
  this patch, since `NetBuffer.hpp` transitively supplies those compat
  headers to the whole module (see that patch's commit message).
