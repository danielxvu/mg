# M8 — magit breadth (navigation + more sections)

**Status:** approved (roadmap item) 2026-06-19 · **Branch:** `cpp-refactor`

M3–M7 built the core status/stage/commit/diff loop. M8 widens it toward a
usable everyday Magit: navigate out of the status buffer, discover the keys,
and surface more of the repository. Decomposed into independent slices, built
incrementally and PR'd per the stack.

## M8-1 — `RET` visits the file at point (pure C, this slice)

- In `magit-status-mode`, `RET` on a file line (`MG_LINE_UNTRACKED` /
  `MG_LINE_UNSTAGED` / `MG_LINE_STAGED`) opens that file in another window
  (poptofile-style: the status buffer stays visible). On a hunk/diff line,
  `RET` visits the owning file too (line-precise jump is a later nicety).
- No engine work: reuse the line→meta `path`, join it onto the repo cwd, and
  reuse mg's `adjustname`/`findbuffer`/`popbuf`/`readin` (the non-prompting
  core of `poptofile`).
- Test: bridge already exposes the path per line; the C glue is covered by an
  end-to-end pty check (RET on a file row opens a buffer with its contents).

## M8-2 — `?` help popup

- `?` shows a transient `*magit-help*` buffer (or echo-area legend) listing the
  magit-status keys: `TAB s u k c g q RET ?`. Pure C; a static text buffer.

## M8-3 — stash + branches sections (engine + render)

- `mg.git` gains read-only listers: `stashes(repo) -> vector<stash_entry>`
  (`git_stash_foreach`) and `branches(repo) -> vector<branch_entry>`
  (`git_branch_iterator`, local + current flag). `std::expected`, RAII as usual.
- Bridge `mg_magit_status_buffer` emits a "Stashes (N)" and a "Branches (N)"
  section (new `MG_LINE_*` kinds). Render-only for M8-3; acting on them
  (`apply`/`pop` stash, checkout branch) is a later slice.
- Tests (`test_git`): a fixture with one stash / two branches lists them.

## M8-4 — section navigation (optional)

- `M-n` / `M-p` jump to the next/previous section header. Pure C over the
  line→meta map (section-header lines are `MG_LINE_OTHER` with empty path; a
  small "is this a header" predicate suffices, or tag them with a new kind).

## Order & risk

M8-1 (RET) and M8-2 (?) are pure-C, zero-engine, high-value — do them first.
M8-3 adds libgit2 listers (the only new engine work). M8-4 is a small polish.
Build 1 → 2 → 3 → 4, each its own commit + stacked PR.

## Out of scope

Acting on stashes/branches (checkout/apply/pop); remotes/fetch/push; the log
view as a separate buffer (recent-commits already shows the tail); line-precise
`RET` into a hunk.
