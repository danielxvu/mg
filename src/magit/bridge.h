/*
 * bridge.h -- extern "C" surface of the native magit engine (task M2d).
 *
 * The legacy C core (main.c, display.c) includes this header; the C++ side
 * (bridge.cpp) implements it over a background monitor thread. No C++ types
 * cross the boundary -- the modeline getter fills a caller-owned buffer.
 */
#ifndef MG_MAGIT_BRIDGE_H
#define MG_MAGIT_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the background monitor for the repository at `repo_path`. */
void mg_magit_start(const char *repo_path);

/* Stop the monitor and join its thread. Safe to call when not started. */
void mg_magit_stop(void);

/* Read-and-clear the "status changed" flag (mirrors winch_flag). */
int mg_magit_take_dirty(void);

/* Copy the current modeline string into `buf` (NUL-terminated); returns its
 * length. Returns 0 when no monitor is running. */
int mg_magit_modeline(char *buf, size_t buflen);

/* Compose the *magit-status* buffer for the repo at `repo_path`, calling
 * `emit(ctx, line)` once per line. Returns the number of lines emitted. This is
 * a one-shot read (independent of the background monitor). */
typedef void (*mg_magit_emit_fn)(void *ctx, const char *line);
int mg_magit_status_buffer(const char *repo_path, mg_magit_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MG_MAGIT_BRIDGE_H */
