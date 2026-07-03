# FM-SHOW-REFS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `y` opens `*magit-refs*` — all local/remote branches with ahead/behind vs HEAD, plus the full tag list; `RET` shows a ref's tip commit; `b` opens the branch menu.

**Architecture:** Engine `refs_overview()` (branch iterator + tag glob/peel + `git_graph_ahead_behind`); bridge `mg_magit_refs_buffer()` composes the sections; editor `*magit-refs*` follows the `*magit-reflog*` pattern exactly, becoming the third user of the shared per-line oid map (so `RET` = the existing `magit_log_visit`).

**Tech Stack:** C++23 module (`mg.git`, libgit2); the mg buffer/keymap machinery; the pty test harness.

## Global Constraints

- Ref rows carry **`path` = the full tip oid** (tags peeled to commits) — NOT the name. (The status buffer's `MG_LINE_BRANCH` rows carry the name; different buffer, different keymap — document at both emit sites.)
- Ahead/behind semantics: `ahead` = commits on the ref not on HEAD; `behind` = commits on HEAD not on the ref; `-1` = unknown (never blocks the listing).
- Unborn HEAD → `Refs (no commits yet)` + empty sections, no error. Detached → `Refs (HEAD detached at <short>)`.
- Symbolic branch refs (`origin/HEAD`) are SKIPPED in this listing (noise here, unlike decorations) — detect via `git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC`, do NOT rely on resolve failing (it succeeds for `origin/HEAD`; proven in FM-LOG-RICH review).
- `*magit-refs*` is NOT a color buffer (deferral).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine — `refs_overview()`

**Files:**
- Modify: `src/magit/git.cppm` (types + declaration near `branches()`'s ~560; definition near `branches()` ~1679)
- Test: `tests/test_git.cpp`

**Interfaces:**
- Produces:
```cpp
struct ref_row {
    std::string name;     // shorthand: "master", "origin/master", "v1.0"
    std::string oid;      // full tip oid (tags: peeled to the commit)
    bool        is_head = false; // locals only
    long        ahead  = -1; // commits on this ref not on HEAD (-1 = unknown)
    long        behind = -1; // commits on HEAD not on this ref (-1 = unknown)
};
struct refs_overview_result {
    std::string head_name;   // "master"; "" when detached/unborn
    std::string head_oid;    // "" when unborn
    std::string upstream;    // HEAD's upstream shorthand, "" if none
    std::vector<ref_row> locals, remotes, tags;
};
std::expected<refs_overview_result, error> refs_overview(std::string repo);
```

- [ ] **Step 1: Write the failing test**

Add to `tests/test_git.cpp`:
```cpp
TEST_CASE("refs_overview lists locals with ahead/behind vs HEAD, remotes absent, tags peeled")
{
    auto dir = make_repo_with_commit("base");
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    // feature branches off base; then one commit on EACH side -> feature is
    // ahead 1 (its commit) and behind 1 (HEAD's commit) of HEAD.
    REQUIRE(run("branch feature") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@e commit --allow-empty -m on-head") == 0);
    REQUIRE(run("checkout -q feature") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@e commit --allow-empty -m on-feature") == 0);
    REQUIRE(run("checkout -q -") == 0); // back to the original branch (HEAD)
    REQUIRE(run("-c user.name=T -c user.email=t@e tag -a v1 -m msg") == 0); // annotated

    auto r = mg::git::refs_overview(d);
    REQUIRE(r.has_value());
    CHECK(!r->head_name.empty());
    CHECK(r->head_oid.size() == 40);

    const mg::git::ref_row *head = nullptr, *feat = nullptr;
    for (const auto &b : r->locals) {
        if (b.is_head) head = &b;
        if (b.name == "feature") feat = &b;
    }
    REQUIRE(head != nullptr);
    CHECK(head->name == r->head_name);
    REQUIRE(feat != nullptr);
    CHECK(feat->ahead == 1);  // on-feature
    CHECK(feat->behind == 1); // on-head
    CHECK(feat->oid.size() == 40);

    CHECK(r->remotes.empty()); // no remotes configured
    REQUIRE(r->tags.size() == 1);
    CHECK(r->tags[0].name == "v1");
    CHECK(r->tags[0].oid == r->head_oid); // annotated tag PEELED to the commit

    // Detached HEAD: head_name empties, rows still listed.
    REQUIRE(run("checkout -q --detach") == 0);
    auto rd = mg::git::refs_overview(d);
    REQUIRE(rd.has_value());
    CHECK(rd->head_name.empty());
    CHECK(!rd->head_oid.empty());
    CHECK(rd->locals.size() == 2);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (compile error: `refs_overview` undeclared — that is the red). `cmake --build build --target test_git`

- [ ] **Step 3: Implement**

Declaration (with the types) near `branches()`'s declaration (~560); definition near `branches()` (~1679), following its RAII idiom:
```cpp
std::expected<refs_overview_result, error> refs_overview(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    refs_overview_result out;

    // HEAD: unborn -> empty result (a valid state, not an error).
    const git_oid *head_oid = nullptr;
    git_reference *raw_head = nullptr;
    int hrc = git_repository_head(&raw_head, repo.get());
    if (hrc == GIT_EUNBORNBRANCH || hrc == GIT_ENOTFOUND)
        return out;
    if (hrc != 0)
        return std::unexpected(last_error());
    detail::ref_ptr head(raw_head);
    head_oid = git_reference_target(head.get());
    if (head_oid != nullptr)
        out.head_oid = detail::full_oid(head_oid);
    if (git_repository_head_detached(repo.get()) != 1)
        if (const char *sh = git_reference_shorthand(head.get()))
            out.head_name = sh;

    // HEAD's upstream shorthand (for the starred row's annotation).
    if (!out.head_name.empty()) {
        git_reference *raw_up = nullptr;
        if (git_branch_upstream(&raw_up, head.get()) == 0) {
            detail::ref_ptr up(raw_up);
            if (const char *sh = git_reference_shorthand(up.get()))
                out.upstream = sh;
        }
    }

    // Branches, local + remote. Symbolic refs (origin/HEAD) are noise in this
    // listing -- skip by TYPE (resolve would succeed for them; see FM-LOG-RICH).
    {
        git_branch_iterator *raw_iter = nullptr;
        if (git_branch_iterator_new(&raw_iter, repo.get(), GIT_BRANCH_ALL) != 0)
            return std::unexpected(last_error());
        detail::branch_iter_ptr iter(raw_iter);
        git_reference *raw_ref = nullptr;
        git_branch_t type;
        int rc;
        while ((rc = git_branch_next(&raw_ref, &type, iter.get())) == 0) {
            detail::ref_ptr ref(raw_ref);
            if (git_reference_type(ref.get()) == GIT_REFERENCE_SYMBOLIC)
                continue;
            const char *sh = git_reference_shorthand(ref.get());
            const git_oid *o = git_reference_target(ref.get());
            if (sh == nullptr || o == nullptr)
                continue;
            ref_row row;
            row.name = sh;
            row.oid = detail::full_oid(o);
            row.is_head = (type == GIT_BRANCH_LOCAL &&
                           git_branch_is_head(ref.get()) == 1);
            // Counts vs HEAD (ahead = on ref, not HEAD). Skip for HEAD's own
            // row (0/0 by definition) and when HEAD is unborn-oid.
            if (!row.is_head && head_oid != nullptr) {
                std::size_t a = 0, b = 0;
                if (git_graph_ahead_behind(&a, &b, repo.get(), o,
                                           head_oid) == 0) {
                    row.ahead = static_cast<long>(a);
                    row.behind = static_cast<long>(b);
                }
            } else if (row.is_head) {
                row.ahead = 0;
                row.behind = 0;
            }
            (type == GIT_BRANCH_LOCAL ? out.locals : out.remotes)
                .push_back(std::move(row));
        }
        if (rc != GIT_ITEROVER)
            return std::unexpected(last_error());
    }

    // Tags, peeled to commits (the decorations() idiom); no counts.
    {
        git_reference_iterator *raw_iter = nullptr;
        if (git_reference_iterator_glob_new(&raw_iter, repo.get(),
                                            "refs/tags/*") != 0)
            return std::unexpected(last_error());
        std::unique_ptr<git_reference_iterator,
            decltype([](git_reference_iterator *i) { git_reference_iterator_free(i); })>
            iter(raw_iter);
        git_reference *raw_ref = nullptr;
        int rc;
        while ((rc = git_reference_next(&raw_ref, iter.get())) == 0) {
            detail::ref_ptr ref(raw_ref);
            const char *sh = git_reference_shorthand(ref.get());
            git_object *raw_obj = nullptr;
            if (sh == nullptr ||
                git_reference_peel(&raw_obj, ref.get(), GIT_OBJECT_COMMIT) != 0)
                continue; // tag of a non-commit: skip
            detail::object_ptr obj(raw_obj);
            ref_row row;
            row.name = sh;
            row.oid = detail::full_oid(git_object_id(obj.get()));
            out.tags.push_back(std::move(row));
        }
        if (rc != GIT_ITEROVER)
            return std::unexpected(last_error());
    }
    return out;
}
```

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(refs): engine refs_overview() with ahead/behind vs HEAD"
```

---

### Task 2: Bridge — `mg_magit_refs_buffer`

**Files:**
- Modify: `src/magit/bridge.h` (decl near `mg_magit_reflog_buffer`'s ~93), `src/magit/bridge.cpp` (impl near `mg_magit_reflog_buffer` ~1008)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Consumes: Task 1's `refs_overview()`.
- Produces: `int mg_magit_refs_buffer(const char *repo_path, mg_magit_emit_fn emit, void *ctx);` — sections as `MG_LINE_SECTION`, branch rows as `MG_LINE_BRANCH` with `path` = the full tip oid, tag rows as `MG_LINE_TAG` likewise. Returns the line count, 0 on failure.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_bridge.cpp`:
```cpp
TEST_CASE("mg_magit_refs_buffer emits sections and oid-carrying branch rows")
{
    auto dir = make_repo_with_commit("c0");
    std::string d = dir.string();
    struct row { std::string line, path; int kind; };
    std::vector<row> rows;
    int n = mg_magit_refs_buffer(d.c_str(),
        [](void *c, const char *l, int kind, const char *p, int) {
            static_cast<std::vector<row>*>(c)->push_back(
                {l, p ? p : "", kind});
        }, &rows);
    REQUIRE(n >= 3); // header + Branches section + >=1 branch row
    bool section = false, starred = false, oid_path = false;
    for (const auto &r : rows) {
        if (r.kind == MG_LINE_SECTION &&
            r.line.find("Branches (") != std::string::npos)
            section = true;
        if (r.kind == MG_LINE_BRANCH && r.line.rfind("* ", 0) == 0)
            starred = true;
        if (r.kind == MG_LINE_BRANCH && r.path.size() == 40 &&
            r.path.find_first_not_of("0123456789abcdef") == std::string::npos)
            oid_path = true;
    }
    CHECK(section);  // the Branches (N) header emitted
    CHECK(starred);  // the checked-out branch is starred
    CHECK(oid_path); // branch rows carry the tip OID (not the name)
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (link error: `mg_magit_refs_buffer` undefined — the red).

- [ ] **Step 3: Implement**

`bridge.h`, after `mg_magit_reflog_buffer`'s decl (~94):
```c
/* Compose the *magit-refs* overview: Branches/Remotes (MG_LINE_BRANCH) and
 * Tags (MG_LINE_TAG) sections with ahead/behind vs HEAD. NOTE: here `path` is
 * the ref's full TIP OID (tags peeled) so RET can show the commit -- unlike
 * the status buffer's branch rows, whose `path` is the branch NAME (their RET
 * checks out). Returns the line count, 0 when the repo can't be read. */
int mg_magit_refs_buffer(const char *repo_path, mg_magit_emit_fn emit,
                         void *ctx);
```
`bridge.cpp`, after `mg_magit_reflog_buffer` (~1015):
```cpp
extern "C" int mg_magit_refs_buffer(const char *repo_path,
                                    mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;
    auto r = mg::git::refs_overview(repo_path);
    if (!r)
        return 0;
    int n = 0;
    auto out = [&](const std::string &line, int kind, const char *path) {
        emit(ctx, line.c_str(), kind, path, -1);
        ++n;
    };
    // Header: named / detached / unborn.
    if (!r->head_name.empty())
        out("Refs (HEAD: " + r->head_name + ")", MG_LINE_SECTION, nullptr);
    else if (!r->head_oid.empty())
        out("Refs (HEAD detached at " + r->head_oid.substr(0, 8) + ")",
            MG_LINE_SECTION, nullptr);
    else
        out("Refs (no commits yet)", MG_LINE_SECTION, nullptr);

    // "ahead A, behind B" phrase; empty when 0/0 or unknown (-1).
    auto counts = [](const mg::git::ref_row &b) -> std::string {
        std::string s;
        if (b.ahead > 0)
            s += "ahead " + std::to_string(b.ahead);
        if (b.behind > 0)
            s += (s.empty() ? "" : ", ") + std::string("behind ") +
                 std::to_string(b.behind);
        return s;
    };
    auto ref_line = [&](const mg::git::ref_row &b) {
        std::string line = (b.is_head ? "* " : "  ") + b.name;
        // The starred row shows its upstream; others show counts vs HEAD.
        std::string ann = b.is_head ? r->upstream : counts(b);
        if (!ann.empty())
            line += "  " + ann;
        // NOTE: path = the tip OID (see bridge.h) -- RET shows the commit.
        out(line, MG_LINE_BRANCH, b.oid.c_str());
    };

    if (!r->locals.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Branches (" + std::to_string(r->locals.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &b : r->locals)
            ref_line(b);
    }
    if (!r->remotes.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Remotes (" + std::to_string(r->remotes.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &b : r->remotes)
            ref_line(b);
    }
    if (!r->tags.empty()) {
        out("", MG_LINE_OTHER, nullptr);
        out("Tags (" + std::to_string(r->tags.size()) + ")",
            MG_LINE_SECTION, nullptr);
        for (const auto &t : r->tags)
            out("  " + t.name, MG_LINE_TAG, t.oid.c_str());
    }
    return n;
}
```
Also add the "path = branch NAME here" counterpart note at the STATUS buffer's branch emit site (`bridge.cpp` ~855, the `out(std::string(b.is_head ? "* " : "  ") + b.name, MG_LINE_BRANCH, b.name.c_str())` line) — one comment line.

- [ ] **Step 4: Run → PASS.** Full suite.

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.h src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(refs): mg_magit_refs_buffer composes the refs overview"
```

---

### Task 3: Editor — `y` opens `*magit-refs*`

**Files:**
- Modify: `src/magit_cmd.c` (forward decls ~128-151; PF arrays + `magrefsmap` next to `magit_reflogmap` ~628; `magit_log_emit` kind generalization ~1493; `maps_add` registration ~1354; `magitmap` `y` entry + KEYMAPE bump ~1091; the build/open/refresh/quit functions next to the reflog ones ~2504)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: Task 2's `mg_magit_refs_buffer`; the shared oid map (`magit_log_oid`/`magit_log_count`/`magit_log_oid_bp`), `magit_log_visit`, `magit_menu_branch`, `magit_metamap`.

- [ ] **Step 1: Write the failing pty test**

```cpp
TEST_CASE("y opens the *magit-refs* overview")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool opened = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "y", 1);        // open the refs overview
            opened = wait_for(master, "Branches (", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(opened); // y rendered *magit-refs* with the Branches section
}
```
Run → FAIL (`y` unbound in `*magit-status*`).

- [ ] **Step 2: Generalize the shared emit callback**

`magit_log_emit` (~1493) — record oids for ref rows too (log/reflog never emit these kinds, so their behavior is unchanged):
```c
	if (magit_log_count < MAGIT_MAX_LINES) {
		if ((kind == MG_LINE_COMMIT || kind == MG_LINE_BRANCH ||
		    kind == MG_LINE_TAG) && path != NULL)
			(void)strlcpy(magit_log_oid[magit_log_count], path,
			    MAGIT_OID_LEN);
		else
			magit_log_oid[magit_log_count][0] = '\0';
		magit_log_count++;
	}
```
Update the oid-map comment block (~228) to name `*magit-refs*` as the third user.

- [ ] **Step 3: Add the keymap, build/open/refresh/quit, and the `y` binding**

Forward decls (next to `magit_reflog_build`'s at ~151):
```c
static int	magit_refs_build(struct buffer *);
static int	magit_show_refs(int, int);
static int	magit_refs_refresh(int, int);
```
PF arrays + keymap next to `magit_reflogmap` (~628). First READ the reflog's `reflog_g`/`reflog_q`/`reflog_esc` PF arrays and mirror them; the quit action is whatever `reflog_q` points at (a shared close command — reuse the same function if it is buffer-agnostic, else mirror it):
```c
/*
 * *magit-refs* keymap: RET shows the ref's tip commit (reuses magit_log_visit
 * via the shared oid map), b opens the branch menu (checkout lives there),
 * g refreshes, q closes. ESC prefix forwards to magit_metamap (M-n/M-p).
 * Entries MUST be ascending: CCHR('M')=13, CCHR('[')=27, 'b'=98, 'g'=103,
 * 'q'=113.
 */
static PF refs_ret[] = { magit_log_visit };	/* RET: show tip commit */
static PF refs_b[]   = { magit_menu_branch };	/* b: branch menu */
static PF refs_g[]   = { magit_refs_refresh };	/* g: refresh */
static struct KEYMAPE (5) magrefsmap = {
	5,
	5,
	rescan,
	{
		{ CCHR('M'), CCHR('M'), refs_ret, NULL },
		{ CCHR('['), CCHR('['), refs_esc,	/* ESC: meta prefix */
		    (KEYMAP *)&magit_metamap },
		{ 'b', 'b', refs_b, NULL },
		{ 'g', 'g', refs_g, NULL },
		{ 'q', 'q', refs_q, NULL }
	}
};
```
(`refs_esc` and `refs_q`: mirror exactly how `reflog_esc`/`reflog_q` are declared — read them first; if `reflog_q`'s function is generic "close this window", point `refs_q` at the same function instead of duplicating.)

Build/open/refresh next to the reflog ones (~2504), mirroring them line-for-line:
```c
/* (Re)build the *magit-refs* buffer + the shared per-line oid map. */
static int
magit_refs_build(struct buffer *bp)
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
	(void)mg_magit_refs_buffer(cwd, magit_log_emit, bp);
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

/* y: open the refs overview in a read-only *magit-refs* buffer. */
static int
magit_show_refs(int f, int n)
{
	struct buffer	*bp;
	struct mgwin	*wp;

	if ((bp = bfind("*magit-refs*", TRUE)) == NULL)
		return (FALSE);
	if (magit_refs_build(bp) != TRUE)
		return (FALSE);
	if ((wp = popbuf(bp, WNONE)) == NULL)
		return (FALSE);
	curwp = wp;
	curbp = bp;
	wp->w_dotp = bp->b_dotp;
	wp->w_doto = bp->b_doto;
	bp->b_modes[1] = name_mode("magit-refs-mode");
	bp->b_nmodes = 1;
	return (TRUE);
}

/* g in *magit-refs*: rebuild in place. */
static int
magit_refs_refresh(int f, int n)
{
	struct buffer	*bp;

	if ((bp = bfind("*magit-refs*", FALSE)) == NULL)
		return (FALSE);
	return (magit_refs_build(bp));
}
```
(Mirror `magit_reflog_refresh`'s exact body shape — read it in case it differs.)

Mode registration next to the reflog's (~1354):
```c
		maps_add((KEYMAP *)&magrefsmap, "magit-refs-mode");
```
`magitmap` (~1091): `KEYMAPE (37)` → `(38)`, both counts `37` → `38`, and insert between `'w'` and `'z'`:
```c
		{ 'w', 'w', magit_w, NULL },			/* w: toggle -w (ignore whitespace) */
		{ 'y', 'y', magit_y, NULL },			/* y: refs overview */
		{ 'z', 'z', magit_z, NULL } /* z: stash menu */
```
with the PF array next to `magit_w`'s:
```c
static PF magit_y[]     = { magit_show_refs };		/* y: refs overview */
```

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure` (the keymap-sort assert validates the `y` insert; `magit_assert_menus_consistent` doesn't cover `magrefsmap`, but the ascending comment documents it like the reflog's).

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(refs): y opens *magit-refs* (RET visits, b branch menu)"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-showrefs .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: `y` in a real repo — sections render with counts; `RET` on a branch shows its tip commit; `RET` on a section header errors gracefully ("Not on a commit"); `b` opens the branch menu; `g`/`q` work; a fresh `git init` repo shows `Refs (no commits yet)`.
- [ ] Update `todo.md`: mark FM-SHOW-REFS done; record deferrals (refs-buffer coloring, per-remote sub-sections, `k` delete from refs, comparison-ref selection, `y` as a transient).
- [ ] Open the PR based on `neomg`.
