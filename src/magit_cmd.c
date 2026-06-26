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
#include "../syntax/neomg_syntax.h"

#define MAGIT_MAX_EXPANDED 64

/*
 * Transient menus: magit's signature popup. A prefix key pops a window listing
 * the action keys (dispatched through `map`) and any sticky `infixes` -- toggle
 * flags / values that persist in the popup and feed the action commands. The
 * popup stays open while infix keys are pressed; an action key or ESC closes it.
 */
enum magit_infix_kind { MAGIT_INFIX_FLAG, MAGIT_INFIX_VALUE };
struct magit_infix {
	KCHAR			 key;
	const char		*arg;	/* shown label, e.g. "-f --force" */
	enum magit_infix_kind	 kind;
	int			 on;	/* FLAG: 0/1 toggle state */
	char			 value[64];	/* VALUE: current text */
};
struct magit_menu_item {
	KCHAR		 key;
	const char	*desc;
};
struct magit_menu {
	const char			*title;
	KEYMAP				*map;		/* action-key dispatch */
	const struct magit_menu_item	*items;
	int				 n_items;
	struct magit_infix		*infixes;	/* mutable; NULL if none */
	int				 n_infixes;
};

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
static int	magit_bisect_start_cmd(int, int);
static int	magit_bisect_good(int, int);
static int	magit_bisect_bad(int, int);
static int	magit_bisect_reset_cmd(int, int);
static int	magit_bisect_report(int, const char *, int, int);
static int	magit_transient(struct magit_menu *, int, int);
static int	magit_conflict_ours(int, int);
static int	magit_conflict_theirs(int, int);
static int	magit_menu_conflict(int, int);
static int	magit_ediff(int, int);
static int	magit_ediff_ours(int, int);
static int	magit_ediff_theirs(int, int);
static int	magit_ediff_both(int, int);
static int	magit_ediff_next(int, int);
static int	magit_ediff_prev(int, int);
static int	magit_ediff_sc_down(int, int);
static int	magit_ediff_sc_up(int, int);
static int	magit_ediff_sc_pgdn(int, int);
static int	magit_ediff_sc_pgup(int, int);
static int	magit_ediff_quit(int, int);
static int	magit_menu_pull(int, int);
static int	magit_menu_push(int, int);
static int	magit_menu_reset(int, int);
static int	magit_menu_worktree(int, int);
static int	magit_menu_bisect(int, int);
static int	magit_menu_branch(int, int);
static int	magit_menu_commit(int, int);
static int	magit_menu_log(int, int);
static int	magit_menu_rebase(int, int);
static int	magit_menu_tag(int, int);
static int	magit_menu_stash(int, int);
static int	magit_stage_all(int, int);
static int	magit_unstage_all(int, int);
static int	magit_ignore(int, int);
static int	magit_region(int *, char **, int *, int *);
static int	magit_line_index(void);
static const char *magit_log_oid_at_point(void);
static int	magit_at_point(char **, int *);
static void	magit_log_emit(void *, const char *, int, const char *, int);
static void	magit_plain_emit(void *, const char *, int, const char *, int);
static void	magit_diff_emit(void *, const char *, int, const char *, int);
static int	magit_process(int, int);
void		magit_cell_color_reset(void);	/* invalidate the per-line memo */
static int	magit_log(int, int);
static int	magit_log_file(int, int);
static int	magit_log_open(int, int);
static int	magit_log_graph_cmd(int, int);
static int	magit_log_range_cmd(int, int);
static int	magit_log_pickaxe_s(int, int);
static int	magit_log_pickaxe_g(int, int);
static void	magit_log_query_reset(void);
static int	magit_blame(int, int);
static int	magit_cherrypick(int, int);
static int	magit_log_visit(int, int);
static int	magit_log_refresh(int, int);
static int	magit_log_revert(int, int);
static int	magit_log_note(int, int);
static int	magit_reflog(int, int);
static int	magit_reflog_refresh(int, int);
static int	magit_reflog_build(struct buffer *);
static int	magit_reflog_reset_soft(int, int);
static int	magit_reflog_reset_mixed(int, int);
static int	magit_reflog_reset_hard(int, int);
static int	magit_reflog_reset_menu(int, int);
static int	magit_merge(int, int);
static int	magit_revert(int, int);
static int	magit_reset_soft(int, int);
static int	magit_reset_mixed(int, int);
static int	magit_reset_hard(int, int);
static int	magit_fetch(int, int);
static int	magit_pull(int, int);
static int	magit_push(int, int);
static int	magit_pull_rebase(int, int);
static int	magit_rebase_report(int, const char *, int, int);
static int	magit_apply_report(int, const char *, int, int);
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
static int	magit_todo_edit(int, int);
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

/* Per-line kind/path for the most recent *magit-commit* render. */
static struct {
	int	kind;
	char	path[PATH_MAX];
} magit_commit_meta[MAGIT_MAX_LINES];
static int	magit_commit_meta_count;

/*
 * The two syntax-colored buffers, cached so display.c's per-cell hook can gate
 * on a pointer compare instead of a bfind() linked-list walk per cell. Set when
 * each buffer is (re)built (status build path / magit_show_rev); a NULL means
 * "not built yet" and never matches a real buffer.
 */
static struct buffer	*magit_status_bp;
static struct buffer	*magit_commit_bp;

/* Paths whose diffs are currently expanded inline. */
static char	magit_expanded[MAGIT_MAX_EXPANDED][PATH_MAX];
static int	magit_expanded_count;

/*
 * Per-line oid map shared by *magit-log* AND *magit-reflog*.  The map is
 * rebuilt by magit_log_build(), magit_reflog_build(), and the async log-file
 * wake (MG_ASYNC_LOG_FILE).  magit_log_oid_bp records which buffer owns the
 * current map; magit_log_oid_at_point() returns NULL when curbp differs from
 * that owner so a stale map can never yield a wrong commit oid on a buffer
 * switch (C-x b / C-x o) without a rebuild.
 */
#define MAGIT_OID_LEN 64
static char		 magit_log_oid[MAGIT_MAX_LINES][MAGIT_OID_LEN];
static int		 magit_log_count;
static struct buffer	*magit_log_oid_bp;	/* buffer the oid map was last built for */

/* When non-empty, *magit-log* is filtered to commits touching this file (l f);
 * empty means the whole-repo log (l l). */
static char	magit_log_file_path[PATH_MAX];

/* Max commits shown in *magit-log* -- the `-n` transient infix (see below). */
static int	magit_log_limit = 100;

static int	magit_log_graph;		/* l g: --graph mode */
static char	magit_log_range[256];		/* l r: "A..B" ("" = none) */
static char	magit_log_pickaxe;		/* 0 / 'S' / 'G' */
static char	magit_log_pickaxe_term[256];	/* the -S/-G term */

/* FM-ASYNC-BLAME: the generation of the latest async blame / log-file request.
 * magit_async_apply() only fills a buffer if the ready result's generation
 * still matches -- a newer request (the user moved to another file) supersedes
 * an in-flight one, whose result is then dropped. */
static unsigned	magit_blame_gen;
static unsigned	magit_logfile_gen;

/* For *magit-ediff*: the file being resolved + the conflict-region index per
 * buffer line (-1 for header/blank lines). */
/* *magit-ediff* (3-pane visual conflict resolver): the file, the current
 * region, and the merged-pane lines to render in standout (the active region). */
static char		magit_ediff_path[PATH_MAX];
static int		magit_ediff_region;	/* current conflict region */
static struct buffer	*magit_ediff_merged_bp;	/* the highlighted pane */
static struct buffer	*magit_ediff_ours_bp;	/* ours side pane (syntax color) */
static struct buffer	*magit_ediff_theirs_bp;	/* theirs side pane (syntax color) */
#define MAGIT_EDIFF_HL_MAX 1024
static struct line	*magit_ediff_hl[MAGIT_EDIFF_HL_MAX];
static int		magit_ediff_hl_n;
/* Word-level refinement spans for the side panes: char range [start,end) of
 * `lp` to render in standout (the words unique to that side). */
#define MAGIT_EDIFF_REF_MAX 2048
struct magit_ediff_ref { struct line *lp; int start, end; };
static struct magit_ediff_ref	magit_ediff_ref[MAGIT_EDIFF_REF_MAX];
static int			magit_ediff_ref_n;
/* Per-merged-line alignment: the ours/theirs full-version line that line maps to
 * (for synchronized scrolling). And the active region's side lines, in order,
 * to attach word-refinement ranges to. */
static int		magit_ediff_aln_ours[MAGIT_MAX_LINES];
static int		magit_ediff_aln_theirs[MAGIT_MAX_LINES];
static int		magit_ediff_merged_n;
static struct line	*magit_ediff_oreg[MAGIT_EDIFF_HL_MAX];
static struct line	*magit_ediff_treg[MAGIT_EDIFF_HL_MAX];
static int		magit_ediff_oreg_n, magit_ediff_treg_n;
static int		magit_ediff_refrow;	/* refine-attach row counter */
/* Non-static: lets display.c's per-cell/per-line redisplay loop skip the
 * highlight lookups entirely (one int test) when no ediff session is open. */
int			magit_ediff_active;

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
static PF magit_E[] = { magit_ediff };		/* E -> per-region conflict resolve */
static PF magit_F[] = { magit_menu_pull };			/* F -> pull menu prefix */
static PF magit_P[] = { magit_menu_push };			/* P -> push menu prefix */

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

/* Push menu: the single action P p push; -f/-u are transient infixes. */
static PF push_p[] = { magit_push };

static struct KEYMAPE (1) magit_pushmenu = {
	1,
	1,
	rescan,
	{
		{ 'p', 'p', push_p, NULL }	/* P p: push (with -f/-u infixes) */
	}
};
static PF magit_S[] = { magit_stage_all };
static PF magit_U[] = { magit_unstage_all };
static PF magit_V[] = { magit_revert };
static PF magit_e[] = { magit_menu_conflict };	/* e -> conflict menu */
static PF magit_f[] = { magit_fetch };
static PF magit_W[] = { magit_menu_worktree };			/* W -> worktree menu prefix */
static PF magit_X[] = { magit_menu_reset };			/* X -> reset menu prefix */
static PF magit_Z[] = { magit_menu_bisect };			/* Z -> bisect menu prefix */

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

/* Bisect menu: Z s start, Z b mark bad, Z g mark good, Z r reset. Ascending. */
static PF bisect_b[] = { magit_bisect_bad };
static PF bisect_g[] = { magit_bisect_good };
static PF bisect_r[] = { magit_bisect_reset_cmd };
static PF bisect_s[] = { magit_bisect_start_cmd };

static struct KEYMAPE (4) magit_bisectmenu = {
	4,
	4,
	rescan,
	{
		{ 'b', 'b', bisect_b, NULL },	/* Z b: mark bad */
		{ 'g', 'g', bisect_g, NULL },	/* Z g: mark good */
		{ 'r', 'r', bisect_r, NULL },	/* Z r: reset */
		{ 's', 's', bisect_s, NULL }	/* Z s: start */
	}
};

/* Conflict menu (e, on an unmerged line): e o keep ours, e t keep theirs. */
static PF conflict_o[] = { magit_conflict_ours };
static PF conflict_t[] = { magit_conflict_theirs };

static struct KEYMAPE (2) magit_conflictmenu = {
	2,
	2,
	rescan,
	{
		{ 'o', 'o', conflict_o, NULL },	/* e o: keep ours */
		{ 't', 't', conflict_t, NULL }	/* e t: keep theirs */
	}
};

/* Log menu: l l whole-repo log, l f log of a file. Ascending. */
static PF log_f[] = { magit_log_file };
static PF log_l[] = { magit_log };
static PF log_g[] = { magit_log_graph_cmd };
static PF log_h[] = { magit_reflog };		/* l h: HEAD reflog */
static PF log_r[] = { magit_log_range_cmd };
static PF log_s[] = { magit_log_pickaxe_s };
static PF log_G[] = { magit_log_pickaxe_g };

static struct KEYMAPE (7) magit_logmenu = {
	7,
	7,
	rescan,
	{
		{ 'G', 'G', log_G, NULL },	/* l G: pickaxe -G (regex) */
		{ 'f', 'f', log_f, NULL },	/* l f: log file */
		{ 'g', 'g', log_g, NULL },	/* l g: graph log */
		{ 'h', 'h', log_h, NULL },	/* l h: HEAD reflog */
		{ 'l', 'l', log_l, NULL },	/* l l: log all */
		{ 'r', 'r', log_r, NULL },	/* l r: log range */
		{ 's', 's', log_s, NULL }	/* l s: pickaxe -S (string) */
	}
};
static PF magit_a[] = { magit_stash_apply };
static PF magit_b[] = { magit_menu_branch };			/* b -> branch menu prefix */
static PF magit_c[] = { magit_menu_commit };			/* c -> commit menu prefix */
static PF magit_g[] = { magit_refresh };
static PF magit_i[] = { magit_ignore };
static PF magit_k[] = { magit_discard };
static PF magit_q[] = { delwind };
static PF magit_l[] = { magit_menu_log };			/* l -> log menu prefix */
static PF magit_r[] = { magit_menu_rebase };			/* r -> rebase menu prefix */
static PF magit_s[] = { magit_stage };
static PF magit_t[] = { magit_menu_tag };			/* t -> tag menu prefix */
static PF magit_u[] = { magit_unstage };
static PF magit_z[] = { magit_menu_stash };			/* z -> stash menu prefix */

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

/*
 * *magit-ediff* mode: n/p step regions, a ours, b theirs, RET both, q close;
 * C-n/C-p/C-v/M-v scroll all three panes together (synchronized).
 */
static PF magediff_ret[] = { magit_ediff_both };
static PF magediff_cn[] = { magit_ediff_sc_down };
static PF magediff_cp[] = { magit_ediff_sc_up };
static PF magediff_cv[] = { magit_ediff_sc_pgdn };
static PF magediff_esc[] = { NULL };		/* ESC -> meta (M-v) submap */
static PF magediff_a[] = { magit_ediff_ours };
static PF magediff_b[] = { magit_ediff_theirs };
static PF magediff_n[] = { magit_ediff_next };
static PF magediff_p[] = { magit_ediff_prev };
static PF magediff_q[] = { magit_ediff_quit };
static PF magediff_mv[] = { magit_ediff_sc_pgup };

static struct KEYMAPE (1) magediff_metamap = {
	1,
	1,
	rescan,
	{
		{ 'v', 'v', magediff_mv, NULL }		/* M-v: page up */
	}
};

static struct KEYMAPE (10) magediffmap = {
	10,
	10,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), magediff_ret, NULL },	/* RET: keep both */
		{ CCHR('N'), CCHR('N'), magediff_cn, NULL },	/* C-n: scroll down */
		{ CCHR('P'), CCHR('P'), magediff_cp, NULL },	/* C-p: scroll up */
		{ CCHR('V'), CCHR('V'), magediff_cv, NULL },	/* C-v: page down */
		{ CCHR('['), CCHR('['), magediff_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magediff_metamap },
		{ 'a', 'a', magediff_a, NULL },			/* a: keep ours */
		{ 'b', 'b', magediff_b, NULL },			/* b: keep theirs */
		{ 'n', 'n', magediff_n, NULL },			/* n: next region */
		{ 'p', 'p', magediff_p, NULL },			/* p: prev region */
		{ 'q', 'q', magediff_q, NULL }			/* q: close */
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

/* *magit-process* view keymap: q closes (read-only process log). */
static PF magprocess_q[] = { delwind };

static struct KEYMAPE (1) magprocessmap = {
	1,
	1,
	rescan,
	{
		{ 'q', 'q', magprocess_q, NULL }		/* q: close */
	}
};

static PF magit_dollar[] = { magit_process };

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
 * *magit-reflog* keymap: RET shows the entry's diff (reuses magit_log_visit),
 * g refreshes, q closes. ESC prefix forwards to magit_metamap (M-n/M-p).
 * Entries MUST be ascending: CCHR('M')=13, CCHR('[')=27, 'g'=103, 'q'=113.
 */
static PF reflog_ret[] = { magit_log_visit };		/* RET: show entry diff */
static PF reflog_esc[] = { NULL };			/* ESC: meta prefix */
static PF reflog_g[]   = { magit_reflog_refresh };	/* g: refresh */
static PF reflog_q[]   = { delwind };			/* q: close */

/* Reset submenu for *magit-reflog*: x h/m/s. Entries ascending. */
static PF reflog_reset_h[] = { magit_reflog_reset_hard };
static PF reflog_reset_m[] = { magit_reflog_reset_mixed };
static PF reflog_reset_s[] = { magit_reflog_reset_soft };

/* Entries MUST stay ascending: 'h'=104, 'm'=109, 's'=115. */
static struct KEYMAPE (3) magit_reflogresetmenu = {
	3,
	3,
	rescan,
	{
		{ 'h', 'h', reflog_reset_h, NULL },	/* x h: hard (confirms) */
		{ 'm', 'm', reflog_reset_m, NULL },	/* x m: mixed */
		{ 's', 's', reflog_reset_s, NULL }	/* x s: soft */
	}
};

static PF reflog_x[] = { magit_reflog_reset_menu };	/* x: reset prefix */

/*
 * *magit-reflog* keymap: RET shows the entry's diff (reuses magit_log_visit),
 * g refreshes, q closes, x opens the reset transient. ESC prefix forwards to
 * magit_metamap (M-n/M-p).
 * Entries MUST be ascending: CCHR('M')=13, CCHR('[')=27, 'g'=103, 'q'=113, 'x'=120.
 */
static struct KEYMAPE (5) magit_reflogmap = {
	5,
	5,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), reflog_ret, NULL },	/* RET: show entry */
		{ CCHR('['), CCHR('['), reflog_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ 'g', 'g', reflog_g, NULL },			/* g: refresh */
		{ 'q', 'q', reflog_q, NULL },			/* q: close */
		{ 'x', 'x', reflog_x, NULL }			/* x: reset transient */
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
static PF todo_e[] = { magit_todo_edit };
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

static struct KEYMAPE (9) magit_todomap = {
	9,
	9,
	rescan,
	{
		{ CCHR('C'), CCHR('C'), todo_esc,		/* C-c prefix */
		    (KEYMAP *)&todo_ccmap },
		{ CCHR('['), CCHR('['), todo_esc,		/* ESC prefix */
		    (KEYMAP *)&todo_metamap },
		{ 'd', 'd', todo_d, NULL },			/* d: drop */
		{ 'e', 'e', todo_e, NULL },			/* e: edit (stop) */
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

/*
 * ---- Transient menus ----
 * Descriptors pair each existing submap with key labels (function_name() can't
 * see our static commands) and optional sticky infixes. magit_transient() pops
 * the menu, loops while infix keys toggle state, and dispatches the chosen
 * action key through the submap.
 */

/* Push infixes: read by magit_push (index 0 = force, 1 = set-upstream). */
static struct magit_infix push_infixes[] = {
	{ 'f', "-f --force-with-lease", MAGIT_INFIX_FLAG, 0, "" },
	{ 'u', "-u --set-upstream",     MAGIT_INFIX_FLAG, 0, "" }
};
/* Log infix: -n max-count; mirrored into magit_log_limit on change. */
static struct magit_infix log_infixes[] = {
	{ 'n', "-n --max-count", MAGIT_INFIX_VALUE, 0, "100" }
};

static const struct magit_menu_item pull_items[] = {
	{ 'p', "pull (merge)" }, { 'r', "pull --rebase" }
};
static const struct magit_menu_item push_items[] = {
	{ 'p', "push to origin" }
};
static const struct magit_menu_item reset_items[] = {
	{ 'h', "reset --hard" }, { 'm', "reset --mixed" }, { 's', "reset --soft" }
};
static const struct magit_menu_item worktree_items[] = {
	{ 'a', "add worktree" }, { 'k', "delete worktree" }
};
static const struct magit_menu_item bisect_items[] = {
	{ 'b', "mark bad" }, { 'g', "mark good" }, { 'r', "reset" }, { 's', "start" }
};
static const struct magit_menu_item branch_items[] = {
	{ 'b', "checkout branch at point" }, { 'c', "create" },
	{ 'k', "delete" }, { 'm', "rename" }
};
static const struct magit_menu_item commit_items[] = {
	{ 'a', "amend" }, { 'c', "commit" }, { 'e', "extend" }, { 'w', "reword" }
};
static const struct magit_menu_item log_items[] = {
	{ 'f', "log file" }, { 'l', "log all" }
};
static const struct magit_menu_item rebase_items[] = {
	{ 'a', "abort" }, { 'e', "onto a branch" }, { 'i', "interactive" },
	{ 'r', "continue" }, { 's', "skip" }, { 'u', "onto upstream" }
};
static const struct magit_menu_item tag_items[] = {
	{ 'a', "annotated" }, { 'k', "delete" }, { 't', "create (lightweight)" }
};
static const struct magit_menu_item stash_items[] = {
	{ 'p', "pop" }, { 'z', "push/create" }
};
static const struct magit_menu_item conflict_items[] = {
	{ 'o', "keep ours" }, { 't', "keep theirs" }
};
static const struct magit_menu_item reflog_reset_items[] = {
	{ 'h', "hard (reset working tree)" },
	{ 'm', "mixed (reset index)" },
	{ 's', "soft (move HEAD only)" }
};

#define MENU_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static struct magit_menu pull_menu = { "Pull", (KEYMAP *)&magit_pullmenu,
	pull_items, MENU_N(pull_items), NULL, 0 };
static struct magit_menu push_menu = { "Push", (KEYMAP *)&magit_pushmenu,
	push_items, MENU_N(push_items), push_infixes, MENU_N(push_infixes) };
static struct magit_menu reset_menu = { "Reset", (KEYMAP *)&magit_resetmenu,
	reset_items, MENU_N(reset_items), NULL, 0 };
static struct magit_menu worktree_menu = { "Worktree",
	(KEYMAP *)&magit_worktreemenu, worktree_items, MENU_N(worktree_items),
	NULL, 0 };
static struct magit_menu bisect_menu = { "Bisect", (KEYMAP *)&magit_bisectmenu,
	bisect_items, MENU_N(bisect_items), NULL, 0 };
static struct magit_menu branch_menu = { "Branch", (KEYMAP *)&magit_branchmenu,
	branch_items, MENU_N(branch_items), NULL, 0 };
static struct magit_menu commit_menu = { "Commit", (KEYMAP *)&magit_commitmenu,
	commit_items, MENU_N(commit_items), NULL, 0 };
static struct magit_menu log_menu = { "Log", (KEYMAP *)&magit_logmenu,
	log_items, MENU_N(log_items), log_infixes, MENU_N(log_infixes) };
static struct magit_menu rebase_menu = { "Rebase", (KEYMAP *)&magit_rebasemenu,
	rebase_items, MENU_N(rebase_items), NULL, 0 };
static struct magit_menu tag_menu = { "Tag", (KEYMAP *)&magit_tagmenu,
	tag_items, MENU_N(tag_items), NULL, 0 };
static struct magit_menu stash_menu = { "Stash", (KEYMAP *)&magit_stashmenu,
	stash_items, MENU_N(stash_items), NULL, 0 };
static struct magit_menu conflict_menu = { "Conflict (file at point)",
	(KEYMAP *)&magit_conflictmenu, conflict_items, MENU_N(conflict_items),
	NULL, 0 };
static struct magit_menu reflog_reset_menu = { "Reset to entry",
	(KEYMAP *)&magit_reflogresetmenu, reflog_reset_items,
	MENU_N(reflog_reset_items), NULL, 0 };

/* Render menu `m` into `bp`: title, infixes (with state), then action keys. */
static void
magit_transient_render(struct buffer *bp, struct magit_menu *m)
{
	struct magit_infix	*x;
	int			 i;

	bp->b_flag |= BFIGNDIRTY;
	(void)bclear(bp);
	bp->b_flag |= BFREADONLY;
	(void)addlinef(bp, "%s", (char *)m->title);
	for (i = 0; i < m->n_infixes; i++) {
		x = &m->infixes[i];
		if (x->kind == MAGIT_INFIX_FLAG)
			(void)addlinef(bp, " %c  %-22s %s", x->key,
			    (char *)x->arg, x->on ? "(on)" : "(off)");
		else
			(void)addlinef(bp, " %c  %-22s %s", x->key,
			    (char *)x->arg, x->value);
	}
	if (m->n_infixes > 0)
		(void)addlinef(bp, " ");
	for (i = 0; i < m->n_items; i++)
		(void)addlinef(bp, " %c  %s", m->items[i].key,
		    (char *)m->items[i].desc);
}

/*
 * Pop menu `m` in a window, loop while infix keys toggle/prompt, then dispatch
 * the chosen action key through m->map (ESC / C-g cancels). Tears the popup
 * down and restores focus to the status window before running the action.
 */
static int
magit_transient(struct magit_menu *m, int f, int n)
{
	struct buffer	*bp, *stbp;
	struct mgwin	*wp, *stwp;
	PF		 fn = NULL;
	char		 val[64];
	int		 i, k, handled;

	stwp = curwp;
	stbp = curbp;
	if ((bp = bfind("*magit-transient*", TRUE)) == NULL)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);

	for (;;) {
		magit_transient_render(bp, m);
		wp->w_dotp = bfirstlp(bp);
		wp->w_doto = 0;
		wp->w_rflag |= WFFULL;
		update(CMODE);
		k = getkey(FALSE);
		if (k == CCHR('G') || k == CCHR('['))	/* C-g / ESC: cancel */
			break;
		handled = 0;
		for (i = 0; i < m->n_infixes; i++) {
			if (m->infixes[i].key != k)
				continue;
			handled = 1;
			if (m->infixes[i].kind == MAGIT_INFIX_FLAG) {
				m->infixes[i].on = !m->infixes[i].on;
			} else {
				/* Empty input keeps the current value (shown in
				 * the popup); a non-empty entry replaces it. */
				val[0] = '\0';
				if (eread("%s (%s): ", val, sizeof(val),
				    EFNEW | EFCR, (char *)m->infixes[i].arg,
				    m->infixes[i].value) != NULL &&
				    val[0] != '\0')
					(void)strlcpy(m->infixes[i].value, val,
					    sizeof(m->infixes[i].value));
				if (m == &log_menu)
					magit_log_limit = (int)strtol(
					    log_infixes[0].value, NULL, 10);
			}
			break;
		}
		if (handled)
			continue;
		fn = doscan(m->map, k, NULL);
		break;
	}

	curwp = wp;			/* delete the popup window... */
	curbp = wp->w_bufp;
	(void)delwind(f, n);
	curwp = stwp;			/* ...and restore the status window */
	curbp = stbp;

	if (fn == NULL)
		return (ABORT);
	return ((*fn)(f, n));
}

static int magit_menu_pull(int f, int n)   { return (magit_transient(&pull_menu, f, n)); }
static int magit_menu_push(int f, int n)   { return (magit_transient(&push_menu, f, n)); }
static int magit_menu_reset(int f, int n)  { return (magit_transient(&reset_menu, f, n)); }
static int magit_menu_worktree(int f, int n){ return (magit_transient(&worktree_menu, f, n)); }
static int magit_menu_bisect(int f, int n) { return (magit_transient(&bisect_menu, f, n)); }
static int magit_menu_branch(int f, int n) { return (magit_transient(&branch_menu, f, n)); }
static int magit_menu_commit(int f, int n) { return (magit_transient(&commit_menu, f, n)); }
static int magit_menu_log(int f, int n)    { return (magit_transient(&log_menu, f, n)); }
static int magit_menu_rebase(int f, int n) { return (magit_transient(&rebase_menu, f, n)); }
static int magit_menu_tag(int f, int n)    { return (magit_transient(&tag_menu, f, n)); }
static int magit_menu_stash(int f, int n)  { return (magit_transient(&stash_menu, f, n)); }
static int magit_reflog_reset_menu(int f, int n) { return (magit_transient(&reflog_reset_menu, f, n)); }

/* e: resolve the conflict at point -- only meaningful on a Conflicts line. */
static int
magit_menu_conflict(int f, int n)
{
	char	*path = NULL;
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_CONFLICT) {
		ewprintf("Point is not on a conflicted file");
		return (FALSE);
	}
	return (magit_transient(&conflict_menu, f, n));
}

/* Shared: resolve the conflicted file at point by keeping `take_theirs`. */
static int
magit_resolve_at_point(int take_theirs, int f, int n)
{
	char	*path = NULL;
	char	 cwd[PATH_MAX];
	int	 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_CONFLICT || path == NULL || path[0] == '\0') {
		ewprintf("Point is not on a conflicted file");
		return (FALSE);
	}
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if (mg_magit_resolve_conflict(cwd, path, take_theirs) != 1) {
		ewprintf("Resolve failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

static int
magit_conflict_ours(int f, int n)
{
	return (magit_resolve_at_point(0, f, n));
}

static int
magit_conflict_theirs(int f, int n)
{
	return (magit_resolve_at_point(1, f, n));
}

/* Entries MUST stay in ascending key order -- doscan() relies on it. */
static struct KEYMAPE (32) magitmap = {
	32,
	32,
	rescan,
	{
		{ CCHR('I'), CCHR('I'), magit_tab, NULL },	/* TAB: expand/collapse */
		{ CCHR('M'), CCHR('M'), magit_ret, NULL },	/* RET: visit file */
		{ CCHR('['), CCHR('['), magit_esc,		/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ '$', '$', magit_dollar, NULL },		/* $: process log */
		{ '?', '?', magit_qmark, NULL },		/* ?: key help */
		{ 'A', 'A', magit_A, NULL },			/* A: cherry-pick */
		{ 'B', 'B', magit_B, NULL },			/* B: blame file at point */
		{ 'E', 'E', magit_E, NULL },			/* E: ediff conflict regions */
		{ 'F', 'F', magit_F, NULL }, /* F: pull menu */
		{ 'P', 'P', magit_P, NULL }, /* P: push menu */
		{ 'S', 'S', magit_S, NULL },			/* S: stage all */
		{ 'U', 'U', magit_U, NULL },			/* U: unstage all */
		{ 'V', 'V', magit_V, NULL },			/* V: revert */
		{ 'W', 'W', magit_W, NULL }, /* W: worktree menu */
		{ 'X', 'X', magit_X, NULL }, /* X: reset menu */
		{ 'Z', 'Z', magit_Z, NULL }, /* Z: bisect menu */
		{ 'a', 'a', magit_a, NULL },			/* a: apply stash */
		{ 'b', 'b', magit_b, NULL }, /* b: branch menu */
		{ 'c', 'c', magit_c, NULL }, /* c: commit menu */
		{ 'e', 'e', magit_e, NULL },			/* e: resolve conflict */
		{ 'f', 'f', magit_f, NULL },			/* f: fetch */
		{ 'g', 'g', magit_g, NULL },
		{ 'i', 'i', magit_i, NULL },			/* i: gitignore */
		{ 'k', 'k', magit_k, NULL },
		{ 'l', 'l', magit_l, NULL }, /* l: log menu */
		{ 'm', 'm', magit_m, NULL },			/* m: merge */
		{ 'q', 'q', magit_q, NULL },
		{ 'r', 'r', magit_r, NULL }, /* r: rebase menu */
		{ 's', 's', magit_s, NULL },
		{ 't', 't', magit_t, NULL }, /* t: tag menu */
		{ 'u', 'u', magit_u, NULL },
		{ 'z', 'z', magit_z, NULL } /* z: stash menu */
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
 * Each transient descriptor's action keys must (a) be ascending and (b) every
 * one resolve in its submap -- otherwise a popup would advertise a key that
 * dispatches to nothing. A drift between the label table and the keymap is a
 * programming error; fail fast.
 */
static void
magit_assert_menus_consistent(void)
{
	static struct magit_menu *const all[] = {
		&pull_menu, &push_menu, &reset_menu, &worktree_menu, &bisect_menu,
		&branch_menu, &commit_menu, &log_menu, &rebase_menu, &tag_menu,
		&stash_menu, &conflict_menu, &reflog_reset_menu
	};
	size_t	m;
	int	i;

	for (m = 0; m < sizeof(all) / sizeof(all[0]); m++) {
		struct magit_menu *mn = all[m];

		for (i = 0; i < mn->n_items; i++) {
			if (i > 0 && mn->items[i - 1].key >= mn->items[i].key)
				panic("magit transient: items out of order");
			if (doscan(mn->map, mn->items[i].key, NULL) == NULL)
				panic("magit transient: item key not in submap");
		}
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
	const char	*cwd = bp->b_cwd;

	/*
	 * Operate on the *buffer's* directory, not the process cwd. magit_status
	 * sets b_cwd from the buffer you invoked it from (a dired dir, a visited
	 * file's dir, ...), so magit-status works on that repo even when neomg was
	 * launched elsewhere. libgit2 discovers the repo root from any path inside.
	 */
	if (cwd[0] == '\0')
		return (FALSE);

	bp->b_flag |= BFIGNDIRTY;	/* don't prompt when re-clearing */
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	magit_status_bp = bp;		/* gate display.c's color hook on this */
	magit_cell_color_reset();	/* freed lines may be reused; drop memo */
	magit_meta_count = 0;
	magit_skip = 0;
	{
		const char	*exp[MAGIT_MAX_EXPANDED];
		int		 i;

		for (i = 0; i < magit_expanded_count; i++)
			exp[i] = magit_expanded[i];
		/*
		 * Replay the monitor thread's warm snapshot (the expensive
		 * workdir scan + revwalk + ref enumeration ran off the UI
		 * thread); falls back to a synchronous build on cold start.
		 */
		(void)mg_magit_status_snapshot(cwd, exp, magit_expanded_count,
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
	char		 repo[NFILEN];

	if (!initialized) {
		magit_assert_keymap_sorted();
		magit_assert_menus_consistent();
		maps_add((KEYMAP *)&magitmap, "magit-status-mode");
		/* Register the log/commit-view modes here too: RET on a stash opens
		 * the commit-view buffer without ever going through `l`. */
		maps_add((KEYMAP *)&maglogmap, "magit-log-mode");
		maps_add((KEYMAP *)&magit_reflogmap, "magit-reflog-mode");
		maps_add((KEYMAP *)&magcommitmap, "magit-commit-view-mode");
		maps_add((KEYMAP *)&magprocessmap, "magit-process-mode");
		maps_add((KEYMAP *)&magit_todomap, "magit-rebase-todo-mode");
		maps_add((KEYMAP *)&magediffmap, "magit-ediff-mode");
		mg_magit_set_cred_prompt(magit_cred_prompt); /* HTTPS user/pass auth */
		initialized = 1;
	}

	/*
	 * The repo to show is the directory of the buffer we're invoked from (a
	 * dired dir, a visited file's dir, or the cwd) -- captured now, while curbp
	 * is still that buffer (before popbuf switches to *magit-status*). Persist
	 * it on *magit-status* so refresh (g) and the post-command rebuilds reuse
	 * the same repo. This is why magit-status works after diring into a repo
	 * from elsewhere, instead of always using the launch cwd.
	 */
	if (getbufcwd(repo, sizeof(repo)) != TRUE)
		(void)strlcpy(repo, "/", sizeof(repo));

	if ((bp = bfind("*magit-status*", TRUE)) == NULL)
		return (FALSE);
	(void)strlcpy(bp->b_cwd, repo, sizeof(bp->b_cwd));
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

/* Discards a ready async result whose target is gone or superseded. */
static void
magit_discard_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
}

/*
 * Apply any async per-file builds (blame / log-file) the worker has finished.
 * Runs only at safe points (the idle handler below), never while a command
 * holds line pointers. A result is applied only if its generation still
 * matches the latest request for its kind AND the target buffer still exists;
 * otherwise it is drained and dropped (the user moved on / closed it).
 */
static void
magit_async_apply(void)
{
	int		 kind;
	char		 path[PATH_MAX];
	unsigned	 gen;
	struct buffer	*bp;
	struct mgwin	*wp;

	while (mg_magit_async_peek(&kind, path, sizeof(path), &gen) >= 0) {
		bp = NULL;
		if (kind == MG_ASYNC_BLAME && gen == magit_blame_gen)
			bp = bfind("*magit-blame*", FALSE);
		else if (kind == MG_ASYNC_LOG_FILE && gen == magit_logfile_gen)
			bp = bfind("*magit-log*", FALSE);

		if (bp == NULL) {			/* superseded or buffer gone */
			(void)mg_magit_async_take(magit_discard_emit, NULL);
			continue;
		}

		bp->b_flag |= BFIGNDIRTY;
		if (bclear(bp) != TRUE) {
			(void)mg_magit_async_take(magit_discard_emit, NULL);
			continue;
		}
		bp->b_flag |= BFREADONLY;
		if (kind == MG_ASYNC_LOG_FILE) {
			magit_log_count = 0;	/* magit_log_emit rebuilds the oid map */
			magit_log_oid_bp = bp;	/* record the owning buffer */
			(void)mg_magit_async_take(magit_log_emit, bp);
		} else {
			(void)mg_magit_async_take(magit_plain_emit, bp);
		}

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
		sgarbf = TRUE;
	}
}

/*
 * Idle handler, driven by the background git workers via the shared wake pipe.
 * Runs only at safe points -- the top-level input loop (main.c) and the
 * top-level command read in getkey() -- never while a command holds line
 * pointers into a buffer. Applies any finished async per-file builds, then (if
 * the status monitor flagged a change) repaints the modeline and rebuilds
 * *magit-status* in place via the cheap snapshot replay. Like the manual `g`
 * refresh, point returns to the top of the buffer.
 */
void
magit_idle_refresh(void)
{
	struct buffer	*bp;

	magit_async_apply();			/* blame / log-file results */

	if (!mg_magit_take_dirty())
		return;
	sgarbf = TRUE;				/* modeline reflects new state */
	if ((bp = bfind("*magit-status*", FALSE)) != NULL)
		(void)magit_build(bp);
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

/*
 * magit-process: show the process log in a read-only *magit-process* buffer.
 * Bound to `$` in magit-status-mode (mirrors magit's `$` binding).
 */
static int
magit_process(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if ((bp = bfind("*magit-process*", TRUE)) == NULL)
		return (FALSE);
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	(void)mg_magit_process_log(magit_plain_emit, bp);
	bp->b_flag |= BFREADONLY;
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	bp->b_modes[1] = name_mode("magit-process-mode");
	bp->b_nmodes = 1;
	(void)gotobob(f, n);		/* top: newest entry first */
	return (TRUE);
}

/* emit callback for *magit-commit*: record kind/path per line for syntax coloring. */
static void
magit_diff_emit(void *ctx, const char *line, int kind, const char *path, int hunk)
{
	(void)hunk;
	if (magit_commit_meta_count < MAGIT_MAX_LINES) {
		magit_commit_meta[magit_commit_meta_count].kind = kind;
		if (path != NULL)
			(void)strlcpy(magit_commit_meta[magit_commit_meta_count].path,
			    path, sizeof(magit_commit_meta[magit_commit_meta_count].path));
		else
			magit_commit_meta[magit_commit_meta_count].path[0] = '\0';
		magit_commit_meta_count++;
	}
	(void)addlinef((struct buffer *)ctx, "%s", (char *)line);
}

/* (Re)build the *magit-log* buffer + its per-line oid map. */
static int
magit_log_build(struct buffer *bp)
{
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	/* Persist the repo on the log buffer so refresh (g) from within it
	 * targets the same repo, not the process cwd. */
	(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd));
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	magit_log_count = 0;
	if (magit_log_graph || magit_log_range[0] != '\0' ||
	    magit_log_pickaxe != '\0') {
		(void)mg_magit_log_query_buffer(cwd, magit_log_graph,
		    magit_log_range[0] ? magit_log_range : NULL, NULL,
		    magit_log_pickaxe, magit_log_pickaxe_term,
		    magit_log_limit, magit_log_emit, bp);
	} else if (magit_log_file_path[0] != '\0') {
		/*
		 * Per-file log is slow (150-360ms); run it on the worker thread
		 * with a placeholder, applied on the wake. Whole-repo log is
		 * ~3ms after the lazy-walk fix, so it stays synchronous.
		 */
		magit_logfile_gen = mg_magit_async_request(MG_ASYNC_LOG_FILE,
		    cwd, magit_log_file_path, magit_log_limit);
		if (magit_logfile_gen != 0)
			(void)addlinef(bp, "Loading log for %s...",
			    magit_log_file_path);
		else
			(void)mg_magit_log_file_buffer(cwd, magit_log_file_path,
			    magit_log_limit, magit_log_emit, bp);
	} else
		(void)mg_magit_log_buffer(cwd, magit_log_limit, magit_log_emit,
		    bp);

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
	magit_log_oid_bp = bp;
	return (TRUE);
}

/* True for the merged-pane lines in the active region -> rendered in standout.
 * Called from display.c's redisplay loop (ENABLE_NATIVE_MAGIT). */
int
magit_line_highlighted(struct buffer *bp, struct line *lp)
{
	int	i;

	if (bp != magit_ediff_merged_bp)
		return (0);
	for (i = 0; i < magit_ediff_hl_n; i++)
		if (magit_ediff_hl[i] == lp)
			return (1);
	return (0);
}

/* True when char `col` of `lp` is a refined (word-level) difference in a side
 * pane -> that one cell renders in standout. Called per cell from display.c. */
int
magit_cell_highlighted(struct buffer *bp, struct line *lp, int col)
{
	int	i;

	(void)bp;
	for (i = 0; i < magit_ediff_ref_n; i++)
		if (magit_ediff_ref[i].lp == lp && col >= magit_ediff_ref[i].start &&
		    col < magit_ediff_ref[i].end)
			return (1);
	return (0);
}

/* The per-line memo cache for magit_cell_color; module-scope so a buffer
 * rebuild can invalidate it (a freed line address can be reused). */
static struct line	*magit_color_cached_lp;
static uint8_t		 magit_color_kindcol[1024];
/* Cached per-line resolved state — valid only when magit_color_cached_lp != NULL. */
static const char	*magit_color_cached_path;	/* points into static meta arrays */
static int		 magit_color_cached_is_diff;

/* Invalidate the per-line color memo. Called after bclear() in each colored
 * buffer's build path, before any new lines are allocated. */
void
magit_cell_color_reset(void)
{
	magit_color_cached_lp = NULL;
	magit_color_cached_path = NULL;
	magit_color_cached_is_diff = 0;
}

/* True for the syntax-colored buffers (status / commit / ediff panes). Lets
 * display.c's per-cell hook gate on pointer compares instead of bfind(). */
int
magit_is_color_buffer(struct buffer *bp)
{
	return (bp != NULL && (bp == magit_status_bp || bp == magit_commit_bp ||
	    bp == magit_ediff_merged_bp || bp == magit_ediff_ours_bp ||
	    bp == magit_ediff_theirs_bp));
}

/*
 * Returns the syntax token kind (0-7) for character index `ci` of a magit
 * diff line in `bp`, or 0 if not a diff content line. The caller (display.c)
 * has already gated on magit_is_color_buffer(), so `bp` is the status, commit,
 * or one of the three ediff panes. Uses the Zig tokenizer via the C ABI;
 * memoizes all per-line state (list walk, path resolution, kindcol) so the
 * non-first-cell path does no list traversal — O(1) per cell after the first.
 */
int
magit_cell_color(struct buffer *bp, struct line *lp, int ci)
{
	static NeomgSpan	 spans[256];
	static size_t		 nspans;
	const char		*path;
	const char		*text;
	uint8_t			 lang;
	int			 len, off, idx, is_diff;
	size_t			 k;

	if (lp != magit_color_cached_lp) {
		/*
		 * First cell of a new line: resolve path/is_diff and fill kindcol.
		 * All per-line work (including any list walk) lives inside this block;
		 * subsequent cells return directly from kindcol without entering here.
		 */
		path = NULL;
		is_diff = 0;

		if (bp == magit_status_bp) {
			/* Walk the line list once per line to find the meta index. */
			struct line *lp2 = bfirstlp(bp);
			for (idx = 0; idx < magit_meta_count && lp2 != bp->b_headp;
			    idx++) {
				if (lp2 == lp) {
					if (magit_meta[idx].kind == MG_LINE_DIFF &&
					    magit_meta[idx].path[0] != '\0') {
						path = magit_meta[idx].path;
						is_diff = 1;
					}
					break;
				}
				lp2 = lforw(lp2);
			}
		} else if (bp == magit_commit_bp) {
			struct line *lp2 = bfirstlp(bp);
			for (idx = 0; idx < magit_commit_meta_count &&
			    lp2 != bp->b_headp; idx++) {
				if (lp2 == lp) {
					if (magit_commit_meta[idx].kind == MG_LINE_DIFF &&
					    magit_commit_meta[idx].path[0] != '\0') {
						path = magit_commit_meta[idx].path;
						is_diff = 1;
					}
					break;
				}
				lp2 = lforw(lp2);
			}
		} else if ((bp == magit_ediff_merged_bp ||
		    bp == magit_ediff_ours_bp ||
		    bp == magit_ediff_theirs_bp) &&
		    magit_ediff_path[0] != '\0') {
			/* ediff panes: all lines are raw source (no +/-/space prefix).
			 * For the merged pane, skip conflict-marker lines (<<<, |||,
			 * ===, >>>) so they don't get spurious syntax color. */
			text = ltext(lp);
			len = llength(lp);
			/* ltext() is not NUL-terminated and is exactly llength() bytes;
			 * require >= 7 bytes before reading the 7-char marker prefix. */
			if (bp == magit_ediff_merged_bp && len >= 7 &&
			    (strncmp(text, "<<<<<<<", 7) == 0 ||
			     strncmp(text, "|||||||", 7) == 0 ||
			     strncmp(text, "=======", 7) == 0 ||
			     strncmp(text, ">>>>>>>", 7) == 0)) {
				/*
				 * Conflict-marker line: cache lp with is_diff=0 so
				 * later cells of this line return 0 immediately (no
				 * re-walk), then return 0 for this cell.
				 */
				magit_color_cached_lp = lp;
				magit_color_cached_path = NULL;
				magit_color_cached_is_diff = 0;
				return (0);
			}
			path = magit_ediff_path;
			is_diff = 1;
		}

		/* Cache resolved metadata before potentially returning early. */
		magit_color_cached_lp = lp;
		magit_color_cached_path = path;
		magit_color_cached_is_diff = is_diff;

		if (!is_diff) {
			memset(magit_color_kindcol, 0, sizeof(magit_color_kindcol));
			return (0);
		}

		/* Tokenize and fill kindcol for this line. */
		text = ltext(lp);
		len = llength(lp);
		/* diff buffers: skip +/-/space origin char; ediff panes: no prefix */
		off = (bp != magit_ediff_merged_bp && bp != magit_ediff_ours_bp &&
		    bp != magit_ediff_theirs_bp &&
		    len > 0 && (text[0] == '+' || text[0] == '-' ||
		    text[0] == ' ')) ? 1 : 0;
		lang = neomg_lang_from_path(path);
		nspans = neomg_highlight_line(lang, text + off, (size_t)(len - off),
		    spans, 256);
		memset(magit_color_kindcol, 0, sizeof(magit_color_kindcol));
		for (k = 0; k < nspans; k++) {
			int j;
			for (j = 0; j < spans[k].len &&
			    (spans[k].start + off + j) < 1024; j++)
				magit_color_kindcol[spans[k].start + off + j] =
				    spans[k].kind;
		}
		/* magit_color_cached_lp already set above */
	}
	/* Non-first-cell path: no list walk, return from cached kindcol in O(1). */
	if (!magit_color_cached_is_diff)
		return (0);
	return (ci >= 0 && ci < 1024) ? magit_color_kindcol[ci] : 0;
}

/* Point the window showing `bp` at `dot` and force a redraw. */
static void
magit_window_to(struct buffer *bp, struct line *dot)
{
	struct mgwin	*wp;

	for (wp = wheadp; wp != NULL; wp = wp->w_wndp)
		if (wp->w_bufp == bp) {
			wp->w_dotp = dot;
			wp->w_doto = 0;
			wp->w_markp = NULL;
			wp->w_marko = 0;
			wp->w_rflag |= WFFULL | WFFRAME;
		}
}

/* The `k`-th line (0-based) of `bp`, clamped to the last real line. */
static struct line *
magit_line_at(struct buffer *bp, int k)
{
	struct line	*lp = bfirstlp(bp);
	int		 i;

	for (i = 0; i < k && lforw(lp) != bp->b_headp; i++)
		lp = lforw(lp);
	return (lp);
}

/* refine target for the attach emit below (the active region's side lines). */
static struct line	**magit_ediff_reftgt;
static int		  magit_ediff_reftgt_n;

/*
 * emit callback that attaches word-refinement ranges to the active region's
 * side-pane lines: the bridge emits region `side`'s lines with wdiff markers in
 * the same order as build_all recorded them, so row N -> magit_ediff_reftgt[N].
 */
static void
magit_ediff_refine_emit(void *ctx, const char *line, int kind, const char *path,
    int hunk)
{
	struct line	*lp;
	int		 op = 0, marked = 0, ci = 0, row = magit_ediff_refrow++;
	const char	*s = line;

	(void)ctx;
	(void)kind;
	(void)path;
	(void)hunk;
	if (row >= magit_ediff_reftgt_n)
		return;
	lp = magit_ediff_reftgt[row];
	while (*s != '\0') {
		if ((s[0] == '[' && s[1] == '-') || (s[0] == '{' && s[1] == '+')) {
			marked = 1;
			op = ci;
			s += 2;
			continue;
		}
		if ((s[0] == '-' && s[1] == ']') || (s[0] == '+' && s[1] == '}')) {
			if (marked && magit_ediff_ref_n < MAGIT_EDIFF_REF_MAX) {
				magit_ediff_ref[magit_ediff_ref_n].lp = lp;
				magit_ediff_ref[magit_ediff_ref_n].start = op;
				magit_ediff_ref[magit_ediff_ref_n].end = ci;
				magit_ediff_ref_n++;
			}
			marked = 0;
			s += 2;
			continue;
		}
		if (((unsigned char)*s & 0xC0) != 0x80)
			ci++;	/* codepoint index (UTF-8-correct) */
		s++;
	}
}

/*
 * (Re)build all three panes from the working file in one pass: merged is the
 * raw file (markers); ours/theirs are the FULL versions (every region resolved
 * to that side); per merged line, magit_ediff_aln_{ours,theirs} maps to the
 * corresponding side line (for synchronized scrolling). Records the active
 * region's merged lines (highlight) + side lines (refinement). Returns the
 * region count.
 */
static int
magit_ediff_build_all(struct buffer *m, struct buffer *o, struct buffer *t)
{
	FILE		*fp;
	char		 cwd[PATH_MAX], full[PATH_MAX], buf[4096];
	struct line	*first = NULL, *ml, *sl;
	int		 scan = -1, regions = 0, mi = 0, oi = 0, ti = 0, active = 0;
	enum { COMMON, OURS, BASE, THEIRS } st = COMMON;

	m->b_flag |= BFIGNDIRTY; (void)bclear(m); m->b_flag |= BFREADONLY;
	o->b_flag |= BFIGNDIRTY; (void)bclear(o); o->b_flag |= BFREADONLY;
	t->b_flag |= BFIGNDIRTY; (void)bclear(t); t->b_flag |= BFREADONLY;
	magit_ediff_hl_n = magit_ediff_oreg_n = magit_ediff_treg_n = 0;
	magit_ediff_merged_bp = m;
	magit_ediff_ours_bp = o;
	magit_ediff_theirs_bp = t;
	magit_cell_color_reset();	/* freed lines may be reused; drop memo */

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (0);
	(void)snprintf(full, sizeof(full), "%s/%s", cwd, magit_ediff_path);
	if ((fp = fopen(full, "r")) != NULL) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			size_t len = strlen(buf);
			int is_s, is_b, is_e, is_g;
			if (len > 0 && buf[len - 1] == '\n')
				buf[len - 1] = '\0';
			is_s = strncmp(buf, "<<<<<<<", 7) == 0;
			is_b = strncmp(buf, "|||||||", 7) == 0;
			is_e = strncmp(buf, "=======", 7) == 0;
			is_g = strncmp(buf, ">>>>>>>", 7) == 0;

			if (mi < MAGIT_MAX_LINES) {
				magit_ediff_aln_ours[mi] = oi;
				magit_ediff_aln_theirs[mi] = ti;
			}
			if (is_s) {
				scan++;
				if (scan + 1 > regions)
					regions = scan + 1;
				st = OURS;
				if (scan == magit_ediff_region)
					active = 1;
			} else if (is_b) {
				st = BASE;
			} else if (is_e) {
				st = THEIRS;
			}

			(void)addlinef(m, "%s", buf);	/* merged gets every line */
			ml = lback(m->b_headp);
			mi++;
			if (active && magit_ediff_hl_n < MAGIT_EDIFF_HL_MAX) {
				if (first == NULL)
					first = ml;
				magit_ediff_hl[magit_ediff_hl_n++] = ml;
			}

			if (!is_s && !is_b && !is_e && !is_g) {	/* content line */
				if (st == COMMON) {
					(void)addlinef(o, "%s", buf); oi++;
					(void)addlinef(t, "%s", buf); ti++;
				} else if (st == OURS) {
					(void)addlinef(o, "%s", buf);
					sl = lback(o->b_headp); oi++;
					if (active &&
					    magit_ediff_oreg_n < MAGIT_EDIFF_HL_MAX)
						magit_ediff_oreg[magit_ediff_oreg_n++] = sl;
				} else if (st == THEIRS) {
					(void)addlinef(t, "%s", buf);
					sl = lback(t->b_headp); ti++;
					if (active &&
					    magit_ediff_treg_n < MAGIT_EDIFF_HL_MAX)
						magit_ediff_treg[magit_ediff_treg_n++] = sl;
				}
				/* BASE: merged only */
			}
			if (is_g) {
				st = COMMON;
				active = 0;
			}
		}
		(void)fclose(fp);
	}
	magit_ediff_merged_n = mi;
	m->b_dotp = (first != NULL) ? first : bfirstlp(m);
	m->b_doto = 0;
	o->b_dotp = bfirstlp(o); o->b_doto = 0;
	t->b_dotp = bfirstlp(t); t->b_doto = 0;
	return (regions);
}

/* Align the ours/theirs panes' top line to the merged pane's viewport. */
static void
magit_ediff_sync_scroll(void)
{
	struct buffer	*m, *o, *t;
	struct mgwin	*wp, *mw = NULL, *ow = NULL, *tw = NULL;
	struct line	*lp;
	int		 top = 0;

	if ((m = bfind("*ediff-merged*", FALSE)) == NULL ||
	    (o = bfind("*ediff-ours*", FALSE)) == NULL ||
	    (t = bfind("*ediff-theirs*", FALSE)) == NULL)
		return;
	for (wp = wheadp; wp != NULL; wp = wp->w_wndp) {
		if (wp->w_bufp == m) mw = wp;
		else if (wp->w_bufp == o) ow = wp;
		else if (wp->w_bufp == t) tw = wp;
	}
	if (mw == NULL)
		return;
	/*
	 * Align by the merged pane's dot line, and force each pane's TOP line
	 * (w_linep) to the corresponding line so all three line up exactly --
	 * w_linep is computed lazily at redisplay, so set it directly rather than
	 * relying on reframe. forwline/forwpage move the merged dot; re-syncing
	 * scrolls every pane together.
	 */
	for (lp = bfirstlp(m); lp != mw->w_dotp && lp != m->b_headp; lp = lforw(lp))
		top++;
	if (top >= magit_ediff_merged_n)
		top = magit_ediff_merged_n > 0 ? magit_ediff_merged_n - 1 : 0;
	if (top >= MAGIT_MAX_LINES)
		top = MAGIT_MAX_LINES - 1;
	/* Clear WFFRAME (reframe centers the dot and would override w_linep). */
	mw->w_linep = mw->w_dotp;	/* merged: dot line to the top */
	mw->w_rflag = (mw->w_rflag & ~WFFRAME) | WFFULL;
	if (ow != NULL) {
		ow->w_linep = ow->w_dotp = magit_line_at(o, magit_ediff_aln_ours[top]);
		ow->w_doto = 0;
		ow->w_rflag = (ow->w_rflag & ~WFFRAME) | WFFULL;
	}
	if (tw != NULL) {
		tw->w_linep = tw->w_dotp = magit_line_at(t, magit_ediff_aln_theirs[top]);
		tw->w_doto = 0;
		tw->w_rflag = (tw->w_rflag & ~WFFRAME) | WFFULL;
	}
}

/* Rebuild all three panes for the current region, attach word-refinement to the
 * active region's side lines, and align the panes. Returns the region count. */
static int
magit_ediff_sync(void)
{
	struct buffer	*merged, *ours, *theirs;
	char		 cwd[PATH_MAX];
	int		 regions;

	if ((merged = bfind("*ediff-merged*", FALSE)) == NULL ||
	    (ours = bfind("*ediff-ours*", FALSE)) == NULL ||
	    (theirs = bfind("*ediff-theirs*", FALSE)) == NULL)
		return (0);
	regions = magit_ediff_build_all(merged, ours, theirs);

	/* Attach word-level refinement to the active region's side lines. The
	 * bridge re-emits region `side`'s lines (with wdiff markers) in the same
	 * order build_all recorded them. */
	magit_ediff_ref_n = 0;
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (regions);
	magit_ediff_refrow = 0;
	magit_ediff_reftgt = magit_ediff_oreg;
	magit_ediff_reftgt_n = magit_ediff_oreg_n;
	(void)mg_magit_conflict_hunk_side(cwd, magit_ediff_path,
	    magit_ediff_region, 0, magit_ediff_refine_emit, NULL);
	magit_ediff_refrow = 0;
	magit_ediff_reftgt = magit_ediff_treg;
	magit_ediff_reftgt_n = magit_ediff_treg_n;
	(void)mg_magit_conflict_hunk_side(cwd, magit_ediff_path,
	    magit_ediff_region, 1, magit_ediff_refine_emit, NULL);

	magit_window_to(merged, merged->b_dotp);	/* show the region */
	magit_ediff_sync_scroll();
	return (regions);
}

/* Tear down the 3 panes and return to a single *magit-status* window. */
static int
magit_ediff_quit(int f, int n)
{
	struct buffer	*st;

	magit_ediff_active = 0;
	magit_ediff_merged_bp = NULL;
	magit_ediff_ours_bp = NULL;
	magit_ediff_theirs_bp = NULL;
	magit_cell_color_reset();
	magit_ediff_hl_n = 0;
	magit_ediff_ref_n = 0;
	(void)onlywind(f, n);
	if ((st = bfind("*magit-status*", FALSE)) != NULL) {
		(void)showbuffer(st, curwp, WFFULL | WFFRAME);
		curbp = st;
		(void)magit_refresh(f, n);
	}
	return (TRUE);
}

/*
 * E (on a Conflicts line): open the 3-pane *magit-ediff* -- merged (top,
 * active region in standout), theirs (middle), ours (bottom). Only the merged
 * pane is interactive (n/p step regions, a/b/RET resolve, q close).
 */
static int
magit_ediff(int f, int n)
{
	struct buffer	*merged, *ours, *theirs;
	struct mgwin	*top, *mid, *bot;
	char		*path = NULL;
	int		 kind, hunk;

	kind = magit_at_point(&path, &hunk);
	if (kind != MG_LINE_CONFLICT || path == NULL || path[0] == '\0') {
		ewprintf("Point is not on a conflicted file");
		return (FALSE);
	}
	(void)strlcpy(magit_ediff_path, path, sizeof(magit_ediff_path));
	magit_ediff_region = 0;

	if ((merged = bfind("*ediff-merged*", TRUE)) == NULL ||
	    (theirs = bfind("*ediff-theirs*", TRUE)) == NULL ||
	    (ours = bfind("*ediff-ours*", TRUE)) == NULL)
		return (FALSE);
	(void)magit_ediff_sync();

	/*
	 * Collapse to one window, then split twice WHILE it still shows the
	 * status buffer (consistent w_linep/w_dotp -- splitwind walks them, and
	 * showbuffer leaves w_linep stale). Assign the pane buffers afterward.
	 */
	(void)onlywind(f, n);
	curwp->w_dotp = bfirstlp(curbp);	/* dot at top -> predictable split */
	curwp->w_doto = 0;
	if (splitwind(f, n) != TRUE)
		return (FALSE);
	if (splitwind(f, n) != TRUE)
		return (FALSE);
	top = wheadp;			/* top / mid / bot, in order */
	mid = top->w_wndp;
	bot = mid->w_wndp;

	(void)showbuffer(merged, top, WFFULL | WFFRAME);
	(void)showbuffer(theirs, mid, WFFULL | WFFRAME);
	(void)showbuffer(ours, bot, WFFULL | WFFRAME);
	top->w_dotp = merged->b_dotp;
	top->w_doto = 0;
	merged->b_modes[1] = name_mode("magit-ediff-mode");
	merged->b_nmodes = 1;

	curwp = top;		/* focus the interactive merged pane */
	curbp = merged;
	magit_ediff_active = 1;	/* enable the redisplay highlight lookups */
	magit_ediff_sync_scroll();	/* align the side panes now the windows exist */
	return (TRUE);
}

/*
 * Resolve the current region (0 ours / 1 theirs / 2 both). When the last region
 * is resolved, stage the now-clean file and close the session.
 */
static int
magit_ediff_resolve(int side, int f, int n)
{
	char	cwd[PATH_MAX];
	int	regions;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if (mg_magit_resolve_conflict_hunk(cwd, magit_ediff_path,
	    magit_ediff_region, side) != 1) {
		ewprintf("Resolve failed");
		return (FALSE);
	}
	regions = magit_ediff_sync();
	if (regions == 0) {
		(void)mg_magit_stage(cwd, magit_ediff_path);
		ewprintf("%s resolved", magit_ediff_path);
		return (magit_ediff_quit(f, n));
	}
	if (magit_ediff_region >= regions)
		magit_ediff_region = regions - 1;
	regions = magit_ediff_sync(); /* re-render at the clamped region */
	ewprintf("%d conflict region%s left", regions, regions == 1 ? "" : "s");
	return (TRUE);
}

static int
magit_ediff_ours(int f, int n)
{
	return (magit_ediff_resolve(0, f, n));
}

static int
magit_ediff_theirs(int f, int n)
{
	return (magit_ediff_resolve(1, f, n));
}

static int
magit_ediff_both(int f, int n)
{
	return (magit_ediff_resolve(2, f, n));
}

/* n / p: step to the next / previous conflict region (all panes follow). */
static int
magit_ediff_step(int delta, int f, int n)
{
	int	regions;

	magit_ediff_region += delta;
	if (magit_ediff_region < 0)
		magit_ediff_region = 0;
	regions = magit_ediff_sync();		/* rebuild at the new region */
	if (regions <= 0)
		return (FALSE);
	if (magit_ediff_region >= regions) {	/* overshot -> clamp + rebuild */
		magit_ediff_region = regions - 1;
		regions = magit_ediff_sync();
	}
	ewprintf("Region %d/%d", magit_ediff_region + 1, regions);
	return (TRUE);
}

static int
magit_ediff_next(int f, int n)
{
	return (magit_ediff_step(1, f, n));
}

static int
magit_ediff_prev(int f, int n)
{
	return (magit_ediff_step(-1, f, n));
}

/*
 * Free scrolling: move point/viewport in the merged pane, then realign the
 * ours/theirs panes so all three scroll together (C-n/C-p line, C-v/M-v page).
 */
static int
magit_ediff_scroll(int (*mv)(int, int), int f, int n)
{
	int	r = (*mv)(f, n);

	magit_ediff_sync_scroll();
	return (r);
}

static int magit_ediff_sc_down(int f, int n) { return (magit_ediff_scroll(forwline, f, n)); }
static int magit_ediff_sc_up(int f, int n)   { return (magit_ediff_scroll(backline, f, n)); }
static int magit_ediff_sc_pgdn(int f, int n) { return (magit_ediff_scroll(forwpage, f, n)); }
static int magit_ediff_sc_pgup(int f, int n) { return (magit_ediff_scroll(backpage, f, n)); }

/* Build + pop the *magit-log* buffer (honoring magit_log_file_path). */
static int
magit_log_open(int f, int n)
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

/* Clear all CLI-log-query state so l l / l f revert to the plain libgit2 log. */
static void
magit_log_query_reset(void)
{
	magit_log_graph = 0;
	magit_log_range[0] = '\0';
	magit_log_pickaxe = '\0';
	magit_log_pickaxe_term[0] = '\0';
}

/* l g: graph log of HEAD (CLI). */
static int
magit_log_graph_cmd(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	magit_log_graph = 1;
	return (magit_log_open(f, n));
}

/* l r: log a commit range (prompted, e.g. main..HEAD). */
static int
magit_log_range_cmd(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	if (eread("Log range: ", magit_log_range, sizeof(magit_log_range),
	    EFNEW | EFCR) == NULL || magit_log_range[0] == '\0')
		return (ABORT);
	return (magit_log_open(f, n));
}

/* l s: pickaxe -S (commits changing the occurrence count of a string). */
static int
magit_log_pickaxe_s(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	if (eread("Pickaxe -S (string): ", magit_log_pickaxe_term,
	    sizeof(magit_log_pickaxe_term), EFNEW | EFCR) == NULL ||
	    magit_log_pickaxe_term[0] == '\0')
		return (ABORT);
	magit_log_pickaxe = 'S';
	return (magit_log_open(f, n));
}

/* l G: pickaxe -G (commits whose diff matches a regex). */
static int
magit_log_pickaxe_g(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	if (eread("Pickaxe -G (regex): ", magit_log_pickaxe_term,
	    sizeof(magit_log_pickaxe_term), EFNEW | EFCR) == NULL ||
	    magit_log_pickaxe_term[0] == '\0')
		return (ABORT);
	magit_log_pickaxe = 'G';
	return (magit_log_open(f, n));
}

/* l l: open the *magit-log* buffer for the whole repo (newest first). */
static int
magit_log(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	return (magit_log_open(f, n));
}

/*
 * l f: log only the commits that touched a file -- the file at point (a
 * staged/unstaged/untracked entry), else prompt for a path.
 */
static int
magit_log_file(int f, int n)
{
	char	*path = NULL;
	int	 kind, hunk;

	magit_log_query_reset();
	kind = magit_at_point(&path, &hunk);
	if ((kind == MG_LINE_UNSTAGED || kind == MG_LINE_STAGED ||
	    kind == MG_LINE_UNTRACKED) && path != NULL && path[0] != '\0')
		(void)strlcpy(magit_log_file_path, path,
		    sizeof(magit_log_file_path));
	else if (eread("Log file: ", magit_log_file_path,
	    sizeof(magit_log_file_path), EFNEW | EFCR) == NULL ||
	    magit_log_file_path[0] == '\0')
		return (ABORT);
	return (magit_log_open(f, n));
}

static int
magit_log_refresh(int f, int n)
{
	struct buffer	*bp;

	if ((bp = bfind("*magit-log*", TRUE)) == NULL)
		return (FALSE);
	return (magit_log_build(bp));
}

/*
 * (Re)build the *magit-reflog* buffer + its per-line oid map (shared with the
 * *magit-log* map; rebuilt on every entry/refresh, so oid-at-point is correct
 * while this buffer is current).
 */
static int
magit_reflog_build(struct buffer *bp)
{
	struct mgwin	*wp;
	char		 cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd));
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	magit_log_count = 0;	/* magit_log_emit refills the oid map */
	(void)mg_magit_reflog_buffer(cwd, 100, magit_log_emit, bp);
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
	magit_log_oid_bp = bp;
	return (TRUE);
}

/* l h: open the HEAD reflog in a read-only *magit-reflog* buffer. */
static int
magit_reflog(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if ((bp = bfind("*magit-reflog*", TRUE)) == NULL)
		return (FALSE);
	if (magit_reflog_build(bp) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-reflog-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}

/* g in *magit-reflog*: rebuild in place. */
static int
magit_reflog_refresh(int f, int n)
{
	struct buffer	*bp;

	if ((bp = bfind("*magit-reflog*", FALSE)) == NULL)
		return (FALSE);
	return (magit_reflog_build(bp));
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if ((bp = bfind("*magit-blame*", TRUE)) == NULL)
		return (FALSE);
	(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd)); /* repo for in-buffer cmds */
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;

	/*
	 * Blame is slow (0.3-1.2s on big files); run it on the worker thread.
	 * Pop the buffer immediately with a placeholder and request the build;
	 * magit_async_apply() fills it on the wake when the result lands. If the
	 * worker isn't running (not a repo), fall back to a synchronous build.
	 */
	magit_blame_gen = mg_magit_async_request(MG_ASYNC_BLAME, cwd, path, 0);
	if (magit_blame_gen != 0)
		(void)addlinef(bp, "Blaming %s...", path);
	else if (mg_magit_blame_file(cwd, path, magit_plain_emit, bp) == 0) {
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

	/* Guard: the oid map is shared by *magit-log* and *magit-reflog*.  If
	 * the user switched buffers (C-x b / C-x o) without rebuilding, the
	 * map belongs to a different buffer and must not be used. */
	if (curbp != magit_log_oid_bp)
		return (NULL);
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if ((bp = bfind("*magit-commit*", TRUE)) == NULL)
		return (FALSE);
	(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd)); /* repo for in-buffer cmds */
	bp->b_flag |= BFIGNDIRTY;
	if (bclear(bp) != TRUE)
		return (FALSE);
	bp->b_flag |= BFREADONLY;
	magit_commit_bp = bp;		/* gate display.c's color hook on this */
	magit_cell_color_reset();	/* freed lines may be reused; drop memo */
	magit_commit_meta_count = 0;
	if (mg_magit_commit_diff(cwd, rev, magit_diff_emit, bp) == 0) {
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_apply_report(mg_magit_cherrypick(cwd, rev), "Cherry-pick",
	    f, n));
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	{
		int code = mg_magit_revert(cwd, oid);
		if (code == 0) {
			ewprintf("Revert failed");
			return (FALSE);
		}
		if (code == 2) {
			ewprintf("Revert left conflicts -- resolve in *magit-status* "
			    "(e o / e t), then c c");
			return (magit_refresh(f, n));
		}
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
	    kind != MG_LINE_DIFF && kind != MG_LINE_CONFLICT)) {
		ewprintf("Nothing to visit on this line");
		return (FALSE);
	}
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if (mg_magit_worktree_remove(cwd, name) != 1) {
		ewprintf("Worktree delete failed");
		return (FALSE);
	}
	return (magit_refresh(f, n));
}

/*
 * Echo a bisect step's progress line (or culprit) and refresh; `ok` is the
 * bridge call's 1/0 return, `msg` its filled message buffer.
 */
static int
magit_bisect_report(int ok, const char *msg, int f, int n)
{
	if (!ok) {
		ewprintf("Bisect failed");
		return (FALSE);
	}
	ewprintf("%s", msg);
	return (magit_refresh(f, n));
}

/*
 * Z s: start a bisect. Prompts for the bad revision (defaults to HEAD) and a
 * known-good revision, then checks out the first midpoint to test.
 */
static int
magit_bisect_start_cmd(int f, int n)
{
	char	bad[PATH_MAX], good[PATH_MAX], cwd[PATH_MAX], msg[256];

	(void)strlcpy(bad, "HEAD", sizeof(bad));
	if (eread("Bisect bad (revision): ", bad, sizeof(bad),
	    EFNEW | EFCR | EFDEF) == NULL || bad[0] == '\0')
		return (ABORT);
	if (eread("Bisect good (revision): ", good, sizeof(good),
	    EFNEW | EFCR) == NULL || good[0] == '\0')
		return (ABORT);
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_bisect_report(
	    mg_magit_bisect_start(cwd, bad, good, msg, sizeof(msg)), msg, f, n));
}

/* Z b / Z g: mark the checked-out commit bad / good, then advance the search. */
static int
magit_bisect_bad(int f, int n)
{
	char	cwd[PATH_MAX], msg[256];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_bisect_report(
	    mg_magit_bisect_mark(cwd, 1, msg, sizeof(msg)), msg, f, n));
}

static int
magit_bisect_good(int f, int n)
{
	char	cwd[PATH_MAX], msg[256];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_bisect_report(
	    mg_magit_bisect_mark(cwd, 0, msg, sizeof(msg)), msg, f, n));
}

/* Z r: end the bisect and return to the starting branch. */
static int
magit_bisect_reset_cmd(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if (mg_magit_bisect_reset(cwd) != 1) {
		ewprintf("Bisect reset failed");
		return (FALSE);
	}
	ewprintf("Bisect reset");
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_apply_report(mg_magit_merge(cwd, name), "Merge", f, n));
}

/* V: revert a commit (prompts for a revision) -- records the inverse on HEAD. */
static int
magit_revert(int f, int n)
{
	char	rev[PATH_MAX], cwd[PATH_MAX];

	if (eread("Revert commit: ", rev, sizeof(rev), EFNEW | EFCR) == NULL ||
	    rev[0] == '\0')
		return (ABORT);
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_apply_report(mg_magit_revert(cwd, rev), "Revert", f, n));
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

/* Reset HEAD to the reflog entry at point. mode: 0 soft, 1 mixed, 2 hard. */
static int
magit_reflog_do_reset(int mode, int f, int n)
{
	const char	*oid;
	char		 cwd[PATH_MAX];

	if ((oid = magit_log_oid_at_point()) == NULL) {
		ewprintf("Not on a reflog entry");
		return (FALSE);
	}
	if (mode == 2 &&
	    eyesno("Hard reset discards working-tree changes") != TRUE)
		return (FALSE);
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	if (mg_magit_reset(cwd, oid, mode) != 1) {
		ewprintf("Reset failed");
		return (FALSE);
	}
	return (magit_reflog_refresh(f, n));
}

static int magit_reflog_reset_soft(int f, int n)  { return magit_reflog_do_reset(0, f, n); }
static int magit_reflog_reset_mixed(int f, int n) { return magit_reflog_do_reset(1, f, n); }
static int magit_reflog_reset_hard(int f, int n)  { return magit_reflog_do_reset(2, f, n); }

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

/*
 * FM-GIT-CLI-WRITES P3: hand the terminal to a child `git` for an interactive
 * network op (push / pull / fetch), so git's credential helper / SSH agent /
 * GPG pinentry / progress meter all talk to the real tty -- the libgit2 path
 * cannot. magit_tty_suspend() is the spawncli prologue: leave raw mode + the
 * alt screen so the child inherits a cooked terminal.
 */
static int
magit_tty_suspend(void)
{
	ttcolor(CTEXT);
	ttnowindow();
	ttmove(nrow - 1, 0);
	if (epresf != FALSE) {
		tteeol();
		epresf = FALSE;
	}
	if (ttcooked() == FALSE)
		return (FALSE);
	tttidy();
	ttflush();
	return (TRUE);
}

/* Restore raw mode + queue a full repaint (the spawncli epilogue). */
static void
magit_tty_restore(void)
{
	ttreinit();
	sgarbf = TRUE;
	(void)ttraw();
}

/* As magit_tty_restore, but first let the user read git's output (auth result,
 * push summary, errors) before mg repaints over it. */
static void
magit_tty_pause_and_restore(void)
{
	char	buf[64];

	fputs("\n-- press ENTER to return to mg --", stdout);
	fflush(stdout);
	(void)fgets(buf, sizeof(buf), stdin);
	magit_tty_restore();
}

enum magit_net_op { MNET_FETCH, MNET_PUSH, MNET_PULL, MNET_PULL_REBASE };

/*
 * Run an interactive git network op with the terminal handed to git. `banner`
 * prints on the inherited tty first. Returns git's exit code (0 = success), -1
 * if git could not be executed (caller falls back to the libgit2 path), or -2
 * if the tty could not be suspended (abort). On return the tty is back in raw
 * mode with a repaint queued.
 */
static int
magit_run_net(enum magit_net_op op, const char *cwd, int a, int b,
    const char *banner)
{
	int	code;

	if (magit_tty_suspend() == FALSE)
		return (-2);
	fputs(banner, stdout);
	fputc('\n', stdout);
	fflush(stdout);

	switch (op) {
	case MNET_PUSH:
		code = mg_magit_push_cli(cwd, a, b);
		break;
	case MNET_PULL:
		code = mg_magit_pull_cli(cwd, 0);
		break;
	case MNET_PULL_REBASE:
		code = mg_magit_pull_cli(cwd, 1);
		break;
	case MNET_FETCH:
	default:
		code = mg_magit_fetch_cli(cwd);
		break;
	}

	if (code == -1)
		magit_tty_restore();		/* git never ran -> fall back */
	else
		magit_tty_pause_and_restore();
	return (code);
}

/* f: fetch from origin (updates remote-tracking refs). */
static int
magit_fetch(int f, int n)
{
	char	cwd[PATH_MAX];
	int	code;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	code = magit_run_net(MNET_FETCH, cwd, 0, 0, "Fetching from origin...");
	if (code == -2)
		return (FALSE);
	if (code == -1) {			/* git unavailable -> libgit2 */
		ewprintf("Fetching from origin...");
		if (mg_magit_fetch(cwd, "origin") != 1) {
			ewprintf("Fetch failed (no origin, or auth required)");
			return (FALSE);
		}
		ewprintf("Fetched from origin");
		return (magit_refresh(f, n));
	}
	if (code != 0) {
		ewprintf("Fetch failed (see output)");
		return (magit_refresh(f, n));
	}
	ewprintf("Fetched from origin");
	return (magit_refresh(f, n));
}

/* F: pull from origin (fetch + merge the upstream into HEAD). */
static int
magit_pull(int f, int n)
{
	char	cwd[PATH_MAX];
	int	code;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	code = magit_run_net(MNET_PULL, cwd, 0, 0, "Pulling from origin...");
	if (code == -2)
		return (FALSE);
	if (code == -1)				/* git unavailable -> libgit2 */
		return (magit_apply_report(mg_magit_pull(cwd, "origin"), "Pull",
		    f, n));
	if (code != 0) {
		ewprintf("Pull stopped -- if conflicts, resolve (e o / e t), "
		    "then c c");
		return (magit_refresh(f, n));
	}
	ewprintf("Pull done");
	return (magit_refresh(f, n));
}

/* Shared push helper: `force` / `set_upstream` map to the engine flags. */
static int
magit_do_push(int force, int set_upstream, int f, int n)
{
	char	cwd[PATH_MAX];
	int	code;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	code = magit_run_net(MNET_PUSH, cwd, force, set_upstream,
	    "Pushing to origin...");
	if (code == -2)
		return (FALSE);
	if (code == -1) {			/* git unavailable -> libgit2 */
		ewprintf("Pushing to origin...");
		if (mg_magit_push(cwd, "origin", force, set_upstream) != 1) {
			ewprintf("Push failed (no origin, non-fast-forward, or "
			    "auth required)");
			return (FALSE);
		}
		ewprintf("Pushed to origin");
		return (magit_refresh(f, n));
	}
	if (code != 0) {
		ewprintf("Push failed (see output)");
		return (magit_refresh(f, n));
	}
	ewprintf("Pushed to origin");
	return (magit_refresh(f, n));
}

/* P p: push the current branch to origin, honoring the -f/-u transient
 * infixes (force-with-lease / set-upstream). */
static int
magit_push(int f, int n)
{
	return (magit_do_push(push_infixes[0].on, push_infixes[1].on, f, n));
}

/* F r: pull --rebase (fetch then rebase onto the upstream). */
static int
magit_pull_rebase(int f, int n)
{
	char	cwd[PATH_MAX];
	int	code;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	code = magit_run_net(MNET_PULL_REBASE, cwd, 0, 0,
	    "Pulling (rebase) from origin...");
	if (code == -2)
		return (FALSE);
	if (code == -1)				/* git unavailable -> libgit2 */
		return (magit_rebase_report(mg_magit_pull_rebase(cwd, "origin"),
		    "Pull --rebase", f, n));
	if (code != 0) {
		ewprintf("Pull --rebase stopped -- resolve, then r r / r s / "
		    "r a");
		return (magit_refresh(f, n));
	}
	ewprintf("Pull --rebase complete");
	return (magit_refresh(f, n));
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

/*
 * Interpret an apply-style bridge result (1 done / 2 left conflicts / 0 fail)
 * for merge / revert / cherry-pick / pull; report and refresh the status buffer.
 */
static int
magit_apply_report(int code, const char *what, int f, int n)
{
	if (code == 0) {
		ewprintf("%s failed", what);
		return (FALSE);
	}
	if (code == 2)
		ewprintf("%s left conflicts -- resolve (e o / e t), then c c to "
		    "commit", what);
	else
		ewprintf("%s done", what);
	return (magit_refresh(f, n));
}

/* r u: rebase the current branch onto its upstream. */
static int
magit_rebase_upstream(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase(cwd, onto), "Rebase", f, n));
}

/* r r: continue a paused rebase (after resolving + staging conflicts). */
static int
magit_rebase_continue(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase_continue(cwd),
	    "Rebase continue", f, n));
}

/* r s: skip the current commit of a paused rebase. */
static int
magit_rebase_skip(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	return (magit_rebase_report(mg_magit_rebase_skip(cwd), "Rebase skip",
	    f, n));
}

/* r a: abort a paused rebase, restoring the pre-rebase state. */
static int
magit_rebase_abort(int f, int n)
{
	char	cwd[PATH_MAX];

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	/* Capture the repo from the invoking buffer (curbp, before showbuffer
	 * switches to *magit-commit*) so the message prefill + the commit itself
	 * target the right repo, not the process cwd. */
	if (getbufcwd(cwd, sizeof(cwd)) == TRUE)
		(void)strlcpy(bp->b_cwd, cwd, sizeof(bp->b_cwd));
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
	if (op != MG_COMMIT_NEW && getbufcwd(cwd, sizeof(cwd)) == TRUE) {
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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	case 5: return ("edit");
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
	    "w reword  e edit  M-n/M-p reorder  C-c C-c run  C-c C-k abort");

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
	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
static int magit_todo_edit(int f, int n)   { return (magit_todo_set(5)); }

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

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
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
	if (rc == 0) {
		ewprintf("Interactive rebase failed (conflict? -- nothing changed)");
		return (FALSE);
	}
	if (rc == 3) {
		/* Stopped at an `edit`: HEAD is the marked commit; the Rebasing
		 * section now drives r r (continue) / r a (abort). */
		(void)magit_refresh(f, n);
		ewprintf("Stopped for edit -- amend (c a / c e), then r r to "
		    "continue");
		return (TRUE);
	}
	if (rc == 2) {
		/* A replayed commit conflicted: resolve in the Conflicts section,
		 * commit, then r r (continue) / r a (abort). */
		(void)magit_refresh(f, n);
		ewprintf("Rebase conflict -- resolve (e o / e t), commit, then r r");
		return (TRUE);
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
