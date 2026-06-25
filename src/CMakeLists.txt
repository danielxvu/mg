# ---------------------------------------------------------------------------
# Legacy mg editor -- faithful plain-C build (task F2).
#
# Reproduces the autotools build: feature-detects the host (mirroring
# configure.ac), generates config.h, compiles the default source set plus any
# lib/ fallbacks for missing libc functions, and links a termcap/curses lib.
# Built in every configuration; under ENABLE_CPP_UPGRADES=OFF this is the whole
# program, exactly as upstream.
# ---------------------------------------------------------------------------
include(CheckIncludeFile)
include(CheckFunctionExists)
include(GNUInstallDirs)

set(MG_VERSION "3.8-pre")

# --- Optional features (configure.ac AC_ARG_ENABLE; default on) -------------
option(ENABLE_AUTOEXEC     "Auto-execute support"            ON)
option(ENABLE_CMODE        "C-mode support"                  ON)
option(ENABLE_COMPILE_GREP "compile & grep mode (M-x compile)" ON)
option(CSCOPE              "Cscope support"                  ON)
option(ENABLE_CTAGS        "ctags(1) support"                ON)
option(ENABLE_DIRED        "directory editor (dired)"        ON)
option(REGEX               "full regexp search"              ON)
option(TOGGLENL            "toggle-newline-prompt extension" ON)

# --- Host feature detection (mirrors configure.ac) --------------------------
check_include_file(pty.h            HAVE_PTY_H)
check_include_file(utmp.h           HAVE_UTMP_H)
check_include_file(term.h           HAVE_TERM_H)
check_include_file(ncurses/curses.h HAVE_NCURSES_CURSES_H)

# AC_CHECK_LIB([util], …): Linux keeps openpty/login_tty/fparseln in libutil;
# macOS/BSD have them in libc. Add it to the link-check set when present.
find_library(UTIL_LIB util)
if(UTIL_LIB)
    list(APPEND CMAKE_REQUIRED_LIBRARIES ${UTIL_LIB})
endif()

# AC_REPLACE_FUNCS: link-test each replaceable function; pull in lib/<fn>.c for
# every one the host lacks (on macOS that is just reallocarray).
set(MG_LIBOBJ "")
foreach(fn fparseln futimens login_tty openpty reallocarray strlcpy strlcat strtonum)
    string(TOUPPER ${fn} FN)
    check_function_exists(${fn} HAVE_${FN})
    if(NOT HAVE_${FN})
        list(APPEND MG_LIBOBJ ${CMAKE_SOURCE_DIR}/lib/${fn}.c)
    endif()
endforeach()

# AC_CHECK_LIB termcap cascade: ncurses -> curses -> tinfo -> termcap.
find_library(CURSES_LIB NAMES ncurses curses tinfo termcap)
if(NOT CURSES_LIB)
    set(WITHOUT_CURSES 1)
endif()

set(MG_STARTUPFILE "${CMAKE_INSTALL_FULL_SYSCONFDIR}/mg")
configure_file(${CMAKE_SOURCE_DIR}/cmake/config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/include/config.h @ONLY)

# --- Source set (src/Makefile.am mg_SOURCES + conditional extensions) -------
set(MG_SOURCES
    basic.c bell.c buffer.c cinfo.c dir.c display.c echo.c extend.c file.c
    fileio.c funmap.c help.c interpreter.c kbd.c keymap.c line.c macro.c main.c
    match.c modes.c paragraph.c region.c search.c spawn.c tty.c ttyio.c
    ttykbd.c undo.c util.c version.c window.c word.c yank.c extensions.c)

if(ENABLE_AUTOEXEC)
    list(APPEND MG_SOURCES autoexec.c)
endif()
if(ENABLE_CMODE)
    list(APPEND MG_SOURCES cmode.c)
endif()
if(ENABLE_COMPILE_GREP)
    list(APPEND MG_SOURCES grep.c)
endif()
if(CSCOPE)
    list(APPEND MG_SOURCES cscope.c)
endif()
if(ENABLE_CTAGS)
    list(APPEND MG_SOURCES tags.c)
endif()
if(ENABLE_DIRED)
    list(APPEND MG_SOURCES dired.c)
endif()
if(REGEX)
    list(APPEND MG_SOURCES re_search.c)
endif()
if(WITHOUT_CURSES)
    list(APPEND MG_SOURCES ansi.c)
endif()

# The native-magit command glue (calls the extern "C" bridge). Linked into mg
# and ENABLE_NATIVE_MAGIT-defined from the root, alongside libmg_magit.
if(ENABLE_NATIVE_MAGIT)
    list(APPEND MG_SOURCES magit_cmd.c)
endif()

add_executable(mg ${MG_SOURCES} ${MG_LIBOBJ})

# The fork ships as `neomg` so it installs side-by-side with a distro's stock
# `mg`. Only the artifact is renamed -- the CMake target stays `mg` (so every
# target_*(mg ...) reference and the COMMAND mg tests keep resolving). The
# autotools build (build.yml) is left as the faithful upstream-parity `mg`.
set_target_properties(mg PROPERTIES OUTPUT_NAME neomg)

# config.h lives in the build tree; def.h's `#include "config.h"` finds it here.
# src/ is also needed so the lib/ compat shims (MG_LIBOBJ: fparseln, strtonum,
# … -- compiled only where libc lacks them, e.g. Linux/musl, never on macOS)
# can resolve their `#include "def.h"`.
target_include_directories(mg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include)

# Upstream mg_CFLAGS (src/Makefile.am).
target_compile_options(mg PRIVATE -W -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(mg PRIVATE
    _GNU_SOURCE
    "DATADIR=\"${CMAKE_INSTALL_FULL_DATADIR}/mg\""
    "DOCDIR=\"${CMAKE_INSTALL_FULL_DOCDIR}\"")

if(CURSES_LIB)
    target_link_libraries(mg PRIVATE ${CURSES_LIB})
endif()
if(UTIL_LIB)
    target_link_libraries(mg PRIVATE ${UTIL_LIB})
endif()

# --- Install: the editor binary + its man page (GNUInstallDirs layout) -------
# `cmake --install <build> --prefix <p>` lays down <p>/bin/neomg and
# <p>/share/man/man1/neomg.1 (installed side-by-side with a distro's stock mg).
# The startup file is read from SYSCONFDIR/mg at runtime (STARTUPFILE in
# config.h); it is user/distro-provided, not shipped.
install(TARGETS mg RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_SOURCE_DIR}/doc/mg.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 RENAME neomg.1)

# --- Smoke test (task F2): `mg -h` prints usage and exits 0, no tty needed ---
if(BUILD_TESTING)
    add_test(NAME mg_help_exits_zero   COMMAND mg -h)
    add_test(NAME mg_help_prints_usage COMMAND mg -h)
    set_tests_properties(mg_help_prints_usage
        PROPERTIES PASS_REGULAR_EXPRESSION "usage:")
endif()
