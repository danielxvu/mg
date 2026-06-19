/*
 * magit_cmd.c -- the `magit-status` command (task M3-3).
 *
 * Glue between mg's C core (buffers/windows) and the native engine's extern "C"
 * bridge. Compiled into mg only when ENABLE_NATIVE_MAGIT is set. Mirrors the
 * listbuffers() pattern: build a read-only buffer, pop it up.
 *
 * This file is in the public domain.
 */
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "def.h"
#include "magit/bridge.h"

#ifdef ENABLE_NATIVE_MAGIT

/* emit callback: append one composed line to the buffer (ctx). */
static void
magit_emit(void *ctx, const char *line)
{
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

/*
 * magit-status: show the working-tree status in a read-only *magit-status*
 * buffer -- branch/HEAD, untracked/unstaged/staged sections, recent commits.
 */
int
magit_status(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return (FALSE);
	if ((bp = bfind("*magit-status*", TRUE)) == NULL)
		return (FALSE);
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	(void)mg_magit_status_buffer(cwd, magit_emit, bp);

	/* Show from the top of the buffer, not where addlinef left the dot. */
	bp->b_dotp = bfirstlp(bp);
	bp->b_doto = 0;

	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;

	return (TRUE);
}

#endif /* ENABLE_NATIVE_MAGIT */
