/*
 * magit_cmd.c -- the `magit-status` command and its interactive buffer
 * (tasks M3-3, M4-3).
 *
 * Glue between mg's C core (buffers/windows/keymaps) and the native engine's
 * extern "C" bridge. Compiled into mg only when ENABLE_NATIVE_MAGIT is set.
 * The status buffer uses a buffer-local keymap (magit-status-mode):
 *   s  stage the file at point      u  unstage the file at point
 *   g  refresh                       q  close the window
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
static int	magit_commit_finish(int, int);
static int	magit_commit_abort(int, int);
static int	magit_toggle_expand(int, int);
static int	magit_visit(int, int);

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
static PF magit_c[] = { magit_commit };
static PF magit_g[] = { magit_refresh };
static PF magit_k[] = { magit_discard };
static PF magit_q[] = { delwind };
static PF magit_s[] = { magit_stage };
static PF magit_u[] = { magit_unstage };

/* Entries MUST stay in ascending key order -- doscan() relies on it. */
static struct KEYMAPE (8) magitmap = {
	8,
	8,
	rescan,
	{
		{ CCHR('I'), CCHR('I'), magit_tab, NULL },	/* TAB: expand/collapse */
		{ CCHR('M'), CCHR('M'), magit_ret, NULL },	/* RET: visit file */
		{ 'c', 'c', magit_c, NULL },
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

static int
magit_stage(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
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
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
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

static int
magit_discard(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	char	 prompt[PATH_MAX + 32];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED &&
	    kind != MG_LINE_STAGED) {
		ewprintf("Nothing to discard on this line");
		return (FALSE);
	}
	(void)snprintf(prompt, sizeof(prompt), "Discard changes to %s", path);
	if (eyesno(prompt) != TRUE)
		return (FALSE);
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if (mg_magit_discard(cwd, path) != 1) {
		ewprintf("Discard failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/* c: open an editable *magit-commit* buffer to compose the message. */
static int
magit_commit(int f, int n)
{
	static int	 initialized = 0;
	struct buffer	*bp;

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
	if (showbuffer(bp, curwp, WFFULL) != TRUE)
		return (FALSE);
	curbp = bp;
	ewprintf("Commit message; C-c C-c to commit, C-c C-k to abort");
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
	if (mg_magit_commit(cwd, msg) != 1) {
		ewprintf("Commit failed (is user.name/user.email set?)");
		return (FALSE);
	}
	magit_commit_leave(cbp, TRUE);
	ewprintf("Committed");
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
