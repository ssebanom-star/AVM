# Android JNI 계층. 코어 엔진(core/)을 그대로 포함해 빌드한다.
cmake_minimum_required(VERSION 3.22)

project(avmjni CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 리포지터리 루트의 core/를 포함 (app/src/main/cpp 기준 4단계 상위).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../core avm_core_build)

add_library(avmjni SHARED
    avm_jni.cpp
)

target_link_libraries(avmjni PRIVATE
    avm_core
    android
    log
)
