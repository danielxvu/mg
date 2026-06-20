# Core C++23 file-IO module (task C3). Gated by ENABLE_CPP_UPGRADES at the root.
#
# Built as a module library so test binaries (and, later, an extern "C" bridge
# into fileio.c) can `import mg.io`. The C++20 module interface is exported via
# a PUBLIC CXX_MODULES file set so linking targets get the BMI scan.
add_library(mg_io)
target_sources(mg_io PUBLIC FILE_SET CXX_MODULES FILES io.cppm)
target_include_directories(mg_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
