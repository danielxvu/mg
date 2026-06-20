# ---------------------------------------------------------------------------
# mg -- modern C++ refactor build system
#
# This CMake project is the scaffolding for an incremental, behaviour-preserving
# refactor of the upstream `mg` (MicroGnuEmacs) C editor into modern C++23.
#
# Architectural contract (kept faithful as tasks land -- see todo.md):
#
#   ENABLE_CPP_UPGRADES = OFF (default)
#       The project builds exactly like upstream plain C: the legacy `mg`
#       binary, no C++ involved. (The faithful C build target itself is a
#       follow-up task; this bootstrap only wires the option and the toolchain.)
#
#   ENABLE_CPP_UPGRADES = ON
#       The modern C++23 modules and their unit tests are compiled in. New code
#       bridges to the legacy terminal/layout layer via `extern "C"` shims so
#       the C core is never polluted.
#
#   ENABLE_NATIVE_MAGIT = OFF (default)
#       Links the isolated C++ coroutine-based Git DAG engine under src/magit/.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

project(mg
    VERSION 3.8
    DESCRIPTION "MicroGnuEmacs -- incremental modern-C++ refactor"
    LANGUAGES C CXX)

# --- Architectural feature gates -------------------------------------------
option(ENABLE_CPP_UPGRADES "Compile the modern C++23 modules instead of plain C" OFF)
option(ENABLE_NATIVE_MAGIT "Link the native C++ Git DAG engine (src/magit/)"     OFF)

# --- Language standards ------------------------------------------------------
# The C core stays C11 to match upstream's expectations; the new engine targets
# C++23 (std::expected, monadic optional, ranges, modules).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Honour BUILD_TESTING (defaults ON) via CTest.
include(CTest)

# The legacy mg editor (faithful plain C). Built in every configuration; it is
# the entire program when ENABLE_CPP_UPGRADES=OFF, and registers the `mg -h`
# smoke test under BUILD_TESTING regardless of the upgrades flag.
add_subdirectory(src)

if(ENABLE_NATIVE_MAGIT AND NOT ENABLE_CPP_UPGRADES)
    message(FATAL_ERROR
        "ENABLE_NATIVE_MAGIT requires ENABLE_CPP_UPGRADES=ON (it is a C++ engine).")
endif()

# Core C++23 modules (task C-series). Built whenever the upgrades are on --
# independent of the native-magit feature, since these replace legacy core
# subsystems. mg.io is a standalone, tested module (not yet wired into the C
# core), so it only needs to exist for the test suite to link it.
if(ENABLE_CPP_UPGRADES)
    add_subdirectory(src/io)
    add_subdirectory(src/text)

    # Wire mg.io into the legacy core: fileio.c's fisdir routes through the
    # mg_io extern "C" bridge under #ifdef ENABLE_CPP_UPGRADES (C3.5). mg now
    # links a C++ library, so the final link uses the C++ driver.
    target_link_libraries(mg PRIVATE mg_io)
    target_compile_definitions(mg PRIVATE ENABLE_CPP_UPGRADES)
    set_target_properties(mg PROPERTIES LINKER_LANGUAGE CXX)
endif()

# Native Git engine (C++20 modules). Configured before tests so its module
# library exists when the test suite links test_magit against it.
if(ENABLE_NATIVE_MAGIT)
    add_subdirectory(src/magit)

    # Link the engine into the legacy mg binary and switch on its C-core hooks
    # (main.c lifecycle/dirty-check, display.c modeline). mg now links a C++
    # library, so the final link must use the C++ driver.
    target_link_libraries(mg PRIVATE mg_magit)
    target_compile_definitions(mg PRIVATE ENABLE_NATIVE_MAGIT)
    set_target_properties(mg PROPERTIES LINKER_LANGUAGE CXX)
endif()

# The C++ unit-test suite exercises the new C++23 modules, so it is only
# meaningful when the upgrades are switched on.
if(ENABLE_CPP_UPGRADES AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
