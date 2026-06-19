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
target_link_libraries(mg_magit PUBLIC PkgConfig::GIT2)
