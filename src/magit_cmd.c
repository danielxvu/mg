/*
 * magit_cmd.c -- the `magit-status` command and its interactive buffer
 * (tasks M3-3, M4-3).
 *
 * Glue between mg's C core (buffers/windows/keymaps) and the native engine's
 * extern "C" bridge. Compiled into mg only when ENABLE_NATIVE_MAGIT is set.
 * The status buffer uses a buffer-local keymap (magit-status-mode):
 *   s  stage the file/hunk/region    u  unstage the file/hunk/region
 *   g  refresh                       q  close the window
 * A region (set the mark with C-SPC, then move point) stages/unstages just the
 * marked lines of one hunk -- magit's signature line/region staging.
 *
 * This file is in the public domain.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "def.h"
#include "kbd.h"
#include "magit/bridge.h"

#ifdef ENABLE_NATIVE_MAGIT

#define MAGIT_MAX_LINES 1024

#define MAGIT_MAX_EXPANDED 64

static int	magit_refresh(int, int);
static int	magit_stage(int, int);
static int	magit_unstage(int, int);
static int	magit_discard(int, int);
static int	magit_commit(int, int);
static int	magit_commit_amend(int, int);
static int	magit_commit_extend(int, int);
static int	magit_commit_reword(int, int);
static int	magit_commit_finish(int, int);
static int	magit_commit_abort(int, int);
static int	magit_toggle_expand(int, int);
static int	magit_visit(int, int);
static int	magit_help(int, int);
static int	magit_next_section(int, int);
static int	magit_prev_section(int, int);
static int	magit_stash_apply(int, int);
static int	magit_stash_push(int, int);
static int	magit_stash_pop(int, int);
static int	magit_checkout(int, int);
static int	magit_branch_create(int, int);
static int	magit_branch_delete(int, int);
static int	magit_branch_rename(int, int);
static int	magit_tag_create_cmd(int, int);
static int	magit_tag_annotate_cmd(int, int);
static int	magit_tag_delete_cmd(int, int);
static int	magit_worktree_add_cmd(int, int);
static int	magit_worktree_delete_cmd(int, int);
static int	magit_stage_all(int, int);
static int	magit_unstage_all(int, int);
static int	magit_ignore(int, int);
static int	magit_region(int *, char **, int *, int *);
static int	magit_line_index(void);
static const char *magit_log_oid_at_point(void);
static int	magit_at_point(char **, int *);
static int	magit_log(int, int);
static int	magit_blame(int, int);
static int	magit_cherrypick(int, int);
static int	magit_log_visit(int, int);
static int	magit_log_refresh(int, int);
static int	magit_log_revert(int, int);
static int	magit_log_note(int, int);
static int	magit_merge(int, int);
static int	magit_revert(int, int);
static int	magit_reset_soft(int, int);
static int	magit_reset_mixed(int, int);
static int	magit_reset_hard(int, int);
static int	magit_fetch(int, int);
static int	magit_pull(int, int);
static int	magit_push(int, int);
static int	magit_push_force(int, int);
static int	magit_push_upstream(int, int);
static int	magit_pull_rebase(int, int);
static int	magit_rebase_report(int, const char *, int, int);
static int	magit_cred_prompt(const char *, int, char *, int);
static int	magit_rebase_upstream(int, int);
static int	magit_rebase_elsewhere(int, int);
static int	magit_rebase_continue(int, int);
static int	magit_rebase_skip(int, int);
static int	magit_rebase_abort(int, int);
static int	magit_rebase_interactive_cmd(int, int);
static int	magit_todo_pick(int, int);
static int	magit_todo_drop(int, int);
static int	magit_todo_squash(int, int);
static int	magit_todo_fixup(int, int);
static int	magit_todo_reword(int, int);
static int	magit_todo_up(int, int);
static int	magit_todo_down(int, int);
static int	magit_todo_execute(int, int);
static int	magit_todo_abort(int, int);

/*
 * line -> {kind, hunk, path} map for the most recent render of *magit-status*.
 * One such buffer, so a single static map suffices. Lines past the cap are
 * still shown but not interactive.
 */
static struct {
	int	kind;
	int	hunk;
	char	path[PATH_MAX];
} magit_meta[MAGIT_MAX_LINES];
static int	magit_meta_count;

/* Paths whose diffs are currently expanded inline. */
static char	magit_expanded[MAGIT_MAX_EXPANDED][PATH_MAX];
static int	magit_expanded_count;

/* For *magit-log*: the full commit oid per buffer line ("" for non-commits). */
#define MAGIT_OID_LEN 64
static char	magit_log_oid[MAGIT_MAX_LINES][MAGIT_OID_LEN];
static int	magit_log_count;

/* Interactive-rebase plan backing the *git-rebase-todo* buffer. Each entry is
 * one commit; the buffer is a rendered view of this array (line i = entry i). */
#define MAGIT_MAX_TODO 256
struct magit_todo_entry {
	int	action;			/* 0 pick 1 drop 2 squash 3 fixup 4 reword */
	char	oid[MAGIT_OID_LEN];
	char	text[160];		/* "shortoid summary" */
	char	message[1024];		/* reword: the new commit message */
};
static struct magit_todo_entry	magit_todo[MAGIT_MAX_TODO];
static int	magit_todo_count;
static char	magit_todo_onto[PATH_MAX];	/* the base to rebase onto */

/* Section titles (count suffix stripped) whose bodies are currently folded. */
#define MAGIT_MAX_FOLDED 32
static char	magit_folded[MAGIT_MAX_FOLDED][PATH_MAX];
static int	magit_folded_count;
/* While rebuilding: true to suppress the body lines of a folded section. */
static int	magit_skip;

static PF magit_tab[] = { magit_toggle_expand };
static PF magit_ret[] = { magit_visit };
static PF magit_esc[] = { NULL };		/* ESC -> meta prefix */
static PF magit_qmark[] = { magit_help };
static PF magit_A[] = { magit_cherrypick };
static PF magit_B[] = { magit_blame };
static PF magit_F[] = { NULL };			/* F -> pull menu prefix */
static PF magit_P[] = { NULL };			/* P -> push menu prefix */

/* Pull menu: F p pull (merge), F r pull --rebase. */
static PF pull_p[] = { magit_pull };
static PF pull_r[] = { magit_pull_rebase };

static struct KEYMAPE (2) magit_pullmenu = {
	2,
	2,
	rescan,
	{
		{ 'p', 'p', pull_p, NULL },	/* F p: pull (merge) */
		{ 'r', 'r', pull_r, NULL }	/* F r: pull --rebase */
	}
};

/* Push menu: P p push, P f force-push, P u push + set-upstream. */
static PF push_f[] = { magit_push_force };
static PF push_p[] = { magit_push };
static PF push_u[] = { magit_push_upstream };

static struct KEYMAPE (3) magit_pushmenu = {
	3,
	3,
	rescan,
	{
		{ 'f', 'f', push_f, NULL },	/* P f: force-push */
		{ 'p', 'p', push_p, NULL },	/* P p: push */
		{ 'u', 'u', push_u, NULL }	/* P u: push + set upstream */
	}
};
static PF magit_S[] = { magit_stage_all };
static PF magit_U[] = { magit_unstage_all };
static PF magit_V[] = { magit_revert };
static PF magit_f[] = { magit_fetch };
static PF magit_W[] = { NULL };			/* W -> worktree menu prefix */
static PF magit_X[] = { NULL };			/* X -> reset menu prefix */

/* Worktree menu: W a add, W k delete. Entries ascending. */
static PF worktree_a[] = { magit_worktree_add_cmd };
static PF worktree_k[] = { magit_worktree_delete_cmd };

static struct KEYMAPE (2) magit_worktreemenu = {
	2,
	2,
	rescan,
	{
		{ 'a', 'a', worktree_a, NULL },	/* W a: add */
		{ 'k', 'k', worktree_k, NULL }	/* W k: delete */
	}
};
static PF magit_m[] = { magit_merge };

/* Reset menu: X h hard / X m mixed / X s soft (each prompts for a revision). */
static PF reset_h[] = { magit_reset_hard };
static PF reset_m[] = { magit_reset_mixed };
static PF reset_s[] = { magit_reset_soft };

static struct KEYMAPE (3) magit_resetmenu = {
	3,
	3,
	rescan,
	{
		{ 'h', 'h', reset_h, NULL },	/* X h: hard */
		{ 'm', 'm', reset_m, NULL },	/* X m: mixed */
		{ 's', 's', reset_s, NULL }	/* X s: soft */
	}
};
static PF magit_a[] = { magit_stash_apply };
static PF magit_b[] = { NULL };			/* b -> branch menu prefix */
static PF magit_c[] = { NULL };			/* c -> commit menu prefix */
static PF magit_g[] = { magit_refresh };
static PF magit_i[] = { magit_ignore };
static PF magit_k[] = { magit_discard };
static PF magit_q[] = { delwind };
static PF magit_l[] = { magit_log };
static PF magit_r[] = { NULL };			/* r -> rebase menu prefix */
static PF magit_s[] = { magit_stage };
static PF magit_t[] = { NULL };			/* t -> tag menu prefix */
static PF magit_u[] = { magit_unstage };
static PF magit_z[] = { NULL };			/* z -> stash menu prefix */

/*
 * Tag menu: `t` prefixes into this. t=create (at HEAD), k=delete (the tag at
 * point, else prompt). Entries ascending.
 */
static PF tag_a[] = { magit_tag_annotate_cmd };
static PF tag_k[] = { magit_tag_delete_cmd };
static PF tag_t[] = { magit_tag_create_cmd };

static struct KEYMAPE (3) magit_tagmenu = {
	3,
	3,
	rescan,
	{
		{ 'a', 'a', tag_a, NULL },	/* t a: annotated */
		{ 'k', 'k', tag_k, NULL },	/* t k: delete */
		{ 't', 't', tag_t, NULL }	/* t t: create (lightweight) */
	}
};

/*
 * *magit-log* keymap: RET shows a commit's diff, V reverts the commit at point,
 * g refreshes, q closes.
 */
static PF maglog_ret[] = { magit_log_visit };
static PF maglog_A[] = { magit_cherrypick };
static PF maglog_T[] = { magit_log_note };
static PF maglog_V[] = { magit_log_revert };
static PF maglog_g[] = { magit_log_refresh };
static PF maglog_q[] = { delwind };

static struct KEYMAPE (6) maglogmap = {
	6,
	6,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), maglog_ret, NULL },	/* RET: show commit */
		{ 'A', 'A', maglog_A, NULL },			/* A: cherry-pick at point */
		{ 'T', 'T', maglog_T, NULL },			/* T: set/remove note */
		{ 'V', 'V', maglog_V, NULL },			/* V: revert commit */
		{ 'g', 'g', maglog_g, NULL },			/* g: refresh */
		{ 'q', 'q', maglog_q, NULL }			/* q: close */
	}
};

/* *magit-commit* view keymap: q closes (the diff is read-only). */
static PF magcommit_q[] = { delwind };

static struct KEYMAPE (1) magcommitmap = {
	1,
	1,
	rescan,
	{
		{ 'q', 'q', magcommit_q, NULL }			/* q: close */
	}
};

/*
 * ESC submap: M-n / M-p jump between section headers. map_default is rescan, so
 * any other meta key (e.g. M-x) falls through to the global keymap unchanged.
 */
static PF magit_meta_n[] = { magit_next_section };
static PF magit_meta_p[] = { magit_prev_section };

static struct KEYMAPE (2) magit_metamap = {
	2,
	2,
	rescan,
	{
		{ 'n', 'n', magit_meta_n, NULL },	/* M-n: next section */
		{ 'p', 'p', magit_meta_p, NULL }	/* M-p: previous section */
	}
};

/*
 * Commit menu: `c` in the status buffer prefixes into this (magit's commit
 * transient). c=commit, a=amend, e=extend, w=reword. Entries ascending.
 */
static PF commit_c[] = { magit_commit };
static PF commit_a[] = { magit_commit_amend };
static PF commit_e[] = { magit_commit_extend };
static PF commit_w[] = { magit_commit_reword };

/*
 * Branch menu: `b` prefixes into this (magit's branch transient). b=checkout
 * (the old bare-b action), c=create, k=delete, m=rename. Entries ascending.
 */
static PF branch_b[] = { magit_checkout };
static PF branch_c[] = { magit_branch_create };
static PF branch_k[] = { magit_branch_delete };
static PF branch_m[] = { magit_branch_rename };

static struct KEYMAPE (4) magit_branchmenu = {
	4,
	4,
	rescan,
	{
		{ 'b', 'b', branch_b, NULL },	/* b b: checkout branch at point */
		{ 'c', 'c', branch_c, NULL },	/* b c: create */
		{ 'k', 'k', branch_k, NULL },	/* b k: delete */
		{ 'm', 'm', branch_m, NULL }	/* b m: rename */
	}
};

/*
 * Rebase menu: `r` prefixes into this (magit's rebase transient, subset).
 * a=abort, e=elsewhere (onto a prompted branch), r=continue, s=skip, u=onto
 * upstream. Entries ascending.
 */
static PF rebase_a[] = { magit_rebase_abort };
static PF rebase_e[] = { magit_rebase_elsewhere };
static PF rebase_i[] = { magit_rebase_interactive_cmd };
static PF rebase_r[] = { magit_rebase_continue };
static PF rebase_s[] = { magit_rebase_skip };
static PF rebase_u[] = { magit_rebase_upstream };

static struct KEYMAPE (6) magit_rebasemenu = {
	6,
	6,
	rescan,
	{
		{ 'a', 'a', rebase_a, NULL },	/* r a: abort */
		{ 'e', 'e', rebase_e, NULL },	/* r e: onto a branch */
		{ 'i', 'i', rebase_i, NULL },	/* r i: interactive */
		{ 'r', 'r', rebase_r, NULL },	/* r r: continue */
		{ 's', 's', rebase_s, NULL },	/* r s: skip */
		{ 'u', 'u', rebase_u, NULL }	/* r u: onto upstream */
	}
};

/*
 * *git-rebase-todo* (magit-rebase-todo-mode) keymap: p/d/k/s/f set the action
 * on the line at point; ESC n / ESC p reorder; C-c C-c runs the plan, C-c C-k
 * aborts. Other keys fall through to fundamental (C-n/C-p navigate).
 */
static PF todo_p[] = { magit_todo_pick };
static PF todo_d[] = { magit_todo_drop };
static PF todo_s[] = { magit_todo_squash };
static PF todo_f[] = { magit_todo_fixup };
static PF todo_w[] = { magit_todo_reword };
static PF todo_esc[] = { NULL };		/* ESC -> reorder submap */
static PF todo_cc_pf[] = { magit_todo_execute };
static PF todo_ck_pf[] = { magit_todo_abort };
static PF todo_meta_n[] = { magit_todo_down };
static PF todo_meta_p[] = { magit_todo_up };

static struct KEYMAPE (2) todo_metamap = {
	2,
	2,
	rescan,
	{
		{ 'n', 'n', todo_meta_n, NULL }, /* ESC n: move down */
		{ 'p', 'p', todo_meta_p, NULL }  /* ESC p: move up */
	}
};

static struct KEYMAPE (2) todo_ccmap = {
	2,
	2,
	rescan,
	{
		{ CCHR('C'), CCHR('C'), todo_cc_pf, NULL },	/* C-c C-c: run */
		{ CCHR('K'), CCHR('K'), todo_ck_pf, NULL }	/* C-c C-k: abort */
	}
};

static struct KEYMAPE (8) magit_todomap = {
	8,
	8,
	rescan,
	{
		{ CCHR('C'), CCHR('C'), todo_esc,		/* C-c prefix */
		    (KEYMAP *)&todo_ccmap },
		{ CCHR('['), CCHR('['), todo_esc,		/* ESC prefix */
		    (KEYMAP *)&todo_metamap },
		{ 'd', 'd', todo_d, NULL },			/* d: drop */
		{ 'f', 'f', todo_f, NULL },			/* f: fixup */
		{ 'k', 'k', todo_d, NULL },			/* k: drop */
		{ 'p', 'p', todo_p, NULL },			/* p: pick */
		{ 's', 's', todo_s, NULL },			/* s: squash */
		{ 'w', 'w', todo_w, NULL }			/* w: reword */
	}
};

/*
 * Stash menu: `z` prefixes into this (magit's stash transient). z=push/create,
 * p=pop. Entries ascending.
 */
static PF stash_p[] = { magit_stash_pop };
static PF stash_z[] = { magit_stash_push };

static struct KEYMAPE (2) magit_stashmenu = {
	2,
	2,
	rescan,
	{
		{ 'p', 'p', stash_p, NULL },	/* z p: pop */
		{ 'z', 'z', stash_z, NULL }	/* z z: push/create */
	}
};

static struct KEYMAPE (4) magit_commitmenu = {
	4,
	4,
	rescan,
	{
		{ 'a', 'a', commit_a, NULL },	/* c a: amend */
		{ 'c', 'c', commit_c, NULL },	/* c c: commit */
		{ 'e', 'e', commit_e, NULL },	/* c e: extend */
		{ 'w', 'w', commit_w, NULL }	/* c w: reword */
	}
};

/* Entries MUST stay in ascending key order -- doscan() relies on it. */
static struct KEYMAPE (28) magitmap = {
	28,
	28,
	rescan,
	{
		{ CCHR('I'), CCHR('I'), magit_tab, NULL },	/* TAB: expand/collapse */
		{ CCHR('M'), CCHR('M'), magit_ret, NULL },	/* RET: visit file */
		{ CCHR('['), CCHR('['), magit_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ '?', '?', magit_qmark, NULL },		/* ?: key help */
		{ 'A', 'A', magit_A, NULL },			/* A: cherry-pick */
		{ 'B', 'B', magit_B, NULL },			/* B: blame file at point */
		{ 'F', 'F', magit_F, (KEYMAP *)&magit_pullmenu }, /* F: pull menu */
		{ 'P', 'P', magit_P, (KEYMAP *)&magit_pushmenu }, /* P: push menu */
		{ 'S', 'S', magit_S, NULL },			/* S: stage all */
		{ 'U', 'U', magit_U, NULL },			/* U: unstage all */
		{ 'V', 'V', magit_V, NULL },			/* V: revert */
		{ 'W', 'W', magit_W, (KEYMAP *)&magit_worktreemenu }, /* W: worktree menu */
		{ 'X', 'X', magit_X, (KEYMAP *)&magit_resetmenu }, /* X: reset menu */
		{ 'a', 'a', magit_a, NULL },			/* a: apply stash */
		{ 'b', 'b', magit_b, (KEYMAP *)&magit_branchmenu }, /* b: branch menu */
		{ 'c', 'c', magit_c, (KEYMAP *)&magit_commitmenu }, /* c: commit menu */
		{ 'f', 'f', magit_f, NULL },			/* f: fetch */
		{ 'g', 'g', magit_g, NULL },
		{ 'i', 'i', magit_i, NULL },			/* i: gitignore */
		{ 'k', 'k', magit_k, NULL },
		{ 'l', 'l', magit_l, NULL },			/* l: log buffer */
		{ 'm', 'm', magit_m, NULL },			/* m: merge */
		{ 'q', 'q', magit_q, NULL },
		{ 'r', 'r', magit_r, (KEYMAP *)&magit_rebasemenu }, /* r: rebase menu */
		{ 's', 's', magit_s, NULL },
		{ 't', 't', magit_t, (KEYMAP *)&magit_tagmenu }, /* t: tag menu */
		{ 'u', 'u', magit_u, NULL },
		{ 'z', 'z', magit_z, (KEYMAP *)&magit_stashmenu } /* z: stash menu */
	}
};

/*
 * Commit-message buffer mode: C-c is a prefix (NULL PF + submap), and the
 * submap binds C-c C-c (finish) and C-c C-k (abort). All other keys fall
 * through to the fundamental map, so the buffer is normally editable.
 */
static PF commit_cc_pf[] = { magit_commit_finish };
static PF commit_ck_pf[] = { magit_commit_abort };

static struct KEYMAPE (2) commit_cmap = {
	2,
	2,
	rescan,
	{
		{ CCHR('C'), CCHR('C'), commit_cc_pf, NULL },
		{ CCHR('K'), CCHR('K'), commit_ck_pf, NULL }
	}
};

static PF commit_prefix[] = { NULL };	/* C-c -> prefix into commit_cmap */

static struct KEYMAPE (1) commitmap = {
	1,
	1,
	rescan,
	{
		{ CCHR('C'), CCHR('C'), commit_prefix, (KEYMAP *)&commit_cmap }
	}
};

/*
 * doscan() finds a binding by scanning map_element[] in order and short-
 * circuiting once the typed key passes an element's range -- so the elements
 * MUST be sorted ascending and non-overlapping, or a binding silently becomes
 * unreachable. This is a compile-time-fixed table, so any violation is a
 * programming error: verify it once and panic (fail fast) rather than ship a
 * dead key. Data-driven, so it covers whatever entries the table grows.
 */
static void
magit_assert_keymap_sorted(void)
{
	int	i;

	for (i = 0; i < magitmap.map_num; i++) {
		struct map_element *e = &magitmap.map_element[i];

		if (e->k_base > e->k_num)
			panic("magit keymap: element has k_base > k_num");
		if (i > 0 && magitmap.map_element[i - 1].k_num >= e->k_base)
			panic("magit keymap: elements out of ascending order");
	}
}

/*
 * A section header's stable key: its text with the trailing " (N)" count
 * stripped, so folds survive across refreshes whose counts change. Copies into
 * `out` (size `n`).
 */
static void
magit_section_key(const char *line, char *out, size_t n)
{
	const char	*paren = strstr(line, " (");

	if (paren != NULL) {
		size_t len = (size_t)(paren - line);
		if (len >= n)
			len = n - 1;
		memcpy(out, line, len);
		out[len] = '\0';
	} else {
		(void)strlcpy(out, line, n);
	}
}

/* True if the section header `line`'s key is in the folded set. */
static int
magit_section_folded(const char *line)
{
	char	key[PATH_MAX];
	int	i;

	magit_section_key(line, key, sizeof(key));
	for (i = 0; i < magit_folded_count; i++)
		if (strcmp(magit_folded[i], key) == 0)
			return (TRUE);
	return (FALSE);
}

/* emit callback: record the line's kind/path, then append it to the buffer. */
static void
magit_emit(void *ctx, const char *line, int kind, const char *path, int hunk)
{
	/*
	 * A folded section keeps its header but drops its body: on a header,
	 * (re)arm skipping for that section; otherwise honor the current skip.
	 */
	if (kind == MG_LINE_SECTION)
		magit_skip = magit_section_folded(line);
	else if (magit_skip)
		return;

	if (magit_meta_count < MAGIT_MAX_LINES) {
		magit_meta[magit_meta_count].kind = kind;
		magit_meta[magit_meta_count].hunk = hunk;
		if (path != NULL)
			(void)strlcpy(magit_meta[magit_meta_count].path, path,
			    sizeof(magit_meta[magit_meta_count].path));
		else
			magit_meta[magit_meta_count].path[0] = '\0';
		magit_meta_count++;
	}
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

/* (Re)build the buffer contents and the line->meta map. */
static int
magit_build(struct buffer *bp)
{
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);

	bp->b_flag |= BFIGNDIRTY;	/* don't prompt when re-clearing */
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	magit_meta_count = 0;
	magit_skip = 0;
	{
		const char	*exp[MAGIT_MAX_EXPANDED];
		int		 i;

		for (i = 0; i < magit_expanded_count; i++)
			exp[i] = magit_expanded[i];
		(void)mg_magit_status_buffer(cwd, exp, magit_expanded_count,
		    magit_emit, bp);
	}

	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;

	/* bclear freed the old lines; re-point any window showing this buffer. */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == bp) {
			wp->w_dotp = bp->b_dotp;
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_rflag |= WFFULL;
		}
	}
	return (TRUE);
}

/*
 * magit-status: show the working-tree status in a read-only *magit-status*
 * buffer -- branch/HEAD, untracked/unstaged/staged sections, recent commits.
 */
int
magit_status(int f, int n)
{
	static int	 initialized = 0;
	struct buffer	*bp;
	struct mgwin	*wp;

	if (!initialized) {
		magit_assert_keymap_sorted();
		maps_add((KEYMAP *)&magitmap, "magit-status-mode");
		/* Register the log/commit-view modes here too: RET on a stash opens
		 * the commit-view buffer without ever going through `l`. */
		maps_add((KEYMAP *)&maglogmap, "magit-log-mode");
		maps_add((KEYMAP *)&magcommitmap, "magit-commit-view-mode");
		maps_add((KEYMAP *)&magit_todomap, "magit-rebase-todo-mode");
		mg_magit_set_cred_prompt(magit_cred_prompt); /* HTTPS user/pass auth */
		initialized = 1;
	}

	if ((bp = bfind("*magit-status*", TRUE)) == NULL)
		return (FALSE);
	if (magit_build(bp) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;		/* make the status window active for s/u/g/q */
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-status-mode");
	bp->b_nmodes = 1;

	return (TRUE);
}

static int
magit_refresh(int f, int n)
{
	struct buffer	*bp;

	if ((bp = bfind("*magit-status*", TRUE)) == NULL)
		return (FALSE);
	return (magit_build(bp));
}

/* emit callback for *magit-log*: record the oid of each commit line. */
static void
magit_log_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
	if (magit_log_count < MAGIT_MAX_LINES) {
		if (kind == MG_LINE_COMMIT && path != NULL)
			(void)strlcpy(magit_log_oid[magit_log_count], path,
			    MAGIT_OID_LEN);
		else
			magit_log_oid[magit_log_count][0] = '\0';
		magit_log_count++;
	}
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

/* emit callback for read-only views (e.g. *magit-commit*): text only. */
static void
magit_plain_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

/* (Re)build the *magit-log* buffer + its per-line oid map. */
static int
magit_log_build(struct buffer *bp)
{
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	magit_log_count = 0;
	(void)mg_magit_log_buffer(cwd, 100, magit_log_emit, bp);

	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp) {
			wp->w_dotp = bp->b_dotp;
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_rflag |= WFFULL;
		}
	return (TRUE);
}

/* l: open the *magit-log* buffer (commit history, newest first). */
static int
magit_log(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	/* magit-log-mode / magit-commit-view-mode are registered by magit_status,
	 * which always runs first (l is only bound in the status buffer). */
	if ((bp = bfind("*magit-log*", TRUE)) == NULL)
		return (FALSE);
	if (magit_log_build(bp) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-log-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}

static int
magit_log_refresh(int f, int n)
{
	struct buffer	*bp;

	if ((bp = bfind("*magit-log*", TRUE)) == NULL)
		return (FALSE);
	return (magit_log_build(bp));
}

/* B: blame the file at point in a read-only *magit-blame* buffer. */
static int
magit_blame(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char		*path = NULL, cwd[PATH_MAX];
	int		 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if ((kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED &&
	    kind != MG_LINE_STAGED) || path == NULL || path[0] == '\0') {
		ewprintf("Not on a file");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if ((bp = bfind("*magit-blame*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	if (mg_magit_blame_file(cwd, path, magit_plain_emit, bp) == 0) {
		ewprintf("Blame failed (untracked or binary?)");
		return (FALSE);
	}
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-commit-view-mode"); /* q closes */
	bp->b_nmodes = 1;
	return (TRUE);
}

/* The full oid of the commit at point in *magit-log*, or NULL. */
static const char *
magit_log_oid_at_point(void)
{
	struct line	*lp;
	int		 idx = 0;

	for (lp = bfirstlp(curbp);
	    lp != curwp->w_dotp && lp != curbp->b_headp; lp = lforw(lp))
		idx++;
	if (idx >= magit_log_count || magit_log_oid[idx][0] == '\0')
		return (NULL);
	return (magit_log_oid[idx]);
}

/* Pop a read-only *magit-commit* buffer showing `rev`'s diff (a commit oid or a
 * stash rev like "stash@{0}"). */
static int
magit_show_rev(const char *rev)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if ((bp = bfind("*magit-commit*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	if (mg_magit_commit_diff(cwd, rev, magit_plain_emit, bp) == 0) {
		ewprintf("No diff for %s", rev);
		return (FALSE);
	}
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-commit-view-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}

/* RET in *magit-log*: pop a read-only *magit-commit* buffer with the diff. */
static int
magit_log_visit(int f, int n)
{
	const char	*oid;

	if ((oid = magit_log_oid_at_point()) == NULL) {
		ewprintf("Not on a commit");
		return (FALSE);
	}
	return (magit_show_rev(oid));
}

/*
 * A: cherry-pick a commit onto HEAD. In *magit-log* it picks the commit at
 * point; elsewhere it prompts for a revision.
 */
static int
magit_cherrypick(int f, int n)
{
	struct buffer	*logbp;
	const char	*oid = NULL;
	char		 rev[PATH_MAX], cwd[PATH_MAX];

	logbp = bfind("*magit-log*", FALSE);
	if (logbp != NULL && curbp == logbp)
		oid = magit_log_oid_at_point();
	if (oid != NULL)
		(void)strlcpy(rev, oid, sizeof(rev));
	else if (eread("Cherry-pick: ", rev, sizeof(rev), EFNEW | EFCR) == NULL ||
	    rev[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_cherrypick(cwd, rev) != 1) {
		ewprintf("Cherry-pick failed (conflict or bad revision)");
		return (FALSE);
	}
	ewprintf("Cherry-picked %.8s", rev);
	return (magit_refresh(f, n));
}

/* T in *magit-log*: set the note on the commit at point (empty input removes). */
static int
magit_log_note(int f, int n)
{
	const char	*oid;
	char		 msg[1024], cwd[PATH_MAX];
	int		 ok;

	if ((oid = magit_log_oid_at_point()) == NULL) {
		ewprintf("Not on a commit");
		return (FALSE);
	}
	if (eread("Note (empty to remove): ", msg, sizeof(msg),
	    EFNEW | EFCR | EFNUL) == NULL)
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	ok = (msg[0] == '\0') ? mg_magit_note_remove(cwd, oid)
	                      : mg_magit_note_set(cwd, oid, msg);
	if (ok != 1) {
		ewprintf("Note update failed");
		return (FALSE);
	}
	ewprintf(msg[0] == '\0' ? "Note removed" : "Note set");
	return (TRUE);
}

/* V in *magit-log*: revert the commit at point (records the inverse on HEAD). */
static int
magit_log_revert(int f, int n)
{
	const char	*oid;
	char		 cwd[PATH_MAX], prompt[48];

	if ((oid = magit_log_oid_at_point()) == NULL) {
		ewprintf("Not on a commit");
		return (FALSE);
	}
	(void)snprintf(prompt, sizeof(prompt), "Revert commit %.8s", oid);
	if (eyesno(prompt) != TRUE)
		return (FALSE);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_revert(cwd, oid) != 1) {
		ewprintf("Revert failed (conflicts?)");
		return (FALSE);
	}
	return (magit_log_refresh(f, n));
}

/* Resolve the kind/path/hunk of the row under the cursor. */
static int
magit_at_point(char **path_out, int *hunk_out)
{
	struct line	*lp;
	int		 idx = 0;

	for (lp = bfirstlp(curbp);
	    lp != curwp->w_dotp && lp != curbp->b_headp; lp = lforw(lp))
		idx++;
	if (idx >= magit_meta_count) {
		*hunk_out = -1;
		return (MG_LINE_OTHER);
	}
	*path_out = magit_meta[idx].path;
	*hunk_out = magit_meta[idx].hunk;
	return (magit_meta[idx].kind);
}

/*
 * If a mark is set, resolve the marked range into a line region within a single
 * hunk: the hunk index, its path, and the inclusive line indices [first, last]
 * within that hunk (0-based, matching the engine's file_diff numbering). The
 * region is valid only when every line between mark and point is a diff line of
 * the same hunk and file. Returns TRUE on a valid region, FALSE otherwise (no
 * mark, or the span crosses non-diff lines / hunk / file boundaries) -- in which
 * case the caller falls back to whole-hunk/whole-file staging.
 */
static int
magit_region(int *hunk_out, char **path_out, int *first_out, int *last_out)
{
	struct line	*lp;
	int		 mark_idx = 0, point_idx, lo, hi, hdr, i;

	if (curwp->w_markp == NULL)
		return (FALSE);

	for (lp = bfirstlp(curbp);
	    lp != curwp->w_markp && lp != curbp->b_headp; lp = lforw(lp))
		mark_idx++;
	if (lp != curwp->w_markp)
		return (FALSE);
	if ((point_idx = magit_line_index()) < 0)
		return (FALSE);

	lo = (mark_idx < point_idx) ? mark_idx : point_idx;
	hi = (mark_idx < point_idx) ? point_idx : mark_idx;
	if (hi >= magit_meta_count)
		return (FALSE);

	/* Every selected line must be a diff line of the same hunk and file. */
	for (i = lo; i <= hi; i++) {
		if (magit_meta[i].kind != MG_LINE_DIFF ||
		    magit_meta[i].hunk != magit_meta[lo].hunk ||
		    strcmp(magit_meta[i].path, magit_meta[lo].path) != 0)
			return (FALSE);
	}

	/* The hunk header precedes the first diff line; li = idx - header - 1. */
	for (hdr = lo; hdr >= 0 && magit_meta[hdr].kind != MG_LINE_HUNK; hdr--)
		;
	if (hdr < 0)
		return (FALSE);

	*hunk_out = magit_meta[lo].hunk;
	*path_out = magit_meta[lo].path;
	*first_out = lo - hdr - 1;
	*last_out = hi - hdr - 1;
	return (TRUE);
}

/* Toggle the fold of the section whose header `line` is (key-based). */
static int
magit_toggle_fold(const char *line)
{
	char	key[PATH_MAX];
	int	i;

	magit_section_key(line, key, sizeof(key));
	for (i = 0; i < magit_folded_count; i++) {
		if (strcmp(magit_folded[i], key) == 0) {	/* unfold */
			(void)strlcpy(magit_folded[i],
			    magit_folded[magit_folded_count - 1], PATH_MAX);
			magit_folded_count--;
			return (TRUE);
		}
	}
	if (magit_folded_count < MAGIT_MAX_FOLDED)		/* fold */
		(void)strlcpy(magit_folded[magit_folded_count++], key, PATH_MAX);
	return (TRUE);
}

/* TAB: fold/unfold a section header, or expand/collapse a file's inline diff. */
static int
magit_toggle_expand(int f, int n)
{
	char	*path = NULL;
	int	 hunk, kind, i, len;
	char	 hdr[PATH_MAX];

	kind = magit_at_point(&path, &hunk);
	/* On a section header, TAB folds/unfolds that whole section. */
	if (kind == MG_LINE_SECTION) {
		len = llength(curwp->w_dotp);
		if (len >= (int)sizeof(hdr))
			len = (int)sizeof(hdr) - 1;
		memcpy(hdr, ltext(curwp->w_dotp), len);
		hdr[len] = '\0';
		(void)magit_toggle_fold(hdr);
		return (magit_refresh(f, n));
	}
	if (kind != MG_LINE_UNSTAGED && kind != MG_LINE_STAGED &&
	    kind != MG_LINE_HUNK && kind != MG_LINE_DIFF) {
		ewprintf("Nothing to expand here");
		return (FALSE);
	}
	for (i = 0; i < magit_expanded_count; i++) {
		if (strcmp(magit_expanded[i], path) == 0) {	/* collapse */
			(void)strlcpy(magit_expanded[i],
			    magit_expanded[magit_expanded_count - 1], PATH_MAX);
			magit_expanded_count--;
			return (magit_refresh(f, n));
		}
	}
	if (magit_expanded_count < MAGIT_MAX_EXPANDED)		/* expand */
		(void)strlcpy(magit_expanded[magit_expanded_count++], path,
		    PATH_MAX);
	return (magit_refresh(f, n));
}

/*
 * RET: visit the file at point in another window (the status buffer stays
 * visible). Works on file lines and on a hunk/diff line (whose meta records the
 * owning file). Non-prompting twin of poptofile().
 */
static int
magit_visit(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char		 cwd[PATH_MAX], full[PATH_MAX], *adjf, *path = NULL;
	int		 kind, hunk, status;

	kind = magit_at_point(&path, &hunk);
	/* On a stash line, RET shows the stash's diff (like a commit). */
	if (kind == MG_LINE_STASH) {
		char	rev[32];

		(void)snprintf(rev, sizeof(rev), "stash@{%d}", hunk);
		return (magit_show_rev(rev));
	}
	if (path == NULL || path[0] == '\0' ||
	    (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED &&
	    kind != MG_LINE_STAGED && kind != MG_LINE_HUNK &&
	    kind != MG_LINE_DIFF)) {
		ewprintf("Nothing to visit on this line");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (snprintf(full, sizeof(full), "%s/%s", cwd, path) >=
	    (int)sizeof(full))
		return (FALSE);
	if ((adjf = adjustname(full, TRUE)) == NULL)
		return (FALSE);
	if ((bp = findbuffer(adjf)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	curwp = wp;
	if (bp->b_fname[0] == '\0') {
		if ((status = readin(adjf)) != TRUE) {
			(void)killbuffer(bp);
			return (status);
		}
	}
	return (TRUE);
}

/* ?: pop a read-only *magit-help* buffer listing the key bindings. */
static int
magit_help(int f, int n)
{
	static const char *const keys[] = {
		"magit-status key bindings",
		"",
		"  TAB      fold a section, or expand/collapse a file diff",
		"  RET      visit the file (or show the stash) at point",
		"  M-n/M-p  next / previous section",
		"  s        stage the file/hunk at point (or marked region)",
		"  u        unstage the file/hunk at point (or marked region)",
		"  S / U    stage all / unstage all",
		"  i        add the file at point to .gitignore",
		"  k        discard changes / region / drop the stash at point",
		"  a        apply the stash at point",
		"  A        cherry-pick (commit at point in the log, else prompt)",
		"  B        blame the file at point",
		"  b b/c/k/m  branch: checkout / create / delete / rename",
		"  t t/a/k  tag: lightweight / annotated / delete",
		"  W a/k    worktree: add / delete",
		"  c c/a/e/w  commit / amend / extend / reword",
		"  z z/p    stash: push / pop",
		"  l        log buffer (RET diff, A cherry-pick, V revert, T note)",
		"  m        merge a branch into HEAD",
		"  r e/u    rebase onto a branch / upstream",
		"  r r/s/a  rebase continue / skip / abort",
		"  r i      interactive rebase (todo buffer)",
		"  V        revert a commit",
		"  X h/m/s  reset HEAD: hard / mixed / soft",
		"  f        fetch (origin)",
		"  F p/r    pull: merge / rebase",
		"  P p/f/u  push / force-push / push set-upstream",
		"  g        refresh",
		"  q        quit this window",
		"  ?        this help",
	};
	struct buffer	*bp;
	struct mgwin	*wp;
	size_t		 i;

	if ((bp = bfind("*magit-help*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		(void)addlinef(bp, "%s", keys[i]);
	bp->b_flag |= BFREADONLY;
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curbp = bp;
	curwp = wp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = 0;
	return (TRUE);
}

/* Index of the line at point in the current window (0-based), or -1. */
static int
magit_line_index(void)
{
	struct line	*lp;
	int		 idx = 0;

	for (lp = bfirstlp(curbp);
	    lp != curwp->w_dotp && lp != curbp->b_headp; lp = lforw(lp))
		idx++;
	return (lp == curwp->w_dotp ? idx : -1);
}

/* Move point to line index `idx` (0-based) in the current window. */
static void
magit_goto_index(int idx)
{
	struct line	*lp = bfirstlp(curbp);
	int		 i;

	for (i = 0; i < idx && lforw(lp) != curbp->b_headp; i++)
		lp = lforw(lp);
	curwp->w_dotp = lp;
	curwp->w_doto = 0;
	curwp->w_rflag |= WFMOVE;
}

/* M-n / M-p: move point to the next / previous section header (MG_LINE_SECTION). */
static int
magit_section_move(int dir)
{
	int	cur, i;

	if ((cur = magit_line_index()) < 0)
		return (FALSE);
	for (i = cur + dir; i >= 0 && i < magit_meta_count; i += dir) {
		if (magit_meta[i].kind == MG_LINE_SECTION) {
			magit_goto_index(i);
			return (TRUE);
		}
	}
	ewprintf("No more sections");
	return (FALSE);
}

static int
magit_next_section(int f, int n)
{
	return (magit_section_move(1));
}

static int
magit_prev_section(int f, int n)
{
	return (magit_section_move(-1));
}

/* a: reapply the stash at point (keeps it in the stash list). */
static int
magit_stash_apply(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_STASH) {
		ewprintf("Not on a stash");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_stash_apply(cwd, hunk) != 1) {	/* hunk holds the index */
		ewprintf("Stash apply failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* z z: stash the working-tree changes away (prompts for an optional message). */
static int
magit_stash_push(int f, int n)
{
	char	msg[PATH_MAX], cwd[PATH_MAX];

	if (eread("Stash message: ", msg, sizeof(msg),
	    EFNEW | EFCR | EFNUL) == NULL)
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_stash_push(cwd, msg) != 1) {
		ewprintf("Stash failed (nothing to stash?)");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* z p: pop a stash (the one at point, else stash@{0}) -- apply then drop. */
static int
magit_stash_pop(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk, index;

	kind = magit_at_point(&path, &hunk);
	index = (kind == MG_LINE_STASH) ? hunk : 0;	/* hunk holds the index */
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_stash_pop(cwd, index) != 1) {
		ewprintf("Stash pop failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* b: check out the branch at point. */
static int
magit_checkout(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_BRANCH) {
		ewprintf("Not on a branch");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_checkout(cwd, path) != 1) {	/* path holds the name */
		ewprintf("Checkout failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* b c: create a branch at HEAD (prompts for the name; does not switch to it). */
static int
magit_branch_create(int f, int n)
{
	char	name[PATH_MAX], cwd[PATH_MAX];

	if (eread("Create branch: ", name, sizeof(name), EFNEW | EFCR) == NULL ||
	    name[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_branch_create(cwd, name) != 1) {
		ewprintf("Branch create failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* b k: delete a branch (the branch at point, else prompt; confirms first). */
static int
magit_branch_delete(int f, int n)
{
	char	*path = NULL;
	char	 name[PATH_MAX], prompt[PATH_MAX + 32], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind == MG_LINE_BRANCH)
		(void)strlcpy(name, path, sizeof(name));
	else if (eread("Delete branch: ", name, sizeof(name), EFNEW | EFCR) ==
	    NULL || name[0] == '\0')
		return (ABORT);
	(void)snprintf(prompt, sizeof(prompt), "Delete branch %s", name);
	if (eyesno(prompt) != TRUE)
		return (FALSE);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_branch_delete(cwd, name) != 1) {
		ewprintf("Branch delete failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* b m: rename a branch (the branch at point, else prompt) to a new name. */
static int
magit_branch_rename(int f, int n)
{
	char	*path = NULL;
	char	 from[PATH_MAX], to[PATH_MAX], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind == MG_LINE_BRANCH)
		(void)strlcpy(from, path, sizeof(from));
	else if (eread("Rename branch: ", from, sizeof(from), EFNEW | EFCR) ==
	    NULL || from[0] == '\0')
		return (ABORT);
	if (eread("Rename %s to: ", to, sizeof(to), EFNEW | EFCR, from) == NULL ||
	    to[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_branch_rename(cwd, from, to) != 1) {
		ewprintf("Branch rename failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* Shared tag-create: lightweight (msg NULL) or annotated (prompt a message). */
static int
magit_do_tag_create(int annotated, int f, int n)
{
	char	name[PATH_MAX], msg[PATH_MAX], cwd[PATH_MAX];

	if (eread("Create tag: ", name, sizeof(name), EFNEW | EFCR) == NULL ||
	    name[0] == '\0')
		return (ABORT);
	if (annotated &&
	    (eread("Tag message: ", msg, sizeof(msg), EFNEW | EFCR) == NULL ||
	    msg[0] == '\0'))
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_tag_create(cwd, name, "HEAD", annotated ? msg : NULL) != 1) {
		ewprintf("Tag create failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* t t: create a lightweight tag at HEAD. */
static int
magit_tag_create_cmd(int f, int n)
{
	return (magit_do_tag_create(0, f, n));
}

/* t a: create an annotated tag at HEAD (prompts for name + message). */
static int
magit_tag_annotate_cmd(int f, int n)
{
	return (magit_do_tag_create(1, f, n));
}

/* t k: delete a tag (the tag at point, else prompt). */
static int
magit_tag_delete_cmd(int f, int n)
{
	char	*path = NULL;
	char	 name[PATH_MAX], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind == MG_LINE_TAG)
		(void)strlcpy(name, path, sizeof(name));
	else if (eread("Delete tag: ", name, sizeof(name), EFNEW | EFCR) ==
	    NULL || name[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_tag_delete(cwd, name) != 1) {
		ewprintf("Tag delete failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* W a: add a worktree (prompts for a name + path). */
static int
magit_worktree_add_cmd(int f, int n)
{
	char	name[PATH_MAX], path[PATH_MAX], cwd[PATH_MAX];

	if (eread("Worktree name: ", name, sizeof(name), EFNEW | EFCR) == NULL ||
	    name[0] == '\0')
		return (ABORT);
	if (eread("Worktree path: ", path, sizeof(path), EFNEW | EFCR) == NULL ||
	    path[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_worktree_add(cwd, name, path) != 1) {
		ewprintf("Worktree add failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* W k: delete a worktree (the one at point, else prompt; confirms). */
static int
magit_worktree_delete_cmd(int f, int n)
{
	char	*path = NULL;
	char	 name[PATH_MAX], prompt[PATH_MAX + 32], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind == MG_LINE_WORKTREE)
		(void)strlcpy(name, path, sizeof(name));
	else if (eread("Delete worktree: ", name, sizeof(name), EFNEW | EFCR) ==
	    NULL || name[0] == '\0')
		return (ABORT);
	(void)snprintf(prompt, sizeof(prompt), "Delete worktree %s", name);
	if (eyesno(prompt) != TRUE)
		return (FALSE);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_worktree_remove(cwd, name) != 1) {
		ewprintf("Worktree delete failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* m: merge a branch into HEAD (the branch at point, else prompt). */
static int
magit_merge(int f, int n)
{
	char	*path = NULL;
	char	 name[PATH_MAX], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind == MG_LINE_BRANCH)
		(void)strlcpy(name, path, sizeof(name));
	else if (eread("Merge branch: ", name, sizeof(name), EFNEW | EFCR) ==
	    NULL || name[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_merge(cwd, name) != 1) {
		ewprintf("Merge failed (conflicts?)");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* V: revert a commit (prompts for a revision) -- records the inverse on HEAD. */
static int
magit_revert(int f, int n)
{
	char	rev[PATH_MAX], cwd[PATH_MAX];

	if (eread("Revert commit: ", rev, sizeof(rev), EFNEW | EFCR) == NULL ||
	    rev[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_revert(cwd, rev) != 1) {
		ewprintf("Revert failed (conflicts?)");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* X h/m/s: reset HEAD (and the tree, per mode) to a prompted revision. */
static int
magit_do_reset(int mode, int f, int n)
{
	char	rev[PATH_MAX], cwd[PATH_MAX], prompt[40];

	(void)snprintf(prompt, sizeof(prompt), "Reset (%s) to: ",
	    mode == 0 ? "soft" : mode == 2 ? "hard" : "mixed");
	if (eread("%s", rev, sizeof(rev), EFNEW | EFCR, prompt) == NULL ||
	    rev[0] == '\0')
		return (ABORT);
	if (mode == 2 && eyesno("Hard reset discards working-tree changes") != TRUE)
		return (FALSE);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_reset(cwd, rev, mode) != 1) {
		ewprintf("Reset failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

static int
magit_reset_soft(int f, int n)
{
	return (magit_do_reset(0, f, n));
}

static int
magit_reset_mixed(int f, int n)
{
	return (magit_do_reset(1, f, n));
}

static int
magit_reset_hard(int f, int n)
{
	return (magit_do_reset(2, f, n));
}

/*
 * Read a line into `buf` (size `n`) WITHOUT echoing -- for passwords (git's
 * getpass style: nothing is shown). RET finishes, C-g aborts, DEL/^H erases.
 * Returns 1 on success, 0 on abort.
 */
static int
magit_read_secret(const char *prompt, char *buf, int n)
{
	int	c, i = 0;

	ewprintf("%s", prompt);
	for (;;) {
		c = getkey(FALSE);
		if (c == CCHR('G'))			/* C-g: abort */
			return (0);
		if (c == CCHR('M') || c == CCHR('J'))	/* RET: done */
			break;
		if (c == CCHR('H') || c == 0x7f) {	/* erase */
			if (i > 0)
				i--;
			continue;
		}
		if (i < n - 1 && c >= ' ' && c < 0x7f)
			buf[i++] = c;
	}
	buf[i] = '\0';
	return (1);
}

/*
 * Credential prompt handed to the native engine (mg_magit_set_cred_prompt):
 * hidden -> a no-echo password read, else a normal minibuffer read. 1 ok, 0
 * cancel.
 */
static int
magit_cred_prompt(const char *prompt, int hidden, char *out, int outlen)
{
	if (hidden)
		return (magit_read_secret(prompt, out, outlen));
	if (eread("%s", out, (size_t)outlen, EFNEW | EFCR, prompt) == NULL)
		return (0);
	return (1);
}

/* f: fetch from origin (updates remote-tracking refs). */
static int
magit_fetch(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	ewprintf("Fetching from origin...");
	if (mg_magit_fetch(cwd, "origin") != 1) {
		ewprintf("Fetch failed (no origin, or auth required)");
		return (FALSE);
	}
	ewprintf("Fetched from origin");
	return (magit_refresh(f, n));
}

/* F: pull from origin (fetch + merge the upstream into HEAD). */
static int
magit_pull(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	ewprintf("Pulling from origin...");
	if (mg_magit_pull(cwd, "origin") != 1) {
		ewprintf("Pull failed (conflicts, no origin, or auth required)");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* Shared push helper: `force` / `set_upstream` map to the engine flags. */
static int
magit_do_push(int force, int set_upstream, int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	ewprintf("Pushing to origin...");
	if (mg_magit_push(cwd, "origin", force, set_upstream) != 1) {
		ewprintf("Push failed (no origin, non-fast-forward, or auth required)");
		return (FALSE);
	}
	ewprintf("Pushed to origin");
	return (magit_refresh(f, n));
}

/* P p: push the current branch to origin. */
static int
magit_push(int f, int n)
{
	return (magit_do_push(0, 0, f, n));
}

/* P f: force-push the current branch to origin. */
static int
magit_push_force(int f, int n)
{
	return (magit_do_push(1, 0, f, n));
}

/* P u: push the current branch and set it as upstream. */
static int
magit_push_upstream(int f, int n)
{
	return (magit_do_push(0, 1, f, n));
}

/* F r: pull --rebase (fetch then rebase onto the upstream). */
static int
magit_pull_rebase(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	return (magit_rebase_report(mg_magit_pull_rebase(cwd, "origin"),
	    "Pull --rebase", f, n));
}

/*
 * Interpret a rebase bridge result (1 done / 2 paused on conflicts / 0 fail),
 * report it, and refresh. `what` names the action for the failure message.
 */
static int
magit_rebase_report(int code, const char *what, int f, int n)
{
	if (code == 0) {
		ewprintf("%s failed", what);
		return (FALSE);
	}
	if (code == 2)
		ewprintf("Rebase paused: resolve conflicts, then r r "
		    "(continue) / r s (skip) / r a (abort)");
	else
		ewprintf("Rebase complete");
	return (magit_refresh(f, n));
}

/* r u: rebase the current branch onto its upstream. */
static int
magit_rebase_upstream(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase(cwd, "@{u}"),
	    "Rebase onto upstream", f, n));
}

/* r e: rebase the current branch onto a prompted branch/revision. */
static int
magit_rebase_elsewhere(int f, int n)
{
	char	onto[PATH_MAX], cwd[PATH_MAX];

	if (eread("Rebase onto: ", onto, sizeof(onto), EFNEW | EFCR) == NULL ||
	    onto[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase(cwd, onto), "Rebase", f, n));
}

/* r r: continue a paused rebase (after resolving + staging conflicts). */
static int
magit_rebase_continue(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase_continue(cwd),
	    "Rebase continue", f, n));
}

/* r s: skip the current commit of a paused rebase. */
static int
magit_rebase_skip(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase_skip(cwd), "Rebase skip",
	    f, n));
}

/* r a: abort a paused rebase, restoring the pre-rebase state. */
static int
magit_rebase_abort(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_rebase_abort(cwd) != 1) {
		ewprintf("Rebase abort failed");
		return (FALSE);
	}
	ewprintf("Rebase aborted");
	return (magit_refresh(f, n));
}

static int
magit_stage(int f, int n)
{
	char	*path = NULL, *rpath = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk, rhunk, first, last;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	/* With a mark spanning one hunk's diff lines, stage just that region. */
	if (magit_region(&rhunk, &rpath, &first, &last)) {
		if (mg_magit_stage_region(cwd, rpath, rhunk, first, last) != 1) {
			ewprintf("Stage region failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	kind = magit_at_point(&path, &hunk);
	/* On a hunk/diff line, stage just that hunk; on a file line, the file. */
	if (kind == MG_LINE_HUNK || kind == MG_LINE_DIFF) {
		if (mg_magit_stage_hunk(cwd, path, hunk) != 1) {
			ewprintf("Stage hunk failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	if (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED) {
		ewprintf("Nothing to stage on this line");
		return (FALSE);
	}
	if (mg_magit_stage(cwd, path) != 1) {
		ewprintf("Stage failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

static int
magit_unstage(int f, int n)
{
	char	*path = NULL, *rpath = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk, rhunk, first, last;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	/* With a mark spanning one hunk's diff lines, unstage just that region. */
	if (magit_region(&rhunk, &rpath, &first, &last)) {
		if (mg_magit_unstage_region(cwd, rpath, rhunk, first, last) != 1) {
			ewprintf("Unstage region failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	kind = magit_at_point(&path, &hunk);
	/* On a hunk/diff line, unstage just that hunk; on a file line, the file. */
	if (kind == MG_LINE_HUNK || kind == MG_LINE_DIFF) {
		if (mg_magit_unstage_hunk(cwd, path, hunk) != 1) {
			ewprintf("Unstage hunk failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	if (kind != MG_LINE_STAGED) {
		ewprintf("Nothing to unstage on this line");
		return (FALSE);
	}
	if (mg_magit_unstage(cwd, path) != 1) {
		ewprintf("Unstage failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* i: add the file at point (a default of its name) to .gitignore. */
static int
magit_ignore(int f, int n)
{
	char	*path = NULL;
	char	 pat[PATH_MAX], cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED &&
	    kind != MG_LINE_STAGED) {
		ewprintf("Nothing to ignore on this line");
		return (FALSE);
	}
	/* Prefill the prompt with the file at point (EFDEF shows buf's contents);
	 * the user can edit it (e.g. to a glob) before confirming. */
	(void)strlcpy(pat, path, sizeof(pat));
	if (eread("Ignore (pattern): ", pat, sizeof(pat), EFNEW | EFCR | EFDEF) ==
	    NULL || pat[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_ignore(cwd, pat) != 1) {
		ewprintf("Ignore failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* S: stage every change in the repo. */
static int
magit_stage_all(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_stage_all(cwd) != 1) {
		ewprintf("Stage all failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* U: unstage everything (reset the index to HEAD). */
static int
magit_unstage_all(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_unstage_all(cwd) != 1) {
		ewprintf("Unstage all failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

static int
magit_discard(int f, int n)
{
	char	*path = NULL, *rpath = NULL;
	char	 cwd[PATH_MAX];
	char	 prompt[PATH_MAX + 32];
	int	 kind, hunk, rhunk, first, last;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	/* With a mark spanning one hunk's diff lines, discard just that region. */
	if (magit_region(&rhunk, &rpath, &first, &last)) {
		if (eyesno("Discard marked region") != TRUE)
			return (FALSE);
		if (mg_magit_discard_region(cwd, rpath, rhunk, first, last) != 1) {
			ewprintf("Discard region failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	kind = magit_at_point(&path, &hunk);
	/* On a stash line, k drops the stash; on a file line, it discards edits. */
	if (kind == MG_LINE_STASH) {
		(void)snprintf(prompt, sizeof(prompt), "Drop stash@{%d}", hunk);
		if (eyesno(prompt) != TRUE)
			return (FALSE);
		if (mg_magit_stash_drop(cwd, hunk) != 1) {
			ewprintf("Stash drop failed");
			return (FALSE);
		}
		return (magit_refresh(f, n));
	}
	if (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED &&
	    kind != MG_LINE_STAGED) {
		ewprintf("Nothing to discard on this line");
		return (FALSE);
	}
	(void)snprintf(prompt, sizeof(prompt), "Discard changes to %s", path);
	if (eyesno(prompt) != TRUE)
		return (FALSE);
	if (mg_magit_discard(cwd, path) != 1) {
		ewprintf("Discard failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* Which commit operation C-c C-c performs in the message buffer. */
#define MG_COMMIT_NEW		0
#define MG_COMMIT_AMEND		1
#define MG_COMMIT_REWORD	2
static int	magit_commit_op = MG_COMMIT_NEW;

/*
 * Open the editable *magit-commit* buffer for commit operation `op`. For amend
 * and reword the buffer is pre-filled with HEAD's message; C-c C-c then runs
 * the right operation (see magit_commit_finish).
 */
static int
magit_open_commit_buffer(int op)
{
	static int	 initialized = 0;
	struct buffer	*bp;
	char		 cwd[PATH_MAX];

	if (!initialized) {
		maps_add((KEYMAP *)&commitmap, "magit-commit-mode");
		initialized = 1;
	}
	if ((bp = bfind("*magit-commit*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag &= ~BFREADONLY;	/* editable */
	bp->b_modes[1] = name_mode("magit-commit-mode");
	bp->b_nmodes = 1;
	magit_commit_op = op;
	if (showbuffer(bp, curwp, WFFULL) != TRUE)
		return (FALSE);
	curbp = bp;

	/* Pre-fill HEAD's message for amend/reword. */
	if (op != MG_COMMIT_NEW && getcwd(cwd, sizeof(cwd)) != NULL) {
		char	msg[4096];
		int	i, len;

		len = mg_magit_head_message(cwd, msg, sizeof(msg));
		for (i = 0; i < len; i++) {
			if (msg[i] == '\n')
				(void)lnewline();
			else
				(void)linsert(1, (unsigned char)msg[i]);
		}
		curwp->w_dotp = bfirstlp(curbp);	/* edit from the top */
		curwp->w_doto = 0;
		curwp->w_rflag |= WFFULL;
	}
	ewprintf("Commit message; C-c C-c to commit, C-c C-k to abort");
	return (TRUE);
}

/* c c: compose a new commit. */
static int
magit_commit(int f, int n)
{
	return (magit_open_commit_buffer(MG_COMMIT_NEW));
}

/* c a: amend HEAD (edit the pre-filled message; staged changes are included). */
static int
magit_commit_amend(int f, int n)
{
	return (magit_open_commit_buffer(MG_COMMIT_AMEND));
}

/* c w: reword HEAD's message (its tree is unchanged). */
static int
magit_commit_reword(int f, int n)
{
	return (magit_open_commit_buffer(MG_COMMIT_REWORD));
}

/* c e: extend HEAD with the staged changes, keeping its message (no edit). */
static int
magit_commit_extend(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_commit_extend(cwd) != 1) {
		ewprintf("Extend failed (is user.name/user.email set?)");
		return (FALSE);
	}
	(void)magit_refresh(f, n);
	ewprintf("Extended HEAD commit");
	return (TRUE);
}

/*
 * Return to the (refreshed) status buffer. The commit buffer is left hidden
 * (reused on the next commit) rather than killed -- killbuffer() here, while the
 * C-c C-c prefix dispatch is still unwinding, frees state still in use.
 */
static void
magit_commit_leave(struct buffer *commitbp, int refresh)
{
	struct buffer	*sbp;

	(void)commitbp;
	if ((sbp = bfind("*magit-status*", TRUE)) != NULL) {
		(void)showbuffer(sbp, curwp, WFFULL);
		curbp = sbp;
		if (refresh)
			(void)magit_build(sbp);
	}
	sgarbf = TRUE;		/* force a full repaint after the prefix sequence */
}

/* C-c C-c: read the buffer as the message and commit. */
static int
magit_commit_finish(int f, int n)
{
	struct buffer	*cbp;
	struct line	*lp;
	char		 msg[4096];
	char		 cwd[PATH_MAX];
	size_t		 len = 0;
	int		 ll;

	cbp = curbp;
	for (lp = bfirstlp(cbp); lp != cbp->b_headp; lp = lforw(lp)) {
		ll = llength(lp);
		if (len + (size_t)ll + 1 >= sizeof(msg))
			break;
		if (ll > 0) {
			memcpy(msg + len, ltext(lp), ll);
			len += ll;
		}
		msg[len++] = '\n';
	}
	msg[len] = '\0';
	if (len == 0 || msg[0] == '\n') {
		ewprintf("Aborting commit due to empty message");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	switch (magit_commit_op) {
	case MG_COMMIT_AMEND:
		ll = mg_magit_commit_amend(cwd, msg);
		break;
	case MG_COMMIT_REWORD:
		ll = mg_magit_commit_reword(cwd, msg);
		break;
	default:
		ll = mg_magit_commit(cwd, msg);
		break;
	}
	if (ll != 1) {
		ewprintf("Commit failed (is user.name/user.email set?)");
		return (FALSE);
	}
	magit_commit_leave(cbp, TRUE);
	ewprintf(magit_commit_op == MG_COMMIT_NEW ? "Committed" : "Amended");
	return (TRUE);
}

/* C-c C-k: discard the message and return to the status buffer. */
static int
magit_commit_abort(int f, int n)
{
	magit_commit_leave(curbp, FALSE);
	ewprintf("Commit aborted");
	return (TRUE);
}

/* ---- interactive rebase: the *git-rebase-todo* buffer (FM-RB-3b) ---------- */

static const char *
todo_action_name(int action)
{
	switch (action) {
	case 1: return ("drop");
	case 2: return ("squash");
	case 3: return ("fixup");
	case 4: return ("reword");
	default: return ("pick");
	}
}

/* emit callback for mg_magit_rebase_todo: collect each commit (action defaults
 * to pick). Does not touch a buffer -- the buffer is rendered from this state. */
static void
magit_todo_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
	(void)ctx;
	(void)hunk;
	if (kind != MG_LINE_COMMIT || path == NULL ||
	    magit_todo_count >= MAGIT_MAX_TODO)
		return;
	magit_todo[magit_todo_count].action = 0; /* pick */
	(void)strlcpy(magit_todo[magit_todo_count].oid, path, MAGIT_OID_LEN);
	(void)strlcpy(magit_todo[magit_todo_count].text, line,
	    sizeof(magit_todo[0].text));
	magit_todo_count++;
}

/* Render the plan into the *git-rebase-todo* buffer (line i = entry i). */
static int
magit_rebase_todo_build(struct buffer *bp)
{
	struct mgwin	*wp;
	int		 i;

	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	for (i = 0; i < magit_todo_count; i++)
		(void)addlinef(bp, "%s %s", todo_action_name(magit_todo[i].action),
		    magit_todo[i].text);
	(void)addlinef(bp, "%s", "");
	(void)addlinef(bp, "%s", "# p pick  d/k drop  s squash  f fixup  "
	    "w reword  M-n/M-p reorder  C-c C-c run  C-c C-k abort");

	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp) {
			wp->w_dotp = bp->b_dotp;
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_rflag |= WFFULL;
		}
	return (TRUE);
}

/* r i: open the interactive-rebase todo buffer for commits after a prompted
 * base (e.g. a branch, @{u}, or HEAD~N). */
static int
magit_rebase_interactive_cmd(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char		 onto[PATH_MAX], cwd[PATH_MAX];

	if (eread("Rebase interactively onto: ", onto, sizeof(onto),
	    EFNEW | EFCR) == NULL || onto[0] == '\0')
		return (ABORT);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);

	magit_todo_count = 0;
	(void)strlcpy(magit_todo_onto, onto, sizeof(magit_todo_onto));
	if (mg_magit_rebase_todo(cwd, onto, magit_todo_emit, NULL) <= 0) {
		ewprintf("Nothing to rebase onto %s", onto);
		return (FALSE);
	}

	if ((bp = bfind("*git-rebase-todo*", TRUE)) == NULL)
		return (FALSE);
	if (magit_rebase_todo_build(bp) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-rebase-todo-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}

/* Set the action of the entry at point and re-render (keeping point). */
static int
magit_todo_set(int action)
{
	struct buffer	*bp;
	int		 idx = magit_line_index();

	if (idx < 0 || idx >= magit_todo_count) {
		ewprintf("Not on a rebase line");
		return (FALSE);
	}
	magit_todo[idx].action = action;
	if ((bp = bfind("*git-rebase-todo*", FALSE)) == NULL)
		return (FALSE);
	(void)magit_rebase_todo_build(bp);
	magit_goto_index(idx);
	return (TRUE);
}

static int magit_todo_pick(int f, int n)   { return (magit_todo_set(0)); }
static int magit_todo_drop(int f, int n)   { return (magit_todo_set(1)); }
static int magit_todo_squash(int f, int n) { return (magit_todo_set(2)); }
static int magit_todo_fixup(int f, int n)  { return (magit_todo_set(3)); }

/* w: reword -- mark the entry at point reword and collect the new message now
 * (so the executor never has to stop mid-sequence). */
static int
magit_todo_reword(int f, int n)
{
	struct buffer	*bp;
	char		 msg[1024];
	int		 idx = magit_line_index();

	if (idx < 0 || idx >= magit_todo_count) {
		ewprintf("Not on a rebase line");
		return (FALSE);
	}
	if (eread("New message: ", msg, sizeof(msg), EFNEW | EFCR) == NULL ||
	    msg[0] == '\0')
		return (ABORT);
	magit_todo[idx].action = 4; /* reword */
	(void)strlcpy(magit_todo[idx].message, msg,
	    sizeof(magit_todo[idx].message));
	if ((bp = bfind("*git-rebase-todo*", FALSE)) == NULL)
		return (FALSE);
	(void)magit_rebase_todo_build(bp);
	magit_goto_index(idx);
	return (TRUE);
}

/* Move the entry at point by `dir` (+1 down, -1 up), re-render, follow it. */
static int
magit_todo_move(int dir)
{
	struct buffer	*bp;
	int		 i = magit_line_index(), j = i + dir;

	if (i < 0 || i >= magit_todo_count || j < 0 || j >= magit_todo_count)
		return (FALSE);
	{
		struct magit_todo_entry tmp = magit_todo[i];
		magit_todo[i] = magit_todo[j];
		magit_todo[j] = tmp;
	}
	if ((bp = bfind("*git-rebase-todo*", FALSE)) == NULL)
		return (FALSE);
	(void)magit_rebase_todo_build(bp);
	magit_goto_index(j);
	return (TRUE);
}

static int magit_todo_down(int f, int n) { return (magit_todo_move(1)); }
static int magit_todo_up(int f, int n)   { return (magit_todo_move(-1)); }

/* Close the todo buffer and return to (refreshing) the status buffer. */
static void
magit_todo_leave(int refresh)
{
	struct buffer	*sbp;

	if ((sbp = bfind("*magit-status*", TRUE)) != NULL) {
		(void)showbuffer(sbp, curwp, WFFULL);
		curbp = sbp;
		if (refresh)
			(void)magit_build(sbp);
	}
	sgarbf = TRUE;
}

/* C-c C-c: execute the plan via the engine, then return to the status buffer. */
static int
magit_todo_execute(int f, int n)
{
	struct mg_magit_rebase_step	steps[MAGIT_MAX_TODO];
	char				cwd[PATH_MAX];
	int				i, rc;

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	for (i = 0; i < magit_todo_count; i++) {
		steps[i].action = magit_todo[i].action;
		steps[i].oid = magit_todo[i].oid;
		steps[i].message =
		    (magit_todo[i].action == 4) ? magit_todo[i].message : NULL;
	}
	rc = mg_magit_rebase_interactive(cwd, magit_todo_onto, steps,
	    magit_todo_count);
	magit_todo_leave(TRUE);
	if (rc != 1) {
		ewprintf("Interactive rebase failed (conflict? -- nothing changed)");
		return (FALSE);
	}
	ewprintf("Interactive rebase complete");
	return (TRUE);
}

/* C-c C-k: discard the plan and return to the status buffer. */
static int
magit_todo_abort(int f, int n)
{
	magit_todo_leave(FALSE);
	ewprintf("Interactive rebase aborted");
	return (TRUE);
}

#endif /* ENABLE_NATIVE_MAGIT */
