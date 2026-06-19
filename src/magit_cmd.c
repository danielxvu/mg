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

static int	magit_refresh(int, int);
static int	magit_stage(int, int);
static int	magit_unstage(int, int);
static int	magit_discard(int, int);
static int	magit_commit(int, int);
static int	magit_commit_finish(int, int);
static int	magit_commit_abort(int, int);

/*
 * line -> {kind, path} map for the most recent render of *magit-status*. There
 * is a single such buffer, so a single static map suffices. Lines past the cap
 * are still shown but are not stageable.
 */
static struct {
	int	kind;
	char	path[PATH_MAX];
} magit_meta[MAGIT_MAX_LINES];
static int	magit_meta_count;

static PF magit_c[] = { magit_commit };
static PF magit_g[] = { magit_refresh };
static PF magit_k[] = { magit_discard };
static PF magit_q[] = { delwind };
static PF magit_s[] = { magit_stage };
static PF magit_u[] = { magit_unstage };

static struct KEYMAPE (6) magitmap = {
	6,
	6,
	rescan,
	{
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
magit_emit(void *ctx, const char *line, int kind, const char *path)
{
	if (magit_meta_count < MAGIT_MAX_LINES) {
		magit_meta[magit_meta_count].kind = kind;
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
	(void)mg_magit_status_buffer(cwd, magit_emit, bp);

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

/* Resolve the kind/path of the file row under the cursor. */
static int
magit_at_point(char **path_out)
{
	struct line	*lp;
	int		 idx = 0;

	for (lp = bfirstlp(curbp);
	    lp != curwp->w_dotp && lp != curbp->b_headp; lp = lforw(lp))
		idx++;
	if (idx >= magit_meta_count)
		return (MG_LINE_OTHER);
	*path_out = magit_meta[idx].path;
	return (magit_meta[idx].kind);
}

static int
magit_stage(int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind;

	kind = magit_at_point(&path);
	if (kind != MG_LINE_UNTRACKED && kind != MG_LINE_UNSTAGED) {
		ewprintf("Nothing to stage on this line");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
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
	int	 kind;

	kind = magit_at_point(&path);
	if (kind != MG_LINE_STAGED) {
		ewprintf("Nothing to unstage on this line");
		return (FALSE);
	}
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
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
	int	 kind;

	kind = magit_at_point(&path);
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
