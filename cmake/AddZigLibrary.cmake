# AddZigLibrary.cmake -- build a Zig 0.16 static library and link it into a C++
# target, uniformly across platforms. Shared by the FM-ZIG status walker
# (src/zigstatus) and the FM-SYNTAX-HIGHLIGHT tokenizer (syntax/), so the Zig
# toolchain integration lives in exactly one place instead of being reinvented
# per feature.
#
# The one platform wrinkle this hides: on macOS, Apple ld (ld64 >= 1267)
# rejects some Zig-produced .a archives because a member lands at a
# non-8-byte-aligned offset (Zig stores members under BSD long-name headers
# `#1/N`; a long member name -- e.g. the status lib's `libneomg_zig_zcu.o` --
# pushes the Mach-O data off an 8-byte boundary). Whether a given archive trips
# this depends on its member names, so rather than guess per-lib we always
# repack on macOS: extract the objects, merge them into one thin object with
# `ld -r`, and repack with libtool into a `__.SYMDEF SORTED` archive Apple ld
# accepts. The repack is a no-op in effect for already-aligned archives, so it
# is safe to apply uniformly. On Linux the Zig .a links directly.
#
#   neomg_add_zig_library(<custom-target-name>
#       ZIG_DIR <dir>          # directory containing build.zig
#       RAW_LIB <path>         # the lib<name>.a that `zig build` emits
#                              #   (typically <ZIG_DIR>/zig-out/lib/lib<name>.a)
#       OUTPUT  <out-var>      # set in the caller to the linkable .a path
#       DEPENDS <files...>)    # Zig sources that should retrigger the build
#
# After calling, add a dependency on <custom-target-name> and link ${<out-var>}.

function(neomg_add_zig_library TARGET)
    cmake_parse_arguments(ZL "" "ZIG_DIR;RAW_LIB;OUTPUT" "DEPENDS" ${ARGN})

    # Locate the Zig 0.16 toolchain lazily -- only when a lib is actually
    # registered -- so merely including this module never forces a Zig
    # dependency. find_program caches, so repeated calls are free.
    find_program(ZIG_EXE zig
        HINTS /opt/local/bin /opt/homebrew/bin /usr/local/bin REQUIRED)

    if(APPLE)
        set(_lib ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_repacked.a)
        set(_repack_dir ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_repack)
        set(_script ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_repack.sh)
        execute_process(COMMAND uname -m
            OUTPUT_VARIABLE _arch OUTPUT_STRIP_TRAILING_WHITESPACE)
        # The repack runs as one script so the custom command is a single
        # argument; ${ZIG_EXE}/paths are baked in at configure time.
        file(GENERATE OUTPUT ${_script} CONTENT
"#!/bin/sh
set -e
\"${ZIG_EXE}\" build -Doptimize=ReleaseFast -p zig-out
rm -rf \"${_repack_dir}\"
mkdir -p \"${_repack_dir}\"
cd \"${_repack_dir}\"
ar x \"${ZL_RAW_LIB}\"
chmod +r ./*.o
ld -r -arch ${_arch} ./*.o -o ./merged.o
libtool -static -o \"${_lib}\" ./merged.o
"
            FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                             GROUP_READ GROUP_EXECUTE
                             WORLD_READ WORLD_EXECUTE)
        add_custom_command(
            OUTPUT ${_lib}
            COMMAND ${_script}
            WORKING_DIRECTORY ${ZL_ZIG_DIR}
            DEPENDS ${ZL_DEPENDS}
            COMMENT "Building + repacking Zig lib for Apple ld (${TARGET})")
    else()
        set(_lib ${ZL_RAW_LIB})
        add_custom_command(
            OUTPUT ${_lib}
            COMMAND ${ZIG_EXE} build -Doptimize=ReleaseFast -p zig-out
            WORKING_DIRECTORY ${ZL_ZIG_DIR}
            DEPENDS ${ZL_DEPENDS}
            COMMENT "Building Zig lib (${TARGET})")
    endif()

    add_custom_target(${TARGET} DEPENDS ${_lib})
    set(${ZL_OUTPUT} ${_lib} PARENT_SCOPE)
endfunction()
