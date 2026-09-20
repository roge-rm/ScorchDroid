cmake_minimum_required(VERSION 3.16)
project(scorchdroid_dedicated_server C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The standalone Linux dedicated server. Buildable two ways, and both are
# used: on its own (what docker/server.Dockerfile configures, so a server
# image never compiles the 5000-line host-tests main), and as a
# subdirectory of host-tests/CMakeLists.txt, which is where it lived until
# the Docker work needed it separable. Either way it is the same binary
# from the same scorched_common.

set(SCORCHDROID_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. CACHE PATH "ScorchDroid repository root")
include(${SCORCHDROID_REPO_ROOT}/cmake/ScorchedCommon.cmake)

# The control channel, in its own library so host-tests can link and drive
# it directly (testControlChannel) rather than only through a running
# process - the project's own rule is that logic is pinned on the host.
add_library(scorchdroid_control STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ControlServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LogRing.cpp
)
target_include_directories(scorchdroid_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(scorchdroid_control PUBLIC scorched_common)

add_executable(dedicated_server ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(dedicated_server PRIVATE
    -Wl,--whole-archive
    scorched_common
    -Wl,--no-whole-archive
    scorchdroid_control
)
# -rdynamic for the same reason host_tests uses it: readable symbol names
# in a backtrace.
target_link_options(dedicated_server PRIVATE -rdynamic)

# Compiled-in *defaults* only - every one of these is overridable at run
# time by a flag or an environment variable (see main.cpp's usage text).
# They exist so a developer build behaves exactly as it did before there
# was a CLI: run it with no arguments and it serves the submodule checkout.
target_compile_definitions(dedicated_server PRIVATE
    SCORCHED_SUBMODULE_ROOT="${SCORCHDROID_REPO_ROOT}/third_party/scorched3d"
    DEDICATED_SERVER_XML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/dedicated_server.xml"
)
