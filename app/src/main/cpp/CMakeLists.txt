cmake_minimum_required(VERSION 3.22)
project(scorchdroid_engine)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../..)
set(THIRD_PARTY_DIR ${REPO_ROOT}/third_party)
set(SCORCHED_SRC ${THIRD_PARTY_DIR}/scorched3d/src)

# Apply our Android-portability patches to the pinned upstream submodule
# checkout before compiling anything from it. Idempotent - see the script
# for why the submodule itself is never edited/committed directly.
execute_process(
    COMMAND bash ${REPO_ROOT}/scripts/apply_patches.sh
    RESULT_VARIABLE APPLY_PATCHES_RESULT
)
if(NOT APPLY_PATCHES_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply patches/scorched3d/*.patch (see scripts/apply_patches.sh)")
endif()

# --- Third-party deps needed by src/common (added one at a time as M1
# progresses; see the porting plan for the full dependency list) ---

set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/expat/expat expat-build)

# --- Vendored Scorched3D core (src/common, src/server) ---
# NOTE M1 in progress, module by module. src/client and src/launcher are
# NEVER added here - they're fully replaced by Kotlin/Compose + a
# from-scratch GLES3 renderer per the porting plan. Each wired-in
# subdirectory below is named explicitly and globbed only within itself,
# so this can never accidentally sweep in src/client or src/launcher.

set(SCORCHED_COMMON_SUBDIRS
    common/XML
    common/common
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

add_library(scorched_common STATIC ${SCORCHED_COMMON_SRCS})

target_include_directories(scorched_common PUBLIC
    ${SCORCHED_SRC}/common
    ${SCORCHED_SRC}/common/porting
    ${CMAKE_CURRENT_SOURCE_DIR}/porting
)

target_link_libraries(scorched_common PUBLIC expat)

# Matches upstream's own dedicated-server build flag (see meson.build's
# server target) - it gates out client-only rendering deps (GLEXT, the
# GLW widget toolkit, etc.) throughout src/common at compile time, which
# is exactly what our "reuse src/common, replace the client" architecture
# wants, for free.
target_compile_definitions(scorched_common PUBLIC S3D_SERVER=1)

# --- App-side JNI bridge (owned by this repo, not vendored) ---

add_library(${CMAKE_PROJECT_NAME} SHARED
    jni/engine_jni.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    jni
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
    scorched_common
    ${log-lib}
)
