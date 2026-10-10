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

file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)
add_custom_target(format
    COMMAND clang-format -i ${FORMAT_SOURCES}
    VERBATIM
)
add_custom_target(format-check
    COMMAND clang-format --dry-run --Werror ${FORMAT_SOURCES}
    VERBATIM
)

find_package(fmt CONFIG REQUIRED)
find_package(SDL3 3.4.2 CONFIG REQUIRED)
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
    src/runtime/address_arena.cpp
    src/runtime/controller.cpp
    src/runtime/display.cpp
    src/runtime/ge.cpp
    src/runtime/execution.cpp
    src/runtime/guest_io.cpp
    src/runtime/guest_structures.cpp
    src/runtime/kernel.cpp
    src/runtime/rasterizer.cpp
    src/runtime/runtime.cpp
    src/runtime/syscall_dispatcher.cpp
)
# Dynamic guest rounding and exception flags must survive optimization.
set_source_files_properties(src/cpu/floating_point.cpp PROPERTIES COMPILE_OPTIONS "-ffp-model=strict")

target_include_directories(psp_core PUBLIC src)
target_link_libraries(psp_core PUBLIC project_options strong_type::strong_type PRIVATE fmt::fmt)

add_library(psp_frontend src/frontend/window.cpp)
target_link_libraries(psp_frontend PUBLIC psp_core SDL3::SDL3)

add_executable(pspemu src/main.cpp)
target_link_libraries(pspemu PRIVATE psp_frontend fmt::fmt)

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
        src/runtime/tests/address_arena_test.cpp
    src/runtime/tests/display_test.cpp
        src/runtime/tests/controller_test.cpp
        src/runtime/tests/ge_test.cpp
        src/runtime/tests/execution_test.cpp
        src/runtime/tests/guest_structures_test.cpp
        src/runtime/tests/kernel_test.cpp
        src/runtime/tests/rasterizer_test.cpp
    )
    target_link_libraries(psp_tests PRIVATE psp_core GTest::gtest_main)
    target_compile_definitions(psp_tests PRIVATE
        PSPAUTOTESTS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/pspautotests"
        PSPEMU_RUNTIME_FIXTURES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/tests/fixtures"
    )
    gtest_discover_tests(psp_tests)

    # A fault after partial output must still print that output; cpu_alu needs about 3M instructions.
    # The absolute PRX path also checks that host directories stay out of the guest's argv[0].
    add_test(NAME pspemu.PrintsGuestOutputBeforeFault
        COMMAND pspemu "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pspautotests/tests/cpu/cpu_alu/cpu_alu.prx"
            --max-instructions 2000000
    )
    set_tests_properties(pspemu.PrintsGuestOutputBeforeFault PROPERTIES PASS_REGULAR_EXPRESSION "test_ins: fffe000f")

    add_executable(psp_frontend_tests src/frontend/tests/window_test.cpp)
    target_link_libraries(psp_frontend_tests PRIVATE psp_frontend GTest::gtest_main)
    target_compile_definitions(psp_frontend_tests PRIVATE
        PSPEMU_RUNTIME_FIXTURES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/tests/fixtures"
    )
    # Isolate SDL from the desktop and host GPU drivers; software surfaces permit pixel readback.
    gtest_discover_tests(psp_frontend_tests PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=offscreen\;SDL_RENDER_DRIVER=software\;SDL_FRAMEBUFFER_ACCELERATION=0"
    )
endif()
