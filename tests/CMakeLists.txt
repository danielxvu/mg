# Unit-test suite for the modern C++23 modules.
#
# doctest is fetched as a lightweight, single-header framework: it compiles fast
# (important for a tight TDD / Ralph-loop cadence) and has zero runtime deps.
# It is pinned to a release tag for reproducibility across fresh agent sessions.
include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
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
endif()
