# Native Magit engine (task M-series). Gated by ENABLE_NATIVE_MAGIT, which the
# root build already constrains to require ENABLE_CPP_UPGRADES (a C++ module).
#
# Built as a module library so the eventual extern "C" modeline bridge and the
# M2 coroutine poller can link it. Its C++20 module is exported via a PUBLIC
# CXX_MODULES file set so linking targets (e.g. test_magit) can `import mg.magit`.
# libgit2 backs the status reader (mg.git). Found via pkg-config; the imported
# target carries include dirs + libs, propagated PUBLIC so test binaries that
# include <git2.h> and link mg_magit get them too.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIT2 REQUIRED IMPORTED_TARGET libgit2)

add_library(mg_magit)
target_sources(mg_magit
    PUBLIC FILE_SET CXX_MODULES FILES coro.cppm magit.cppm fswatch.cppm git.cppm)
# bridge.cpp is a regular TU (imports the modules above) exposing the extern "C"
# API in bridge.h to the legacy C core.
target_sources(mg_magit PRIVATE bridge.cpp)
target_include_directories(mg_magit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}) # bridge.h
target_link_libraries(mg_magit PUBLIC PkgConfig::GIT2)
find_package(Threads REQUIRED)
target_link_libraries(mg_magit PUBLIC Threads::Threads)

# mg.fswatch uses FSEvents (CoreServices) on macOS by default. Harmless to link
# even when MG_FSWATCH_FORCE_KQUEUE compiles the kqueue backend instead (no
# CoreServices symbols are referenced then). libdispatch is in libSystem.
if(APPLE)
    target_link_libraries(mg_magit PUBLIC "-framework CoreServices")
endif()

# --- Zig static libraries (shared build/link helper) -----------------------
# Both the FM-SYNTAX-HIGHLIGHT diff tokenizer (built whenever the native engine
# is) and the optional FM-ZIG hybrid status walker link a Zig 0.16 static lib.
# The helper finds zig once and handles the macOS Apple-ld repack uniformly
# (see cmake/AddZigLibrary.cmake) -- no per-feature copy of the integration.
include(${CMAKE_SOURCE_DIR}/cmake/AddZigLibrary.cmake)

# FM-SYNTAX-HIGHLIGHT: the Zig diff tokenizer (a core part of the native engine).
set(SYNTAX_DIR ${CMAKE_SOURCE_DIR}/syntax)
neomg_add_zig_library(neomg_syntax_lib
    ZIG_DIR ${SYNTAX_DIR}
    RAW_LIB ${SYNTAX_DIR}/zig-out/lib/libneomg_syntax.a
    OUTPUT  SYNTAX_LIB
    DEPENDS ${SYNTAX_DIR}/neomg_syntax.zig ${SYNTAX_DIR}/build.zig)
add_dependencies(mg_magit neomg_syntax_lib)
target_link_libraries(mg_magit PUBLIC ${SYNTAX_LIB})
target_include_directories(mg_magit PUBLIC ${SYNTAX_DIR})

# FM-ZIG: the optional hybrid cold-status walker (default-on in the presets).
if(ENABLE_ZIG_STATUS)
    set(ZIG_DIR ${CMAKE_SOURCE_DIR}/src/zigstatus)
    neomg_add_zig_library(neomg_zig_lib
        ZIG_DIR ${ZIG_DIR}
        RAW_LIB ${ZIG_DIR}/zig-out/lib/libneomg_zig.a
        OUTPUT  ZIG_LIB
        DEPENDS ${ZIG_DIR}/src/lib.zig ${ZIG_DIR}/src/status.zig
                ${ZIG_DIR}/src/walk.zig ${ZIG_DIR}/src/index.zig
                ${ZIG_DIR}/src/gitignore.zig ${ZIG_DIR}/src/sha1.zig
                ${ZIG_DIR}/build.zig)
    add_dependencies(mg_magit neomg_zig_lib)
    target_link_libraries(mg_magit PRIVATE ${ZIG_LIB})
    target_include_directories(mg_magit PRIVATE ${ZIG_DIR}/include)
    target_compile_definitions(mg_magit PUBLIC MG_ZIG_STATUS)
endif()
