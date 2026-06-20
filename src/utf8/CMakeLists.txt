# UTF-8 codepoint layer (task U1). Gated by ENABLE_CPP_UPGRADES at the root.
#
# A C++ wrapper over utf8proc (the light editor-grade Unicode lib), found via
# pkg-config -- same shape as mg.git wrapping libgit2. The C++20 module exports
# the codepoint API; bridge.cpp is a regular TU exposing the extern "C" surface
# the legacy C core will call (display/cursor, U2+). utf8proc is propagated
# PUBLIC so test binaries that link mg_utf8 get its headers/libs.
find_package(PkgConfig REQUIRED)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)

add_library(mg_utf8)
target_sources(mg_utf8 PUBLIC FILE_SET CXX_MODULES FILES utf8.cppm)
target_sources(mg_utf8 PRIVATE bridge.cpp)
target_include_directories(mg_utf8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mg_utf8 PUBLIC PkgConfig::UTF8PROC)
