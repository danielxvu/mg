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
static int	magit_checkout(int, int);
static int	magit_stage_all(int, int);
static int	magit_unstage_all(int, int);
static int	magit_region(int *, char **, int *, int *);
static int	magit_line_index(void);

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

static PF magit_tab[] = { magit_toggle_expand };
static PF magit_ret[] = { magit_visit };
static PF magit_esc[] = { NULL };		/* ESC -> meta prefix */
static PF magit_qmark[] = { magit_help };
static PF magit_S[] = { magit_stage_all };
static PF magit_U[] = { magit_unstage_all };
static PF magit_a[] = { magit_stash_apply };
static PF magit_b[] = { magit_checkout };
static PF magit_c[] = { NULL };			/* c -> commit menu prefix */
static PF magit_g[] = { magit_refresh };
static PF magit_k[] = { magit_discard };
static PF magit_q[] = { delwind };
static PF magit_s[] = { magit_stage };
static PF magit_u[] = { magit_unstage };

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
static struct KEYMAPE (14) magitmap = {
	14,
	14,
	rescan,
	{
		{ CCHR('I'), CCHR('I'), magit_tab, NULL },	/* TAB: expand/collapse */
		{ CCHR('M'), CCHR('M'), magit_ret, NULL },	/* RET: visit file */
		{ CCHR('['), CCHR('['), magit_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ '?', '?', magit_qmark, NULL },		/* ?: key help */
		{ 'S', 'S', magit_S, NULL },			/* S: stage all */
		{ 'U', 'U', magit_U, NULL },			/* U: unstage all */
		{ 'a', 'a', magit_a, NULL },			/* a: apply stash */
		{ 'b', 'b', magit_b, NULL },			/* b: checkout branch */
		{ 'c', 'c', magit_c, (KEYMAP *)&magit_commitmenu }, /* c: commit menu */
		{ 'g', 'g', magit_g, NULL },
		{ 'k', 'k', magit_k, NULL },
		{ 'q', 'q', magit_q, NULL },
		{ 's', 's', magit_s, NULL },
		{ 'u', 'u', magit_u, NULL }
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

/* emit callback: record the line's kind/path, then append it to the buffer. */
static void
magit_emit(void *ctx, const char *line, int kind, const char *path, int hunk)
{
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

/* TAB: expand/collapse the inline diff for the file at point. */
static int
magit_toggle_expand(int f, int n)
{
	char	*path = NULL;
	int	 hunk, kind, i;

	kind = magit_at_point(&path, &hunk);
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
		"  TAB      expand / collapse the inline diff",
		"  RET      visit the file at point (other window)",
		"  M-n/M-p  next / previous section",
		"  s        stage the file/hunk at point (or marked region)",
		"  u        unstage the file/hunk at point (or marked region)",
		"  S / U    stage all / unstage all",
		"  k        discard changes / drop the stash at point",
		"  a        apply the stash at point",
		"  b        check out the branch at point",
		"  c c/a/e/w  commit / amend / extend / reword",
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
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	char	 prompt[PATH_MAX + 32];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
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

#endif /* ENABLE_NATIVE_MAGIT */
