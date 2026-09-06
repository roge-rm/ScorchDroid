cmake_minimum_required(VERSION 3.22)
project(scorchdroid_engine)

# M0: toolchain-proof only. Real Scorched3D source directories (upstream
# third_party/scorched3d/src/common, src/server) are added explicitly,
# module by module, starting in M1 - never globbed, and src/client /
# src/launcher are never added at all (replaced entirely by Kotlin/Compose
# + a from-scratch GLES3 renderer per the porting plan).

add_library(${CMAKE_PROJECT_NAME} SHARED
    jni/engine_jni.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    jni
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${log-lib}
)
