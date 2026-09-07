cmake_minimum_required(VERSION 3.22)
project(scorchdroid_engine)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(THIRD_PARTY_DIR ${REPO_ROOT}/third_party)
set(SCORCHED_SRC ${THIRD_PARTY_DIR}/scorched3d/src)

# NOTE: patches/scorched3d/*.patch are applied to the submodule checkout by
# the Gradle "applyScorchedPatches" task (app/build.gradle.kts), not here.
# CMake only re-runs execute_process() when it decides to reconfigure (e.g.
# CMakeLists.txt changed) - not on every build - so a patch-apply call here
# can silently be skipped for builds where nothing in this file changed,
# even if the submodule checkout itself has since been reset to pristine
# (e.g. by `git submodule update`). Gradle can be forced to check every
# build regardless of CMake's own configure cache.

# --- Third-party deps needed by src/common (added one at a time as M1
# progresses; see the porting plan for the full dependency list) ---

set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/expat/expat expat-build)

# Lua 5.1.5 (official lua.org source, MIT license), compiled as C++ per
# src/common/lua's expectations. lua.c/luac.c are the standalone
# interpreter/compiler executables - not needed as a library.
file(GLOB LUA_SRCS ${THIRD_PARTY_DIR}/lua-5.1.5/src/*.c)
list(REMOVE_ITEM LUA_SRCS
    ${THIRD_PARTY_DIR}/lua-5.1.5/src/lua.c
    ${THIRD_PARTY_DIR}/lua-5.1.5/src/luac.c
)
add_library(lua STATIC ${LUA_SRCS})
target_include_directories(lua PUBLIC ${THIRD_PARTY_DIR}/lua-5.1.5/src)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX)

# NDK's default -D_FORTIFY_SOURCE=2 produces a false-positive
# "strchr: prevented read past end of buffer" abort in luaL_findtable()
# (lauxlib.c) on a plain 4-byte string literal ("s3d") - a known class of
# bionic/clang __builtin_object_size misbehavior on certain compiler/
# optimization-level combinations, not a real bug in this 30-year-old,
# widely-used code path. Disabled for this target only.
target_compile_options(lua PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)

# libpng (+ Android/NDK-provided zlib). Needed natively, not just for
# client-side rendering textures: common/landscapemap's heightmap loading
# calls ImageFactory::loadImage() unconditionally, even server-side - see
# the porting plan for why the earlier "BitmapFactory on the Kotlin side"
# plan was superseded for this.
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/libpng libpng-build)

# libjpeg-turbo. 390+ .jpg assets in the real bundled data/ (icons, some
# textures) genuinely need this, not just PNG - see the porting plan.
# SIMD disabled to avoid requiring a NASM toolchain in this build; revisit
# if JPEG decode performance ever matters.
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(WITH_SIMD OFF CACHE BOOL "" FORCE)
set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_JAVA OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/libjpeg-turbo libjpeg-turbo-build)

# --- Vendored Scorched3D core (src/common, src/server) ---
# NOTE M1 in progress, module by module. src/client and src/launcher are
# NEVER added here - they're fully replaced by Kotlin/Compose + a
# from-scratch GLES3 renderer per the porting plan. Each wired-in
# subdirectory below is named explicitly and globbed only within itself,
# so this can never accidentally sweep in src/client or src/launcher.

set(SCORCHED_COMMON_SUBDIRS
    common/XML
    common/common
    common/lang
    common/net
    common/engine
    common/movement
    common/placement
    common/tanket
    common/target
    common/tank
    common/landscapedef
    common/landscapemap
    common/3dsparse
    common/events
    common/actions
    common/simactions
    common/coms
    common/weapons
    common/lua
    server/server
    server/tankai
    common/image
)

set(SCORCHED_COMMON_SRCS "")
foreach(subdir ${SCORCHED_COMMON_SUBDIRS})
    file(GLOB subdir_srcs ${SCORCHED_SRC}/${subdir}/*.cpp)
    list(APPEND SCORCHED_COMMON_SRCS ${subdir_srcs})
endforeach()

# common/Keyboard.cpp is a live SDL_Event-polling keyboard-state tracker
# (SDL_GetKeyState/SDL_EnableKeyRepeat/SDL_Event) meant to be fed by SDL's
# main loop. Android input is touch-first and will get its own JNI-driven
# input path (see the porting plan) - this isn't a header-portability fix,
# it's a real design task for a later milestone, so it's excluded here
# rather than stubbed.
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/common/common/Keyboard.cpp)

# ServerBrowserInfo uses SDL_net's UDP API for the legacy public
# master-server-list broadcast/announce mechanism. Per the porting plan,
# browsing/announcing to the public PC server list is an explicit,
# separate, deferred feature (LAN discovery uses Android NSD instead) -
# excluded rather than ported for now.
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/server/server/ServerBrowserInfo.cpp)

# ServerMain is the standalone dedicated-server executable's main loop
# (only ever called from the excluded scorcheds/main.cpp) and pulls in
# ServerBrowserInfo.hpp - nothing else references it, and our Android
# harness drives server logic directly rather than through this loop, so
# it's excluded rather than ported.
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/server/server/ServerMain.cpp)

add_library(scorched_common STATIC
    ${SCORCHED_COMMON_SRCS}
    porting/SDL_net_compat.cpp
    porting/SDL_thread_compat.cpp
    porting/Keyboard_stub.cpp
    porting/SoundEventQueue.cpp
    porting/DeformEventQueue.cpp
    porting/EffectEventQueue.cpp
    porting/ClientSync.cpp
    porting/ClientContext.cpp
    porting/LandscapeTextureBuilder.cpp
)

# common/3dsparse/aseFile.{lex,tab}.cpp are flex/bison-generated parsers
# using the 'register' keyword, which is a hard error under C++17 (it was
# only deprecated pre-C++17, removed after). Rather than hand-editing
# generated code, compile just these two files against an older standard
# that still accepts it.
set_source_files_properties(
    ${SCORCHED_SRC}/common/3dsparse/aseFile.lex.cpp
    ${SCORCHED_SRC}/common/3dsparse/aseFile.tab.cpp
    PROPERTIES COMPILE_OPTIONS "-std=gnu++14"
)

target_include_directories(scorched_common PUBLIC
    ${SCORCHED_SRC}/common
    ${SCORCHED_SRC}/common/porting
    ${SCORCHED_SRC}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/porting
    # libjpeg-turbo doesn't export its include dirs via the target (unlike
    # libpng/expat), just a directory-scoped include_directories() that
    # doesn't reach consumers in a different CMakeLists.txt - add manually.
    ${THIRD_PARTY_DIR}/libjpeg-turbo/src
    ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build
)

target_link_libraries(scorched_common PUBLIC expat lua png_static jpeg-static)

# Matches upstream's own dedicated-server build flag (see meson.build's
# server target) - it gates out client-only rendering deps (GLEXT, the
# GLW widget toolkit, etc.) throughout src/common at compile time, which
# is exactly what our "reuse src/common, replace the client" architecture
# wants, for free.
target_compile_definitions(scorched_common PUBLIC S3D_SERVER=1)

# Android/bionic has a real snprintf/vsnprintf - tell DefinesString.hpp so
# it doesn't fall back to common/common/snprintf.c's smb_snprintf (a
# portable substitute for platforms without one, mainly old MSVC). This
# macro is normally set by autoconf/meson's configure step, which our
# CMake build doesn't run.
target_compile_definitions(scorched_common PUBLIC HAVE_SNPRINTF)

# --- App-side JNI bridge (owned by this repo, not vendored) ---

add_library(${CMAKE_PROJECT_NAME} SHARED
    jni/engine_jni.cpp
    jni/renderer_jni.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    jni
)

find_library(log-lib log)
find_library(gles3-lib GLESv3)

# scorched_common relies heavily on self-registering static objects
# (REGISTER_ACCESSORY_SOURCE and similar macros - a file-scope object whose
# constructor registers a weapon/accessory/action type into a factory map).
# A static library only pulls in the .o files it needs to resolve an
# otherwise-undefined symbol; nothing else references these registration
# objects, so the linker silently drops them and their constructors never
# run - accessories.xml then fails to parse with "Failed to find accessory
# part type" for every registered type. --whole-archive forces every object
# file in scorched_common into the final .so regardless.
target_link_libraries(${CMAKE_PROJECT_NAME}
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
    ${log-lib}
    ${gles3-lib}
)
