cmake_minimum_required(VERSION 3.20)

project(PSPEmulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PSPEMU_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

find_package(fmt CONFIG REQUIRED)
add_subdirectory(third_party/strong_type)

add_library(project_options INTERFACE)

if(MSVC)
    target_compile_options(project_options INTERFACE /W4)
else()
    target_compile_options(project_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(PSPEMU_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC")
    endif()
    target_compile_options(project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(project_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(psp_core
    src/common/payload_reader.cpp
    src/cpu/cpu.cpp
    src/cpu/cpu_state.cpp
    src/memory/memory.cpp
    src/loader/prx_reader.cpp
    src/loader/prx.cpp
    src/runtime/execution.cpp
    src/runtime/runtime.cpp
)
target_include_directories(psp_core PUBLIC src)
target_link_libraries(psp_core PUBLIC project_options strong_type::strong_type)

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
        src/loader/tests/prx_test.cpp
        src/loader/tests/prx_reader_test.cpp
        src/runtime/tests/execution_test.cpp
    )
    target_link_libraries(psp_tests PRIVATE psp_core GTest::gtest_main)
    target_compile_definitions(psp_tests PRIVATE PSPAUTOTESTS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/pspautotests")
    gtest_discover_tests(psp_tests)
endif()
