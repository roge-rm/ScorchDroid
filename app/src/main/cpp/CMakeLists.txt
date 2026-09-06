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
# NOTE M1 in progress: only the XML module (+ its direct transitive deps)
# is wired up so far. src/client and src/launcher are never added here -
# they're fully replaced by Kotlin/Compose + a from-scratch GLES3 renderer.

add_library(scorched_common STATIC
    ${SCORCHED_SRC}/common/XML/XMLFile.cpp
    ${SCORCHED_SRC}/common/XML/XMLNode.cpp
    ${SCORCHED_SRC}/common/XML/XMLParser.cpp
    ${SCORCHED_SRC}/common/XML/XMLStringBuffer.cpp
)

target_include_directories(scorched_common PUBLIC
    ${SCORCHED_SRC}/common
    ${SCORCHED_SRC}/common/porting
)

target_link_libraries(scorched_common PUBLIC expat)

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
