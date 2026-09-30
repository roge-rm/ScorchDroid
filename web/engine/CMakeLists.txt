cmake_minimum_required(VERSION 3.22)
project(scorchdroid_web C CXX)

# The engine for the browser: the phone's engine_jni.cpp and renderer_jni.cpp
# compiled to WebAssembly against include/jni.h and include/android/log.h,
# over the same scorched_common the phone and the dedicated server build.
#
# One thread. A page served over plain http (the dedicated server's own, on
# a LAN) can't be cross-origin isolated, and without that there are no
# threads to have, so the build doesn't ask for them. The few places the
# engine made its own (NetBridge's send thread, the ground and ocean
# workers) run inline under __EMSCRIPTEN__ instead.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

# zlib is Emscripten's port (build.sh builds it), where libpng's
# find_package(ZLIB) looks for a system one.
set(ZLIB_INCLUDE_DIR ${EMSCRIPTEN_SYSROOT}/include CACHE PATH "" FORCE)
set(ZLIB_LIBRARY ${EMSCRIPTEN_SYSROOT}/lib/wasm32-emscripten/libz.a CACHE FILEPATH "" FORCE)

set(SCORCHDROID_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. CACHE PATH "ScorchDroid repository root")
include(${SCORCHDROID_REPO_ROOT}/cmake/ScorchedCommon.cmake)

set(CPP ${SCORCHDROID_REPO_ROOT}/app/src/main/cpp)

# The ASE parser defines alloca away to nothing, which then breaks the
# declaration in Emscripten's <alloca.h> when <stdlib.h> pulls it in. Taking
# the header first means its guard is already set by then.
set_source_files_properties(
    ${SCORCHED_SRC}/common/3dsparse/aseFile.lex.cpp
    ${SCORCHED_SRC}/common/3dsparse/aseFile.tab.cpp
    PROPERTIES COMPILE_OPTIONS "-std=gnu++14;-include;alloca.h"
)

add_executable(scorchdroid
    ${CPP}/jni/engine_jni.cpp
    ${CPP}/jni/renderer_jni.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WebSocketTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web_bridge.cpp
)
# include/ first, so the JNI files get the web's <jni.h> and <android/log.h>.
target_include_directories(scorchdroid BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CPP}/jni
)
target_link_libraries(scorchdroid PRIVATE
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
)
# The data every page loads. build.sh points this at a staged copy without
# the mods that are loaded later (stage_data.py); on its own it's all of it.
set(SCORCHDROID_WEB_DATA ${SCORCHDROID_REPO_ROOT}/third_party/scorched3d/data CACHE PATH "the preloaded game data")
set(DATA ${SCORCHDROID_WEB_DATA})
target_link_options(scorchdroid PRIVATE
    -sMODULARIZE=1
    -sEXPORT_NAME=ScorchDroidEngine
    -sENVIRONMENT=web
    -sINITIAL_MEMORY=256MB
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=2MB
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    # The game's files, where the phone extracts them: data/ and the
    # server config beside it, under one root the engine chdir()s into.
    "--preload-file=${DATA}@/scorched_root/data"
    "--preload-file=${CPP}/../assets/scorchdroid_server.xml@/scorched_root/scorchdroid_server.xml"
    # The licence the About screen shows, as the phone stages it.
    "--preload-file=${SCORCHDROID_REPO_ROOT}/LICENSE@/scorched_root/licenses/GPL-2.0.txt"
    -sFORCE_FILESYSTEM=1
    -lidbfs.js
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToUTF8','stringToNewUTF8','lengthBytesUTF8','FS','IDBFS','GL','HEAPU8','HEAP32']"
    "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
)
