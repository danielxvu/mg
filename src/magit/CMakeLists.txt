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

if(ENABLE_ZIG_STATUS)
    set(ZIG_DIR ${CMAKE_SOURCE_DIR}/src/zigstatus)
    set(ZIG_RAW_LIB ${ZIG_DIR}/zig-out/lib/libneomg_zig.a)

    # On macOS, Apple ld (ld64 ≥ 1267) rejects Zig-generated .a files because
    # the Zig archiver stores members with BSD long-name headers (#1/N) whose
    # 60-byte header + variable-length name leaves the Mach-O member data at an
    # offset that is not 8-byte aligned.  Fix: extract the single Mach-O .o,
    # merge it into one thin object via `ld -r`, then repack with macOS libtool
    # which produces a `__.SYMDEF SORTED` archive that Apple ld accepts.
    if(APPLE)
        set(ZIG_LIB ${CMAKE_CURRENT_BINARY_DIR}/libneomg_zig_repacked.a)
        # Repack script: Apple ld (ld64 ≥ 1267) rejects the Zig .a because the
        # member data is not 8-byte aligned (BSD #1/N headers leave it at
        # offset 184).  Fix: ar-extract -> ld -r merge -> libtool repack.
        # The repack shell script is written into the build tree at configure
        # time so the custom command can invoke it as a single argument.
        set(ZIG_REPACK_DIR ${CMAKE_CURRENT_BINARY_DIR}/zig_repack)
        set(ZIG_REPACK_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/zig_repack_apple.sh)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE _ZIG_HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(GENERATE OUTPUT ${ZIG_REPACK_SCRIPT} CONTENT
"#!/bin/sh
set -e
ZIG_EXE='${ZIG_EXE}'
ZIG_DIR='${ZIG_DIR}'
ZIG_RAW_LIB='${ZIG_RAW_LIB}'
ZIG_REPACK_DIR='${ZIG_REPACK_DIR}'
ZIG_LIB='${ZIG_LIB}'
\"$ZIG_EXE\" build -Doptimize=ReleaseFast -p zig-out
rm -rf \"$ZIG_REPACK_DIR\"
mkdir -p \"$ZIG_REPACK_DIR\"
cd \"$ZIG_REPACK_DIR\"
ar x \"$ZIG_RAW_LIB\"
chmod +r ./*.o
ld -r -arch ${_ZIG_HOST_ARCH} ./*.o -o ./merged_zig.o
libtool -static -o \"$ZIG_LIB\" ./merged_zig.o
"
            FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                             GROUP_READ GROUP_EXECUTE
                             WORLD_READ WORLD_EXECUTE)
        add_custom_command(
            OUTPUT ${ZIG_LIB}
            COMMAND ${ZIG_REPACK_SCRIPT}
            WORKING_DIRECTORY ${ZIG_DIR}
            DEPENDS ${ZIG_DIR}/src/lib.zig ${ZIG_DIR}/src/status.zig
                    ${ZIG_DIR}/src/walk.zig ${ZIG_DIR}/src/index.zig
                    ${ZIG_DIR}/src/gitignore.zig ${ZIG_DIR}/src/sha1.zig
                    ${ZIG_DIR}/build.zig
            COMMENT "Building + repacking Zig status lib for Apple ld (libneomg_zig_repacked.a)")
    else()
        set(ZIG_LIB ${ZIG_RAW_LIB})
        add_custom_command(
            OUTPUT ${ZIG_LIB}
            COMMAND ${ZIG_EXE} build -Doptimize=ReleaseFast
            WORKING_DIRECTORY ${ZIG_DIR}
            DEPENDS ${ZIG_DIR}/src/lib.zig ${ZIG_DIR}/src/status.zig
                    ${ZIG_DIR}/src/walk.zig ${ZIG_DIR}/src/index.zig
                    ${ZIG_DIR}/src/gitignore.zig ${ZIG_DIR}/src/sha1.zig
                    ${ZIG_DIR}/build.zig
            COMMENT "Building Zig status lib (libneomg_zig.a)")
    endif()

    add_custom_target(neomg_zig_lib DEPENDS ${ZIG_LIB})
    add_dependencies(mg_magit neomg_zig_lib)
    target_link_libraries(mg_magit PRIVATE ${ZIG_LIB})
    target_include_directories(mg_magit PRIVATE ${ZIG_DIR}/include)
    target_compile_definitions(mg_magit PRIVATE MG_ZIG_STATUS)
endif()
