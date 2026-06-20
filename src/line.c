/*	$OpenBSD: line.c,v 1.63 2021/03/01 10:51:14 lum Exp $	*/

/* This file is in the public domain. */

/*
 *		Text line handling.
 *
 * The functions in this file are a general set of line management
 * utilities. They are the only routines that touch the text. They
 * also touch the buffer and window structures to make sure that the
 * necessary updating gets done.
 *
 * Note that this code only updates the dot and mark values in the window
 * list.  Since all the code acts on the current window, the buffer that
 * we are editing must be displayed, which means that "b_nwnd" is non-zero,
 * which means that the dot and mark values in the buffer headers are
 * nonsense.
 */

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"

#ifdef ENABLE_CPP_UPGRADES
/* lalloc/lrealloc/lfreestore + the accessors come from the mg.line C++ module
 * (src/line/storage.cpp); they are declared in def.h. */
#else
/*
 * The private storage layout of a line (task C2a-3). The rest of the editor
 * sees only an incomplete `struct line;` in def.h and reaches lines through the
 * accessors below, so this representation can change without a ripple. Under
 * ENABLE_CPP_UPGRADES the whole storage core (this struct, the accessors,
 * lalloc/lrealloc, lfreestore) instead comes from the mg.line C++ module
 * (vector<char>-backed); see src/line/. Each line holds the used size, the size
 * of the text array, and the text; the end-of-line is implied, not stored.
 */
struct line {
	struct line	*l_fp;		/* Link to the next line	 */
	struct line	*l_bp;		/* Link to the previous line	 */
	int		 l_size;	/* Allocated size		 */
	int		 l_used;	/* Used size			 */
	char		*l_text;	/* Content of the line		 */
};

/* The line accessors, out of line so the layout above stays private. */
struct line *lforw(const struct line *lp)	{ return (lp->l_fp); }
struct line *lback(const struct line *lp)	{ return (lp->l_bp); }
int	lgetc(const struct line *lp, int n)	{ return (CHARMASK(lp->l_text[n])); }
void	lputc(struct line *lp, int n, int c)	{ lp->l_text[n] = c; }
int	llength(const struct line *lp)		{ return (lp->l_used); }
char	*ltext(const struct line *lp)		{ return (lp->l_text); }
int	lsize(const struct line *lp)		{ return (lp->l_size); }
void	lsetlen(struct line *lp, int n)	{ lp->l_used = n; }
void	lsetforw(struct line *lp, struct line *lq) { lp->l_fp = lq; }
void	lsetback(struct line *lp, struct line *lq) { lp->l_bp = lq; }
#endif /* !ENABLE_CPP_UPGRADES */

int	casereplace = TRUE;

/*
 * Preserve the case of the replaced string.
 */
int
setcasereplace(int f, int n)
{
	if (f & FFARG)
		casereplace = n > 0;
	else
		casereplace = !casereplace;
	ewprintf("Case-replace is %sabled", casereplace ? "en" : "dis");
	return (TRUE);
}

#ifndef ENABLE_CPP_UPGRADES
/*
 * Allocate a new line of size `used'.  lrealloc() can be called if the line
 * ever needs to grow beyond that. (Under ENABLE_CPP_UPGRADES, lalloc/lrealloc
 * come from the mg.line C++ module instead.)
 */
struct line *
lalloc(int used)
{
	struct line *lp;

	if ((lp = malloc(sizeof(*lp))) == NULL)
		return (NULL);
	lp->l_text = NULL;
	lp->l_size = 0;
	lp->l_used = used;	/* XXX */
	if (lrealloc(lp, used) == FALSE) {
		free(lp);
		return (NULL);
	}
	return (lp);
}

int
lrealloc(struct line *lp, int newsize)
{
	char *tmp;

	if (lp->l_size < newsize) {
		if ((tmp = realloc(lp->l_text, newsize)) == NULL)
			return (FALSE);
		lp->l_text = tmp;
		lp->l_size = newsize;
	}
	return (TRUE);
}
#endif /* !ENABLE_CPP_UPGRADES */

/*
 * Delete line "lp".  Fix all of the links that might point to it (they are
 * moved to offset 0 of the next line.  Unlink the line from whatever buffer
 * it might be in, and release the memory.  The buffers are updated too; the
 * magic conditions described in the above comments don't hold here.
 */
void
lfree(struct line *lp)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_linep == lp)
			wp->w_linep = lforw(lp);
		if (wp->w_dotp == lp) {
			wp->w_dotp = lforw(lp);
			wp->w_doto = 0;
		}
		if (wp->w_markp == lp) {
			wp->w_markp = lforw(lp);
			wp->w_marko = 0;
		}
	}
	for (bp = bheadp; bp != NULL; bp = bp->b_bufp) {
		if (bp->b_nwnd == 0) {
			if (bp->b_dotp == lp) {
				bp->b_dotp = lforw(lp);
				bp->b_doto = 0;
			}
			if (bp->b_markp == lp) {
				bp->b_markp = lforw(lp);
				bp->b_marko = 0;
			}
		}
	}
	lsetforw(lback(lp), lforw(lp));
	lsetback(lforw(lp), lback(lp));
#ifdef ENABLE_CPP_UPGRADES
	lfreestore(lp);		/* C++ delete (frees the vector<char> too) */
#else
	free(lp->l_text);
	free(lp);
#endif
}

/*
 * This routine is called when a character changes in place in the current
 * buffer. It updates all of the required flags in the buffer and window
 * system. The flag used is passed as an argument; if the buffer is being
 * displayed in more than 1 window we change EDIT to HARD. Set MODE if the
 * mode line needs to be updated (the "*" has to be set).
 */
void
lchange(int flag)
{
	struct mgwin	*wp;

	/* update mode lines if this is the first change. */
	if ((curbp->b_flag & BFCHG) == 0) {
		flag |= WFMODE;
		curbp->b_flag |= BFCHG;
	}
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == curbp) {
			wp->w_rflag |= flag;
			if (wp != curwp)
				wp->w_rflag |= WFFULL;
		}
	}
}

/*
 * Insert "n" copies of the character "c" at the current location of dot.
 * In the easy case all that happens is the text is stored in the line.
 * In the hard case, the line has to be reallocated.  When the window list
 * is updated, take special care; I screwed it up once.  You always update
 * dot in the current window.  You update mark and a dot in another window
 * if it is greater than the place where you did the insert. Return TRUE
 * if all is well, and FALSE on errors.
 */
int
linsert(int n, int c)
{
	struct line	*lp1;
	struct mgwin	*wp;
	RSIZE	 i;
	int	 doto;
	int s;

	if (!n)
		return (TRUE);

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read only");
		return (FALSE);
	}

	lchange(WFEDIT);

	/* current line */
	lp1 = curwp->w_dotp;

	/* special case for the end */
	if (lp1 == curbp->b_headp) {
		struct line *lp2, *lp3;

		/* now should only happen in empty buffer */
		if (curwp->w_doto != 0) {
			dobeep();
			ewprintf("bug: linsert");
			return (FALSE);
		}
		/* allocate a new line */
		if ((lp2 = lalloc(n)) == NULL)
			return (FALSE);
		/* previous line */
		lp3 = lback(lp1);
		/* link in */
		lsetforw(lp3, lp2);
		lsetforw(lp2, lp1);
		lsetback(lp1, lp2);
		lsetback(lp2, lp3);
		for (i = 0; i < n; ++i)
			lputc(lp2, i, c);
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_linep == lp1)
				wp->w_linep = lp2;
			if (wp->w_dotp == lp1)
				wp->w_dotp = lp2;
			if (wp->w_markp == lp1)
				wp->w_markp = lp2;
		}
		undo_add_insert(lp2, 0, n);
		curwp->w_doto = n;
		return (TRUE);
	}
	/* save for later */
	doto = curwp->w_doto;

	if ((llength(lp1) + n) > lsize(lp1)) {
		if (lrealloc(lp1, llength(lp1) + n) == FALSE)
			return (FALSE);
	}
	lsetlen(lp1, llength(lp1) + n);
	if (llength(lp1) != n)
		memmove(&ltext(lp1)[doto + n], &ltext(lp1)[doto],
		    llength(lp1) - n - doto);

	/* Add the characters */
	for (i = 0; i < n; ++i)
		lputc(lp1, doto + i, c);
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_dotp == lp1) {
			if (wp == curwp || wp->w_doto > doto)
				wp->w_doto += n;
		}
		if (wp->w_markp == lp1) {
			if (wp->w_marko > doto)
				wp->w_marko += n;
		}
	}
	undo_add_insert(curwp->w_dotp, doto, n);
	return (TRUE);
}

/*
 * Do the work of inserting a newline at the given line/offset.
 * If mark is on the current line, we may have to move the markline
 * to keep line numbers in sync.
 * lnewline_at assumes the current buffer is writable. Checking for
 * this fact should be done by the caller.
 */
int
lnewline_at(struct line *lp1, int doto)
{
	struct line	*lp2;
	struct mgwin	*wp;
	int	 	 nlen, tcurwpdotline;

	lchange(WFFULL);

	curwp->w_bufp->b_lines++;
	/* Check if mark is past dot (even on current line) */
	if (curwp->w_markline > curwp->w_dotline  ||
	   (curwp->w_dotline == curwp->w_markline &&
	    curwp->w_marko >= doto))
		curwp->w_markline++;

	tcurwpdotline = curwp->w_dotline;

	/* If start of line, allocate a new line instead of copying */
	if (doto == 0) {
		/* new first part */
		if ((lp2 = lalloc(0)) == NULL)
			return (FALSE);
		lsetback(lp2, lback(lp1));
		lsetforw(lback(lp1), lp2);
		lsetforw(lp2, lp1);
		lsetback(lp1, lp2);
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_linep == lp1)
				wp->w_linep = lp2;
			if (wp->w_dotline >= tcurwpdotline &&
			    wp->w_bufp == curwp->w_bufp)
				wp->w_dotline++;
		}
		undo_add_boundary(FFRAND, 1);
		undo_add_insert(lp2, 0, 1);
		undo_add_boundary(FFRAND, 1);
		return (TRUE);
	}

	/* length of new part */
	nlen = llength(lp1) - doto;

	/* new second half line */
	if ((lp2 = lalloc(nlen)) == NULL)
		return (FALSE);
	if (nlen != 0)
		bcopy(&ltext(lp1)[doto], &ltext(lp2)[0], nlen);
	lsetlen(lp1, doto);
	lsetback(lp2, lp1);
	lsetforw(lp2, lforw(lp1));
	lsetforw(lp1, lp2);
	lsetback(lforw(lp2), lp2);
	/* Windows */
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_dotp == lp1 && wp->w_doto >= doto) {
			wp->w_dotp = lp2;
			wp->w_doto -= doto;
			wp->w_dotline++;
		} else if (wp->w_dotline > tcurwpdotline &&
			   wp->w_bufp == curwp->w_bufp)
			wp->w_dotline++;
		if (wp->w_markp == lp1 && wp->w_marko >= doto) {
			wp->w_markp = lp2;
			wp->w_marko -= doto;
		}
	}
	undo_add_boundary(FFRAND, 1);
	undo_add_insert(lp1, llength(lp1), 1);
	undo_add_boundary(FFRAND, 1);
	return (TRUE);
}

/*
 * Insert a newline into the buffer at the current location of dot in the
 * current window.
 */
int
lnewline(void)
{
	int s;

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read only");
		return (FALSE);
	}
	return (lnewline_at(curwp->w_dotp, curwp->w_doto));
}

/*
 * This function deletes "n" bytes, starting at dot. (actually, n+1, as the
 * newline is included) It understands how to deal with end of lines, etc.
 * It returns TRUE if all of the characters were deleted, and FALSE if
 * they were not (because dot ran into the end of the buffer).
 * The "kflag" indicates either no insertion, or direction  of insertion
 * into the kill buffer.
 */
int
ldelete(RSIZE n, int kflag)
{
	struct line	*dotp;
	RSIZE		 chunk;
	struct mgwin	*wp;
	int		 doto;
	char		*cp1, *cp2;
	size_t		 len;
	char		*sv = NULL;
	int		 end;
	int		 s;
	int		 rval = FALSE;

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read only");
		goto out;
	}
	len = n;
	if ((sv = calloc(1, len + 1)) == NULL)
		goto out;
	end = 0;

	undo_add_delete(curwp->w_dotp, curwp->w_doto, n, (kflag & KREG));

	while (n != 0) {
		dotp = curwp->w_dotp;
		doto = curwp->w_doto;
		/* Hit the end of the buffer */
		if (dotp == curbp->b_headp)
			goto out;
		/* Size of the chunk */
		chunk = llength(dotp) - doto;

		if (chunk > n)
			chunk = n;
		/* End of line, merge */
		if (chunk == 0) {
			if (dotp == blastlp(curbp))
				goto out;
			lchange(WFFULL);
			if (ldelnewline() == FALSE)
				goto out;
			end = strlcat(sv, curbp->b_nlchr, len + 1);
			--n;
			continue;
		}
		lchange(WFEDIT);
		/* Scrunch text */
		cp1 = &ltext(dotp)[doto];
		memcpy(&sv[end], cp1, chunk);
		end += chunk;
		sv[end] = '\0';
		for (cp2 = cp1 + chunk; cp2 < &ltext(dotp)[llength(dotp)];
		    cp2++)
			*cp1++ = *cp2;
		lsetlen(dotp, llength(dotp) - (int)chunk);
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_dotp == dotp && wp->w_doto >= doto) {
				wp->w_doto -= chunk;
				if (wp->w_doto < doto)
					wp->w_doto = doto;
			}
			if (wp->w_markp == dotp && wp->w_marko >= doto) {
				wp->w_marko -= chunk;
				if (wp->w_marko < doto)
					wp->w_marko = doto;
			}
		}
		n -= chunk;
	}
	if (kchunk(sv, (RSIZE)len, kflag) != TRUE)
		goto out;
	rval = TRUE;
out:
	free(sv);
	return (rval);
}

/*
 * Delete a newline and join the current line with the next line. If the next
 * line is the magic header line always return TRUE; merging the last line
 * with the header line can be thought of as always being a successful
 * operation.  Even if nothing is done, this makes the kill buffer work
 * "right". If the mark is past the dot (actually, markline > dotline),
 * decrease the markline accordingly to keep line numbers in sync.
 * Easy cases can be done by shuffling data around.  Hard cases
 * require that lines be moved about in memory.  Return FALSE on error and
 * TRUE if all looks ok. We do not update w_dotline here, as deletes are done
 * after moves.
 */
int
ldelnewline(void)
{
	struct line	*lp1, *lp2, *lp3;
	struct mgwin	*wp;
	int s;

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read only");
		return (FALSE);
	}

	lp1 = curwp->w_dotp;
	lp2 = lforw(lp1);
	/* at the end of the buffer */
	if (lp2 == curbp->b_headp)
		return (TRUE);
	/* Keep line counts in sync */
	curwp->w_bufp->b_lines--;
	if (curwp->w_markline > curwp->w_dotline)
		curwp->w_markline--;
	if (llength(lp2) <= lsize(lp1) - llength(lp1)) {
		bcopy(&ltext(lp2)[0], &ltext(lp1)[llength(lp1)], llength(lp2));
		for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
			if (wp->w_linep == lp2)
				wp->w_linep = lp1;
			if (wp->w_dotp == lp2) {
				wp->w_dotp = lp1;
				wp->w_doto += llength(lp1);
			}
			if (wp->w_markp == lp2) {
				wp->w_markp = lp1;
				wp->w_marko += llength(lp1);
			}
		}
		lsetlen(lp1, llength(lp1) + llength(lp2));
		lsetforw(lp1, lforw(lp2));
		lsetback(lforw(lp2), lp1);
#ifdef ENABLE_CPP_UPGRADES
		lfreestore(lp2);
#else
		free(lp2);
#endif
		return (TRUE);
	}
	if ((lp3 = lalloc(llength(lp1) + llength(lp2))) == NULL)
		return (FALSE);
	bcopy(&ltext(lp1)[0], &ltext(lp3)[0], llength(lp1));
	bcopy(&ltext(lp2)[0], &ltext(lp3)[llength(lp1)], llength(lp2));
	lsetforw(lback(lp1), lp3);
	lsetforw(lp3, lforw(lp2));
	lsetback(lforw(lp2), lp3);
	lsetback(lp3, lback(lp1));
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_linep == lp1 || wp->w_linep == lp2)
			wp->w_linep = lp3;
		if (wp->w_dotp == lp1)
			wp->w_dotp = lp3;
		else if (wp->w_dotp == lp2) {
			wp->w_dotp = lp3;
			wp->w_doto += llength(lp1);
		}
		if (wp->w_markp == lp1)
			wp->w_markp = lp3;
		else if (wp->w_markp == lp2) {
			wp->w_markp = lp3;
			wp->w_marko += llength(lp1);
		}
	}
#ifdef ENABLE_CPP_UPGRADES
	lfreestore(lp1);
	lfreestore(lp2);
#else
	free(lp1);
	free(lp2);
#endif
	return (TRUE);
}

/*
 * Replace plen characters before dot with argument string.  Control-J
 * characters in st are interpreted as newlines.  There is a casehack
 * disable flag (normally it likes to match case of replacement to what
 * was there).
 */
int
lreplace(RSIZE plen, char *st)
{
	RSIZE	rlen;	/* replacement length		 */
	struct line *lp;
	RSIZE n;
	int s, doto, is_query_capitalised = 0, is_query_allcaps = 0;
	int is_replace_alllower = 0;
	char *repl = NULL;

	if ((s = checkdirty(curbp)) != TRUE)
		return (s);
	if (curbp->b_flag & BFREADONLY) {
		dobeep();
		ewprintf("Buffer is read only");
		return (FALSE);
	}

	if ((repl = strdup(st)) == NULL) {
		dobeep();
		ewprintf("out of memory");
		return (FALSE);
	}
	rlen = strlen(repl);

	undo_boundary_enable(FFRAND, 0);
	(void)backchar(FFARG | FFRAND, (int)plen);

	if (casereplace != TRUE)
		goto done;

	lp = curwp->w_dotp;
	if (ltext(lp) == NULL)
		goto done;
	doto = curwp->w_doto;
	n = plen;

	is_query_capitalised = isupper((unsigned char)lgetc(lp, doto));

	if (is_query_capitalised) {
		for (n = 0, is_query_allcaps = 1; n < plen && is_query_allcaps;
		    n++) {
			is_query_allcaps = !isalpha((unsigned char)lgetc(lp,
			    doto)) || isupper((unsigned char)lgetc(lp, doto));
			doto++;
			if (doto == llength(lp)) {
				doto = 0;
				lp = lforw(lp);
				n++; /* \n is implicit in the buffer */
			}
		}
	}

	for (n = 0, is_replace_alllower = 1; n < rlen && is_replace_alllower;
	    n++)
		is_replace_alllower = !isupper((unsigned char)repl[n]);

	if (is_replace_alllower) {
		if (is_query_allcaps) {
			for (n = 0; n < rlen; n++)
				repl[n] = toupper((unsigned char)repl[n]);
		} else if (is_query_capitalised) {
			repl[0] = toupper((unsigned char)repl[0]);
		}
	}

 done:
	(void)ldelete(plen, KNONE);
	region_put_data(repl, rlen);
	lchange(WFFULL);

	undo_boundary_enable(FFRAND, 1);

	free(repl);
	return (TRUE);
}

/*
 * Allocate and return the supplied line as a C string
 */
char *
linetostr(const struct line *ln)
{
	int	 len;
	char	*line;

	len = llength(ln);
	if (len == INT_MAX)  /* (len + 1) overflow */
		return (NULL);

	if ((line = malloc(len + 1)) == NULL)
		return (NULL);

	(void)memcpy(line, ltext(ln), len);
	line[len] = '\0';

	return (line);
}
