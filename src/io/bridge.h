/*
 * bridge.h -- extern "C" surface of the mg.io file-IO layer (task C3.5).
 *
 * The legacy C core (fileio.c) includes this to route a file-system query
 * through the modern std::expected layer (bridge.cpp) without any C++ type
 * crossing the boundary. Gated on the C side by ENABLE_CPP_UPGRADES.
 */
#ifndef MG_IO_BRIDGE_H
#define MG_IO_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Is `path` a directory?  1 = directory, 0 = not a directory, -1 = stat error
 * (mirrors fisdir's TRUE/FALSE/ABORT trichotomy). */
int mg_io_isdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MG_IO_BRIDGE_H */
