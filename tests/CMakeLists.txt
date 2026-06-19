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

# Helper so every future module's test binary is wired up the same way.
function(mg_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE doctest::doctest)
    doctest_discover_tests(${name})
endfunction()

mg_add_test(test_sanity test_sanity.cpp)
