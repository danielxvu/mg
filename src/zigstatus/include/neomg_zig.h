#ifndef NEOMG_ZIG_H
#define NEOMG_ZIG_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*neomg_zig_emit_fn)(void *ctx, const char *path, size_t path_len, char x, char y);
int neomg_zig_worktree_status(const char *repo, size_t repo_len, neomg_zig_emit_fn emit, void *ctx);
#ifdef __cplusplus
}
#endif
#endif
