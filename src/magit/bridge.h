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
#define MG_LINE_COMMIT    9   /* a commit line in the log (path = full oid) */
#define MG_LINE_TAG       10  /* a tag entry (path = tag name) */
#define MG_LINE_WORKTREE  11  /* a worktree entry (path = worktree name) */

/* Compose the *magit-status* buffer for the repo at `repo_path`, calling
 * `emit(ctx, line, kind, path, hunk)` once per line. Files whose path is in
 * `expanded[0..n_expanded)` have their diff hunks emitted inline. Returns the
 * number of lines emitted. A one-shot read (independent of the monitor). */
typedef void (*mg_magit_emit_fn)(void *ctx, const char *line, int kind,
                                 const char *path, int hunk);
int mg_magit_status_buffer(const char *repo_path, const char *const *expanded,
                           int n_expanded, mg_magit_emit_fn emit, void *ctx);

/* Compose the *magit-log* buffer: up to `n` recent commits (newest first), each
 * emitted as one MG_LINE_COMMIT line whose `path` is the commit's full oid.
 * Returns the number of lines emitted. */
int mg_magit_log_buffer(const char *repo_path, int n, mg_magit_emit_fn emit,
                        void *ctx);

/* Compose a *magit-commit* buffer for commit `rev`: a header line then its diff
 * (MG_LINE_HUNK / MG_LINE_DIFF). Returns the number of lines emitted, 0 on a
 * bad rev. */
int mg_magit_commit_diff(const char *repo_path, const char *rev,
                         mg_magit_emit_fn emit, void *ctx);

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

/* Discard only the lines [first, last] of `path`'s unstaged hunk `hunk` from
 * the working tree (revert to the index). Destructive. 1 ok, 0 fail. */
int mg_magit_discard_region(const char *repo_path, const char *path, int hunk,
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

/* Prompt the user for a credential during fetch/push: fill `out` (size `outlen`)
 * with the answer to `prompt`; `hidden` != 0 requests non-echoing input (a
 * password). Return 1 on success, 0 to cancel. */
typedef int (*mg_magit_cred_prompt_fn)(const char *prompt, int hidden,
                                       char *out, int outlen);

/* Register the credential prompt used for HTTPS user/password auth (NULL ->
 * ssh-agent only). Call once at startup. */
void mg_magit_set_cred_prompt(mg_magit_cred_prompt_fn fn);

/* Fetch / push (current branch) / pull (fetch + merge) against `remote`
 * (NULL -> "origin"). Return 1 on success, 0 on failure. ssh uses the agent;
 * HTTPS user/password prompts via mg_magit_set_cred_prompt's callback. */
int mg_magit_fetch(const char *repo_path, const char *remote);
int mg_magit_push(const char *repo_path, const char *remote, int force,
                  int set_upstream);
int mg_magit_pull(const char *repo_path, const char *remote);
/* Pull --rebase: fetch then rebase onto the tracking ref. 1 done / 2 paused on
 * conflict / 0 fail. */
int mg_magit_pull_rebase(const char *repo_path, const char *remote);

/* Reset HEAD to `rev` (mode 0=soft, 1=mixed, 2=hard) / revert commit `rev`
 * (records the inverse on HEAD) / merge local branch `name` into HEAD (ff or a
 * merge commit; conflicts abort). Return 1 on success, 0 on failure. */
int mg_magit_reset(const char *repo_path, const char *rev, int mode);
int mg_magit_revert(const char *repo_path, const char *rev);
int mg_magit_merge(const char *repo_path, const char *name);

/* Cherry-pick commit `rev` onto HEAD (new commit, keeps author + message).
 * 1 ok, 0 fail (incl. conflict, which leaves the repo untouched). */
int mg_magit_cherrypick(const char *repo_path, const char *rev);

/* Rebase the current branch onto `upstream` (a branch name / revspec, e.g.
 * "@{u}"). Returns 1 = done, 2 = paused on conflicts (resolve + continue),
 * 0 = failure. */
int mg_magit_rebase(const char *repo_path, const char *upstream);

/* Continue / skip / abort a paused rebase (1 done, 2 still-conflicts for
 * continue/skip, 0 fail; abort: 1 ok / 0 fail). Whether a rebase is in
 * progress -> 1/0. */
int mg_magit_rebase_continue(const char *repo_path);
int mg_magit_rebase_skip(const char *repo_path);
int mg_magit_rebase_abort(const char *repo_path);
int mg_magit_rebase_in_progress(const char *repo_path);

/* One interactive-rebase plan step: `action` 0=pick, 1=drop, 2=squash,
 * 3=fixup, 4=reword; `oid` is the commit's full sha-1 hex; `message` is the new
 * message for reword (NULL otherwise). */
struct mg_magit_rebase_step {
	int action;
	const char *oid;
	const char *message;
};

/* Emit the commits an interactive rebase onto `onto` would replay -- one
 * MG_LINE_COMMIT line "shortoid summary" each (oldest first), full oid in
 * `path`. Returns the count; 0 on error. (Seeds the *git-rebase-todo* buffer.) */
int mg_magit_rebase_todo(const char *repo_path, const char *onto,
                         mg_magit_emit_fn emit, void *ctx);

/* Interactive rebase: replay `steps[0..n)` (oldest first) onto `onto`, then
 * move the current branch to the result. Returns 1 on success, 0 on failure
 * (incl. conflict, which leaves the repo untouched). reword/edit unsupported. */
int mg_magit_rebase_interactive(const char *repo_path, const char *onto,
                                const struct mg_magit_rebase_step *steps, int n);

/* Create / delete / rename a local branch. create makes `name` at HEAD without
 * switching; rename moves `from` to `to`. Return 1 on success, 0 on failure. */
int mg_magit_branch_create(const char *repo_path, const char *name);
int mg_magit_branch_delete(const char *repo_path, const char *name);
int mg_magit_branch_rename(const char *repo_path, const char *from,
                           const char *to);

/* Create a tag `name` at `target` (revspec, NULL -> "HEAD"); a non-NULL/
 * non-empty `message` makes it annotated, else lightweight. / delete tag
 * `name`. Return 1 on success, 0 on failure. */
int mg_magit_tag_create(const char *repo_path, const char *name,
                        const char *target, const char *message);
int mg_magit_tag_delete(const char *repo_path, const char *name);

/* Append `pattern` to the repo's top-level .gitignore. 1 ok, 0 fail. */
int mg_magit_ignore(const char *repo_path, const char *pattern);

/* Emit a blame of `path`: one "<oid> <author> <line>" per source line. Returns
 * the line count, 0 on error. (Seeds the *magit-blame* buffer.) */
int mg_magit_blame_file(const char *repo_path, const char *path,
                        mg_magit_emit_fn emit, void *ctx);

/* Set / remove the git note on commit `rev`. 1 ok, 0 fail. (The note, if any,
 * is shown in the *magit-commit* view.) */
int mg_magit_note_set(const char *repo_path, const char *rev,
                      const char *message);
int mg_magit_note_remove(const char *repo_path, const char *rev);

/* Add a worktree `name` at `path` / remove worktree `name` (prunes admin files
 * and deletes its working-tree directory). 1 ok, 0 fail. */
int mg_magit_worktree_add(const char *repo_path, const char *name,
                          const char *path);
int mg_magit_worktree_remove(const char *repo_path, const char *name);

/* git bisect, the binary-search bug hunt. start: mark `bad`/`good` revspecs and
 * check out the first midpoint. mark: record the current (tested) commit as bad
 * (`is_bad`!=0) or good, then advance. Both fill `out` with the progress / "...
 * is the first bad commit" line and return 1, else 0. reset: leave bisect and
 * return to the starting branch. active: 1 while a bisect is in progress. */
int mg_magit_bisect_start(const char *repo_path, const char *bad,
                          const char *good, char *out, int outlen);
int mg_magit_bisect_mark(const char *repo_path, int is_bad, char *out,
                         int outlen);
int mg_magit_bisect_reset(const char *repo_path);
int mg_magit_bisect_active(const char *repo_path);

#ifdef __cplusplus
}
#endif

#endif /* MG_MAGIT_BRIDGE_H */
