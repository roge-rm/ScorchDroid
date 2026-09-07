cmake_minimum_required(VERSION 3.16)
project(scorchdroid_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fast, non-Android verification loop for the game-logic code shared with
# the Android build (see the porting plan): builds the same scorched_common
# module list as app/src/main/cpp/CMakeLists.txt, natively for this
# machine, so weapon/economy/fixed-point math changes can be checked with a
# plain host compile+run instead of a full NDK build + emulator/device
# cycle. Deliberately kept close to a literal copy of the Android
# CMakeLists.txt's module list rather than trying to find a minimal subset,
# so these tests run against the exact same source set Android does.

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(THIRD_PARTY_DIR ${REPO_ROOT}/third_party)
set(SCORCHED_SRC ${THIRD_PARTY_DIR}/scorched3d/src)
set(ANDROID_PORTING_DIR ${REPO_ROOT}/app/src/main/cpp/porting)

# NOTE: same caveat as app/src/main/cpp/CMakeLists.txt - CMake only re-runs
# execute_process() on reconfigure. Delete build/ (or touch this file) after
# resetting the submodule if patches seem to not be applying.
execute_process(
    COMMAND bash ${REPO_ROOT}/scripts/apply_patches.sh
    RESULT_VARIABLE APPLY_PATCHES_RESULT
)
if(NOT APPLY_PATCHES_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply patches/scorched3d/*.patch (see scripts/apply_patches.sh)")
endif()

set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/expat/expat expat-build)

file(GLOB LUA_SRCS ${THIRD_PARTY_DIR}/lua-5.1.5/src/*.c)
list(REMOVE_ITEM LUA_SRCS
    ${THIRD_PARTY_DIR}/lua-5.1.5/src/lua.c
    ${THIRD_PARTY_DIR}/lua-5.1.5/src/luac.c
)
add_library(lua STATIC ${LUA_SRCS})
target_include_directories(lua PUBLIC ${THIRD_PARTY_DIR}/lua-5.1.5/src)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX)

set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/libpng libpng-build)

set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
set(WITH_SIMD OFF CACHE BOOL "" FORCE)
set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_JAVA OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/libjpeg-turbo libjpeg-turbo-build)

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

# Same exclusions as the Android build - see that CMakeLists.txt for why.
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/common/common/Keyboard.cpp)
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/server/server/ServerBrowserInfo.cpp)
list(REMOVE_ITEM SCORCHED_COMMON_SRCS ${SCORCHED_SRC}/server/server/ServerMain.cpp)

add_library(scorched_common STATIC
    ${SCORCHED_COMMON_SRCS}
    ${ANDROID_PORTING_DIR}/SDL_net_compat.cpp
    ${ANDROID_PORTING_DIR}/SDL_thread_compat.cpp
    ${ANDROID_PORTING_DIR}/Keyboard_stub.cpp
    ${ANDROID_PORTING_DIR}/SoundEventQueue.cpp
    ${ANDROID_PORTING_DIR}/DeformEventQueue.cpp
    ${ANDROID_PORTING_DIR}/ClientSync.cpp
    ${ANDROID_PORTING_DIR}/ClientContext.cpp
    ${ANDROID_PORTING_DIR}/LandscapeTextureBuilder.cpp
)

set_source_files_properties(
    ${SCORCHED_SRC}/common/3dsparse/aseFile.lex.cpp
    ${SCORCHED_SRC}/common/3dsparse/aseFile.tab.cpp
    PROPERTIES COMPILE_OPTIONS "-std=gnu++14"
)

target_include_directories(scorched_common PUBLIC
    ${SCORCHED_SRC}/common
    ${SCORCHED_SRC}/common/porting
    ${SCORCHED_SRC}/server
    ${ANDROID_PORTING_DIR}
    ${THIRD_PARTY_DIR}/libjpeg-turbo/src
    ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build
)

target_link_libraries(scorched_common PUBLIC expat lua png_static jpeg-static pthread)

target_compile_definitions(scorched_common PUBLIC S3D_SERVER=1 HAVE_SNPRINTF)

add_executable(host_tests main.cpp)
target_link_libraries(host_tests PRIVATE
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
)
# -rdynamic so the temporary crash-backtrace handler in main.cpp (see
# crashBacktraceHandler) can resolve symbol names via backtrace_symbols_fd().
target_link_options(host_tests PRIVATE -rdynamic)
# So main.cpp can chdir() to real upstream data without any Android-style
# asset-extraction step - this is a plain Linux process with normal
# filesystem access to the submodule checkout.
target_compile_definitions(host_tests PRIVATE
    SCORCHED_SUBMODULE_ROOT="${THIRD_PARTY_DIR}/scorched3d"
)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)

# M5: a genuine standalone Linux dedicated server for real cross-play
# testing (see dedicated-server/main.cpp for why this isn't upstream's own
# src/server/scorcheds - no system SDL/expat/meson available in this
# environment) - built from the exact same scorched_common library above,
# no new dependencies needed.
add_executable(dedicated_server ${REPO_ROOT}/dedicated-server/main.cpp)
target_link_libraries(dedicated_server PRIVATE
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
)
target_link_options(dedicated_server PRIVATE -rdynamic)
target_compile_definitions(dedicated_server PRIVATE
    SCORCHED_SUBMODULE_ROOT="${THIRD_PARTY_DIR}/scorched3d"
    DEDICATED_SERVER_XML_PATH="${REPO_ROOT}/dedicated-server/dedicated_server.xml"
)
