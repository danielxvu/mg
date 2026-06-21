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

/* Line kinds passed to the emit callback, so the caller can map a buffer line
 * back to a file (and hunk) for staging. `path` is NULL for non-file lines;
 * `hunk` is the 0-based hunk index for HUNK/DIFF lines, else -1. */
#define MG_LINE_OTHER     0
#define MG_LINE_UNTRACKED 1
#define MG_LINE_UNSTAGED  2
#define MG_LINE_STAGED    3
#define MG_LINE_HUNK      4   /* a hunk header under an expanded file */
#define MG_LINE_DIFF      5   /* a +/-/context diff line within a hunk */
#define MG_LINE_STASH     6   /* a stash entry (stash@{N}) */
#define MG_LINE_BRANCH    7   /* a local branch */
#define MG_LINE_SECTION   8   /* a section header line (for M-n/M-p nav) */

/* Compose the *magit-status* buffer for the repo at `repo_path`, calling
 * `emit(ctx, line, kind, path, hunk)` once per line. Files whose path is in
 * `expanded[0..n_expanded)` have their diff hunks emitted inline. Returns the
 * number of lines emitted. A one-shot read (independent of the monitor). */
typedef void (*mg_magit_emit_fn)(void *ctx, const char *line, int kind,
                                 const char *path, int hunk);
int mg_magit_status_buffer(const char *repo_path, const char *const *expanded,
                           int n_expanded, mg_magit_emit_fn emit, void *ctx);

/* Stage / unstage / discard a single file (path relative to the repo root).
 * Returns 1 on success, 0 on failure. Discard deletes an untracked file or
 * reverts a tracked one to HEAD. */
int mg_magit_stage(const char *repo_path, const char *path);
int mg_magit_unstage(const char *repo_path, const char *path);
int mg_magit_discard(const char *repo_path, const char *path);

/* Stage every change / unstage everything (the S and U keys). 1 ok, 0 fail. */
int mg_magit_stage_all(const char *repo_path);
int mg_magit_unstage_all(const char *repo_path);

/* Commit the staged tree with `message`. Returns 1 on success, 0 on failure
 * (e.g. user.name/user.email unset, or nothing staged). */
int mg_magit_commit(const char *repo_path, const char *message);

/* Amend HEAD with the staged tree + `message`; extend = keep message, add
 * staged; reword = change only the message. Return 1 on success, 0 on failure. */
int mg_magit_commit_amend(const char *repo_path, const char *message);
int mg_magit_commit_extend(const char *repo_path);
int mg_magit_commit_reword(const char *repo_path, const char *message);

/* Copy HEAD's full commit message into `buf` (NUL-terminated); returns length,
 * or 0 if there is no HEAD / on error. (For pre-filling an amend/reword buffer.) */
int mg_magit_head_message(const char *repo_path, char *buf, size_t buflen);

/* Stage / unstage a single hunk (0-based `hunk`, as numbered in the rendered
 * diff) of `path`. Returns 1 on success, 0 on failure. */
int mg_magit_stage_hunk(const char *repo_path, const char *path, int hunk);
int mg_magit_unstage_hunk(const char *repo_path, const char *path, int hunk);

/* Stage / unstage only the lines [first, last] (0-based, inclusive, as numbered
 * within hunk `hunk` of the rendered diff) of `path` -- magit line/region
 * staging. Returns 1 on success, 0 on failure. */
int mg_magit_stage_region(const char *repo_path, const char *path, int hunk,
                          int first, int last);
int mg_magit_unstage_region(const char *repo_path, const char *path, int hunk,
                            int first, int last);

/* Apply (reapply, keep) / drop (delete) stash number `index`. Returns 1 on
 * success, 0 on failure. */
int mg_magit_stash_apply(const char *repo_path, int index);
int mg_magit_stash_drop(const char *repo_path, int index);

/* Create a stash from the working tree (`message` may be NULL) / pop stash
 * `index` (apply then drop). Return 1 on success, 0 on failure. */
int mg_magit_stash_push(const char *repo_path, const char *message);
int mg_magit_stash_pop(const char *repo_path, int index);

/* Check out local branch `name` (moves HEAD + updates the worktree). Returns
 * 1 on success, 0 on failure (e.g. conflicting local changes). */
int mg_magit_checkout(const char *repo_path, const char *name);

/* Create / delete / rename a local branch. create makes `name` at HEAD without
 * switching; rename moves `from` to `to`. Return 1 on success, 0 on failure. */
int mg_magit_branch_create(const char *repo_path, const char *name);
int mg_magit_branch_delete(const char *repo_path, const char *name);
int mg_magit_branch_rename(const char *repo_path, const char *from,
                           const char *to);

#ifdef __cplusplus
}
#endif

#endif /* MG_MAGIT_BRIDGE_H */
