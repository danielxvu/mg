# Unit-test suite for the modern C++23 modules.
#
# doctest is fetched as a lightweight, single-header framework: it compiles fast
# (important for a tight TDD / Ralph-loop cadence) and has zero runtime deps.
# It is pinned to a release tag for reproducibility across fresh agent sessions.
include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)  # keep doctest's own install() rules out of `cmake --install`
FetchContent_MakeAvailable(doctest)

# `doctest_discover_tests` registers each TEST_CASE with CTest individually.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

# Helper for a plain (header-only) test binary.
function(mg_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    doctest_discover_tests(${name})
endfunction()

# Helper for a test binary that imports C++20 named modules.
#   mg_add_module_test(<name> SOURCES <.cpp…> MODULES <.cppm…>)
# Module interface units go in a FILE_SET of type CXX_MODULES so CMake scans
# their import graph (P1689) and builds BMIs in dependency order.
function(mg_add_module_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;MODULES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_sources(${name} PRIVATE FILE_SET CXX_MODULES FILES ${ARG_MODULES})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    doctest_discover_tests(${name})
endfunction()

mg_add_test(test_sanity test_sanity.cpp)

# Module toolchain verification (F3): only meaningful with a scan-capable
# compiler (MacPorts clang-21). The `cpp` CMake preset selects it.
mg_add_module_test(test_modules
    SOURCES test_modules.cpp
    MODULES mg.probe.cppm)

# Core C++23 module tests (task C-series): import mg.io / mg.text.
add_executable(test_io test_io.cpp)
target_link_libraries(test_io PRIVATE doctest::doctest mg_io)
doctest_discover_tests(test_io)

add_executable(test_text test_text.cpp)
target_link_libraries(test_text PRIVATE doctest::doctest mg_text)
doctest_discover_tests(test_text)

add_executable(test_io_bridge test_io_bridge.cpp)
target_link_libraries(test_io_bridge PRIVATE doctest::doctest mg_io)
doctest_discover_tests(test_io_bridge)

add_executable(test_text_bridge test_text_bridge.cpp)
target_link_libraries(test_text_bridge PRIVATE doctest::doctest mg_text)
doctest_discover_tests(test_text_bridge)

add_executable(test_utf8 test_utf8.cpp)
target_link_libraries(test_utf8 PRIVATE doctest::doctest mg_utf8)
doctest_discover_tests(test_utf8)

# Magit module tests: import mg.magit from the mg_magit module library.
if(ENABLE_NATIVE_MAGIT)
    add_executable(test_magit test_magit.cpp)
    target_link_libraries(test_magit PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_magit)

    add_executable(test_fswatch test_fswatch.cpp)
    target_link_libraries(test_fswatch PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_fswatch)

    add_executable(test_coro test_coro.cpp)
    target_link_libraries(test_coro PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_coro)

    add_executable(test_git test_git.cpp)
    target_link_libraries(test_git PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_git)

    add_executable(test_bridge test_bridge.cpp)
    target_link_libraries(test_bridge PRIVATE doctest::doctest mg_magit)
    doctest_discover_tests(test_bridge)

    # Editor-level (pty) test: drives the built neomg binary to cover the C
    # command layer (src/magit_cmd.c) the engine tests can't reach. Needs the
    # binary path + forkpty (libutil on glibc; in libc on musl/macOS, so link
    # util only if a separate lib exists).
    add_executable(test_editor test_editor.cpp)
    target_link_libraries(test_editor PRIVATE doctest::doctest)
    find_library(UTIL_LIB util)
    if(UTIL_LIB)
        target_link_libraries(test_editor PRIVATE ${UTIL_LIB})
    endif()
    add_dependencies(test_editor mg) # the binary it drives
    target_compile_definitions(test_editor PRIVATE
        NEOMG_BINARY="$<TARGET_FILE:mg>")
    doctest_discover_tests(test_editor)

    # Manual status micro-benchmark (NOT registered with CTest). Links the magit
    # module so the README's numbers are reproducible:
    #   ./build/tests/neomg_bench <repo> [scoped-subdir]
    add_executable(neomg_bench ${CMAKE_SOURCE_DIR}/bench/status_bench.cpp)
    target_link_libraries(neomg_bench PRIVATE mg_magit)
    target_include_directories(neomg_bench PRIVATE ${CMAKE_SOURCE_DIR}/src/magit)
endif()
