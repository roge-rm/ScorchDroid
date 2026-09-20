cmake_minimum_required(VERSION 3.16)
project(scorchdroid_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fast, non-Android verification loop for the game-logic code shared with
# the Android build (see the porting plan): builds the same scorched_common
# module list as app/src/main/cpp/CMakeLists.txt, natively for this
# machine, so weapon/economy/fixed-point math changes can be checked with a
# plain host compile+run instead of a full NDK build + emulator/device
# cycle.
#
# That library's definition now lives in cmake/ScorchedCommon.cmake, shared
# with dedicated-server/CMakeLists.txt, rather than being spelled out here
# twice. It is still deliberately kept close to a literal copy of the
# Android CMakeLists.txt's module list rather than a minimal subset, so
# these tests run against the exact same source set Android does.

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(THIRD_PARTY_DIR ${REPO_ROOT}/third_party)
set(SCORCHDROID_REPO_ROOT ${REPO_ROOT} CACHE PATH "ScorchDroid repository root")
include(${REPO_ROOT}/cmake/ScorchedCommon.cmake)

# The dedicated server and its control channel, built from their own
# project so a server image can configure that directory alone. Included
# here so the documented `cd host-tests/build && cmake .. && make` still
# produces both binaries, exactly as it did when this file defined the
# server target itself.
add_subdirectory(${REPO_ROOT}/dedicated-server dedicated-server-build)

add_executable(host_tests main.cpp)
target_link_libraries(host_tests PRIVATE
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
    scorchdroid_control
)
# -rdynamic so the temporary crash-backtrace handler in main.cpp (see
# crashBacktraceHandler) can resolve symbol names via backtrace_symbols_fd().
target_link_options(host_tests PRIVATE -rdynamic)
# So main.cpp can chdir() to real upstream data without any Android-style
# asset-extraction step - this is a plain Linux process with normal
# filesystem access to the submodule checkout.
target_compile_definitions(host_tests PRIVATE
    SCORCHED_SUBMODULE_ROOT="${THIRD_PARTY_DIR}/scorched3d"
    # The config the app actually ships, so the setup tests exercise our own
    # server settings rather than upstream's data/server.xml - the bot it
    # names is exactly what a mod has to be able to provide.
    SCORCHDROID_APP_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/assets/scorchdroid_server.xml"
)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
