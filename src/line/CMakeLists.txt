# C++ line storage (task C2b-2). Gated by ENABLE_CPP_UPGRADES at the root.
#
# A plain C++ static library (no module needed -- the surface is the extern "C"
# accessors in def.h + bridge.h): storage.cpp defines lalloc/lrealloc/lfreestore
# and the 11 line accessors over a std::vector<char>-backed `struct line`, which
# the legacy line.c uses in place of its malloc/realloc storage.
add_library(mg_line)
target_sources(mg_line PRIVATE storage.cpp)
target_include_directories(mg_line PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
