# Core C++23 pure-text module (task C1). Gated by ENABLE_CPP_UPGRADES at the root.
#
# All-constexpr leaf utilities (character classification + tab-stop math). Built
# as a module library so test binaries (and a future C-core consumer) can
# `import mg.text`.
add_library(mg_text)
target_sources(mg_text PUBLIC FILE_SET CXX_MODULES FILES text.cppm)
# bridge.cpp is a regular TU (imports mg.text) exposing the extern "C" API in
# bridge.h to the legacy C core's chrdef.h macros (task C1.5).
target_sources(mg_text PRIVATE bridge.cpp)
target_include_directories(mg_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
