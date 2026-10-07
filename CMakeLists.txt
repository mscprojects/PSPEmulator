cmake_minimum_required(VERSION 3.20)

project(PSPEmulator LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PSPEmulator requires Clang")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PSPEMU_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

find_package(fmt CONFIG REQUIRED)
add_subdirectory(third_party/strong_type)

add_library(project_options INTERFACE)

target_compile_options(project_options INTERFACE -Wall -Wextra -Wpedantic $<$<CONFIG:Debug>:-O1>)

if(PSPEMU_ENABLE_SANITIZERS)
    target_compile_options(project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(psp_core
    src/common/payload_reader.cpp
    src/cpu/cpu.cpp
    src/cpu/floating_point.cpp
    src/memory/memory.cpp
    src/loader/prx_reader.cpp
    src/loader/prx.cpp
    src/runtime/execution.cpp
    src/runtime/guest_io.cpp
    src/runtime/guest_structures.cpp
    src/runtime/kernel.cpp
    src/runtime/runtime.cpp
    src/runtime/syscall_dispatcher.cpp
)
# Dynamic guest rounding and exception flags must survive optimization.
set_source_files_properties(src/cpu/floating_point.cpp PROPERTIES COMPILE_OPTIONS "-ffp-model=strict")

target_include_directories(psp_core PUBLIC src)
target_link_libraries(psp_core PUBLIC project_options strong_type::strong_type PRIVATE fmt::fmt)

add_executable(pspemu src/main.cpp)
target_link_libraries(pspemu PRIVATE psp_core fmt::fmt)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    add_executable(psp_tests
        src/common/tests/payload_reader_test.cpp
        src/memory/tests/memory_test.cpp
        src/cpu/tests/cpu_test.cpp
        src/cpu/tests/cpu_fpu_test.cpp
        src/loader/tests/prx_test.cpp
        src/loader/tests/prx_reader_test.cpp
        src/runtime/tests/execution_test.cpp
        src/runtime/tests/guest_structures_test.cpp
        src/runtime/tests/kernel_test.cpp
    )
    target_link_libraries(psp_tests PRIVATE psp_core GTest::gtest_main)
    target_compile_definitions(psp_tests PRIVATE PSPAUTOTESTS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/pspautotests")
    gtest_discover_tests(psp_tests)
endif()
