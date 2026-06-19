# Native Magit engine (task M-series). Gated by ENABLE_NATIVE_MAGIT, which the
# root build already constrains to require ENABLE_CPP_UPGRADES (a C++ module).
#
# Built as a module library so the eventual extern "C" modeline bridge and the
# M2 coroutine poller can link it. Its C++20 module is exported via a PUBLIC
# CXX_MODULES file set so linking targets (e.g. test_magit) can `import mg.magit`.
add_library(mg_magit)
target_sources(mg_magit
    PUBLIC FILE_SET CXX_MODULES FILES coro.cppm magit.cppm fswatch.cppm)
