# AVM 코어 엔진 (네이티브 C++20).
#
# 두 가지 방식으로 빌드된다:
#  1. Android NDK: app/src/main/cpp/CMakeLists.txt가 add_subdirectory로 포함.
#  2. 호스트(리눅스/맥) 개발 빌드: 단위 테스트와 셀프테스트 CLI 실행.
#     cmake -S core -B build && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.22)

project(avm_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(avm_core STATIC
    src/log.cpp
    src/cpu/exception.cpp
    src/cpu/sysreg.cpp
    src/mem/phys_mem.cpp
    src/mmu/mmu.cpp
    src/decode/decoder.cpp
    src/interp/interpreter.cpp
    src/jit/arm64_emitter.cpp
    src/jit/code_cache.cpp
    src/jit/jit_engine.cpp
    src/dev/pl011.cpp
    src/vm/virt_board.cpp
    src/selftest.cpp
)

target_include_directories(avm_core PUBLIC include)

target_compile_options(avm_core PRIVATE
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti          # 코어 엔진은 예외/RTTI 비사용
)

if(ANDROID)
    target_link_libraries(avm_core PUBLIC log)
endif()

# 호스트 전용: 단위 테스트 + 셀프테스트 CLI.
if(NOT ANDROID)
    enable_testing()

    add_executable(avm_tests
        tests/test_main.cpp
        tests/decoder_test.cpp
        tests/interpreter_test.cpp
        tests/phys_mem_test.cpp
        tests/exception_test.cpp
        tests/sysreg_test.cpp
        tests/mmu_test.cpp
        tests/pl011_test.cpp
        tests/virt_board_test.cpp
        tests/arm64_emitter_test.cpp
        tests/code_cache_test.cpp
        tests/jit_translate_test.cpp
        tests/jit_exec_test.cpp
    )
    target_link_libraries(avm_tests PRIVATE avm_core)
    target_include_directories(avm_tests PRIVATE tests)
    target_compile_options(avm_tests PRIVATE -Wall -Wextra)
    add_test(NAME avm_unit_tests COMMAND avm_tests)

    add_executable(avm_selftest tools/avm_selftest_main.cpp)
    target_link_libraries(avm_selftest PRIVATE avm_core)
    add_test(NAME avm_cpu_selftest COMMAND avm_selftest)
endif()
