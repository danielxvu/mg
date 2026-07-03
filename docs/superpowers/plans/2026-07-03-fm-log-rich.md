# FM-LOG-RICH Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `*magit-log*` commit rows show git's ref decoration (`(HEAD -> master, tag: v1)`) colored by ref kind — on BOTH log paths (the default libgit2 log and the filtered CLI log).

**Architecture:** Engine: `%d` as a new `%x1f` field in `log_query`'s format (CLI path), plus a `decorations()` oid→refs map over libgit2 (for the default/per-file paths). Bridge: `mg_magit_log_buffer`/`mg_magit_log_file_buffer` compose the decoration from the map. Editor: `*magit-log*` joins the per-cell color system — `magit_cell_color` parses the decoration group once per line (memoized in `magit_color_kindcol`) and classifies segments by ref grammar into the existing `ttfgkind` palette. No `display.c` or emit-ABI changes.

**Tech Stack:** C++23 module (`mg.git`, libgit2); the mg per-cell color hook in `magit_cmd.c`; the pty test harness.

## Global Constraints

- Undecorated rows compose **byte-identical** to today (`<short> <summary>`, graph rows `<graph art><short> <summary>`); graph connector rows unchanged.
- Coloring is never load-bearing: a line whose paren group fails the ref grammar gets **no** color (kind 0) — never a wrong-data behavior.
- Color kinds map to the existing `ttfgkind` palette (do NOT touch `display.c`): HEAD/`->` = 4 (cyan), local branch = 2 (green), remote (name contains `/`) = 1 (blue), `tag: …` = 5 (yellow), parens/commas = 0.
- `log_row` gains no new member; reflog and rebase-todo rows are NOT decorated.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine — `%d` in `log_query` + the `decorations()` map

**Files:**
- Modify: `src/magit/git.cppm` (format arg ~line 1602; row parser ~1642; new `decorations()` + its declaration near `branches()`'s at ~560)
- Test: `tests/test_git.cpp`

**Interfaces:**
- Produces: `log_query` rows shaped `<graph art><short> (<refs>) <summary>` when decorated; `decorations(std::string repo) -> std::expected<std::map<std::string, std::string>, error>` mapping full oid → `"HEAD -> master, origin/main, tag: v1"` (no parens). Task 2 consumes `decorations()`; Task 3 consumes the text shape.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_git.cpp`:
```cpp
TEST_CASE("log_query decorates ref-pointed rows and leaves others byte-identical")
{
    auto dir = make_repo_with_commit("first");
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("-c user.name=T -c user.email=t@e commit --allow-empty -m second") == 0);
    REQUIRE(run("tag v1") == 0); // lightweight tag at HEAD

    auto r = mg::git::log_query(d, mg::git::log_options{});
    REQUIRE(r.has_value());
    REQUIRE(r->size() >= 2);
    const std::string &head = (*r)[0].text;
    CHECK(head.find("(HEAD -> ") != std::string::npos);
    CHECK(head.find("tag: v1") != std::string::npos);
    CHECK(head.find(") second") != std::string::npos); // decoration precedes summary
    const std::string &plain = (*r)[1].text; // no refs point at "first"
    CHECK(plain.find('(') == std::string::npos);
    CHECK(plain.find("first") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("decorations maps tip oid to HEAD/branch/tag; peels annotated tags")
{
    auto dir = make_repo_with_commit("base");
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("-c user.name=T -c user.email=t@e tag -a v2 -m annotated") == 0);

    auto rows = mg::git::log_query(d, mg::git::log_options{}); // fetch the tip oid
    REQUIRE(rows.has_value());
    REQUIRE(!rows->empty());
    const std::string tip = (*rows)[0].oid;

    auto m = mg::git::decorations(d);
    REQUIRE(m.has_value());
    auto it = m->find(tip);
    REQUIRE(it != m->end());
    CHECK(it->second.find("HEAD -> ") != std::string::npos);
    CHECK(it->second.find("tag: v2") != std::string::npos); // annotated tag PEELED to the commit
    // A fabricated oid is absent.
    CHECK(m->find(std::string(40, '0')) == m->end());
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL.** `cmake --build build --target test_git && ./build/tests/test_git --test-case="*decorat*"`
Expected: first test FAILs (no `%d` in the format); second doesn't compile (`decorations` undeclared) — that compile error is its red.

- [ ] **Step 3: Implement**

**(a) The format line (~1602):**
```cpp
    // Leading %x1f so --graph's art lands in field[0] and the same parser
    // handles graph + non-graph lines uniformly. %d = ref decoration
    // (" (HEAD -> master, tag: v1)" or empty), its own field so the parser
    // composes git-oneline order and skips it cleanly when empty.
    args.emplace_back("--format=%x1f%H%x1f%h%x1f%d%x1f%s");
```
**(b) The parser's commit-row branch (~1642)** — fields become `f[0]=graph art, f[1]=full, f[2]=short, f[3]=decoration, f[4]=summary`:
```cpp
        if (f.size() >= 5) {
            // f[0]=graph art, f[1]=full, f[2]=short, f[3]=decoration, f[4]=summary
            log_row row;
            // %d renders " (refs)" with a leading space, or "". Compose
            // git-oneline order; undecorated rows stay byte-identical.
            std::string dec = f[3];
            if (!dec.empty() && dec.front() == ' ')
                dec.erase(0, 1);
            row.text = dec.empty() ? f[0] + f[2] + " " + f[4]
                                   : f[0] + f[2] + " " + dec + " " + f[4];
            row.oid = f[1];
            rows.push_back(std::move(row));
        } else if (!line.empty()) {
            rows.push_back(log_row{line, ""}); // connector-only line
        }
```
**(c) `decorations()`** — declaration next to `branches()`'s (~560):
```cpp
// FM-LOG-RICH: full oid -> composed ref decoration (no parens), e.g.
// "HEAD -> master, origin/main, tag: v1". Segment order: the checked-out
// branch (HEAD ->) first, then local branches, remote branches, tags.
std::expected<std::map<std::string, std::string>, error> decorations(std::string repo);
```
Definition next to `branches()` (~1679), following its RAII idiom exactly:
```cpp
std::expected<std::map<std::string, std::string>, error> decorations(std::string path)
{
    detail::init_guard guard;
    git_repository *raw_repo = nullptr;
    if (git_repository_open_ext(&raw_repo, path.c_str(), 0, nullptr) != 0)
        return std::unexpected(last_error());
    detail::repo_ptr repo(raw_repo);

    // Collect per-oid segments grouped by kind so composition order is
    // head-branch, locals, remotes, tags regardless of iterator order.
    struct segs { std::string head; std::vector<std::string> local, remote, tag; };
    std::map<std::string, segs> by_oid;

    // HEAD: symbolic -> remember the checked-out branch; detached -> "HEAD".
    std::string head_branch;
    {
        git_reference *raw_head = nullptr;
        if (git_repository_head(&raw_head, repo.get()) == 0) {
            detail::ref_ptr head(raw_head);
            const git_oid *o = git_reference_target(head.get());
            if (git_repository_head_detached(repo.get()) == 1) {
                if (o != nullptr) {
                    char hex[GIT_OID_HEXSZ + 1] = {0};
                    git_oid_fmt(hex, o);
                    by_oid[hex].head = "HEAD";
                }
            } else if (const char *sh = git_reference_shorthand(head.get())) {
                head_branch = sh; // its segment renders "HEAD -> <name>" below
            }
        }
        // Unborn HEAD (empty repo): no decoration -- fall through.
    }

    // Branches, local + remote.
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
            const char *sh = git_reference_shorthand(ref.get());
            git_reference *raw_res = nullptr;
            if (sh == nullptr || git_reference_resolve(&raw_res, ref.get()) != 0)
                continue; // symbolic remote HEAD (origin/HEAD) etc.: skip
            detail::ref_ptr res(raw_res);
            const git_oid *o = git_reference_target(res.get());
            if (o == nullptr)
                continue;
            char hex[GIT_OID_HEXSZ + 1] = {0};
            git_oid_fmt(hex, o);
            if (type == GIT_BRANCH_LOCAL && sh == head_branch)
                by_oid[hex].head = std::string("HEAD -> ") + sh;
            else if (type == GIT_BRANCH_LOCAL)
                by_oid[hex].local.emplace_back(sh);
            else
                by_oid[hex].remote.emplace_back(sh);
        }
        if (rc != GIT_ITEROVER)
            return std::unexpected(last_error());
    }

    // Tags, peeled to the commit (annotated tags decorate the tagged commit).
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
                continue; // tag of a non-commit (blob/tree): skip
            std::unique_ptr<git_object,
                decltype([](git_object *o) { git_object_free(o); })> obj(raw_obj);
            char hex[GIT_OID_HEXSZ + 1] = {0};
            git_oid_fmt(hex, git_object_id(obj.get()));
            by_oid[hex].tag.emplace_back(std::string("tag: ") + sh);
        }
        if (rc != GIT_ITEROVER)
            return std::unexpected(last_error());
    }

    // Compose: head-branch first, then locals, remotes, tags.
    std::map<std::string, std::string> out;
    for (auto &[oid, s] : by_oid) {
        std::string dec = s.head;
        for (auto *group : {&s.local, &s.remote, &s.tag})
            for (auto &name : *group) {
                if (!dec.empty())
                    dec += ", ";
                dec += name;
            }
        if (!dec.empty())
            out.emplace(oid, std::move(dec));
    }
    return out;
}
```
(`<map>` is already imported via the module's std imports — verify; the file already uses `std::map` elsewhere. If `detail::ref_ptr` or the deleter idiom differs, match the file's actual pattern.)

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`. Pre-existing `log_query` tests asserting exact HEAD-row text may now see decoration — update ONLY those expectations (report which); undecorated-row expectations must pass unchanged.

- [ ] **Step 5: Commit**
```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(log): %d decoration in log_query + engine decorations() map"
```

---

### Task 2: Bridge — decorate the libgit2 log paths

**Files:**
- Modify: `src/magit/bridge.cpp` (`mg_magit_log_buffer` ~982; `mg_magit_log_file_buffer` ~1060)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Consumes: Task 1's `mg::git::decorations()`.
- Produces: the default (`l l`) and per-file (`l f`) log rows carry the same `<short> (<dec>) <summary>` shape as the CLI path.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_bridge.cpp` (emit-callback idiom as in the neighboring log tests):
```cpp
TEST_CASE("mg_magit_log_buffer decorates the HEAD row (libgit2 path)")
{
    auto dir = make_repo_with_commit("c0");
    std::string d = dir.string();
    std::vector<std::string> lines;
    int n = mg_magit_log_buffer(d.c_str(), 10,
        [](void *c, const char *l, int kind, const char *, int) {
            if (kind == MG_LINE_COMMIT)
                static_cast<std::vector<std::string>*>(c)->push_back(l);
        }, &lines);
    REQUIRE(n >= 1);
    REQUIRE(!lines.empty());
    // The tip row is decorated by the branch ref via the decorations() map --
    // this is the DEFAULT log path (libgit2), not the CLI %d path.
    CHECK(lines.front().find("(HEAD -> ") != std::string::npos);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (the libgit2 path composes `short + " " + summary`, no decoration).

- [ ] **Step 3: Decorate both compositions**

`mg_magit_log_buffer` (~982) — fetch the map once, look up per row:
```cpp
    int n = 0;
    auto commits = mg::git::recent_commits(repo_path, n_commits);
    if (!commits)
        return 0;
    // FM-LOG-RICH: same "(refs)" decoration the CLI %d path shows.
    auto decs = mg::git::decorations(repo_path);
    for (const auto &c : *commits) {
        std::string text = c.short_oid + " ";
        if (decs) {
            auto it = decs->find(c.oid);
            if (it != decs->end())
                text += "(" + it->second + ") ";
        }
        text += c.summary;
        emit(ctx, text.c_str(), MG_LINE_COMMIT, c.oid.c_str(), -1);
        ++n;
    }
    return n;
```
`mg_magit_log_file_buffer` (~1060): the identical transformation on its loop (map fetched once before the loop, same `text` composition around `mg::git::log_file`'s commits).

- [ ] **Step 4: Run → PASS.** Full suite. (The async `l f` path replays this function's captured lines, so it inherits the decoration — no extra change.)

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(log): decorate the libgit2 log paths via decorations()"
```

---

### Task 3: Editor — color the decoration in `*magit-log*`

**Files:**
- Modify: `src/magit_cmd.c` (`magit_log_bp` static; `magit_log_build` ~1566; `magit_is_color_buffer` ~1690; `magit_cell_color` ~1704; the new parser function)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: the row text shape `<graph art><short> (<refs>) <summary>` from Tasks 1–2.

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp`:
```cpp
TEST_CASE("*magit-log* shows colored ref decoration on the HEAD row")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool decorated = false, colored = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2); // C-x 1
            (void)!::write(master, "ll", 2);       // l l: open *magit-log*
            // Branch name varies (master/main) -- assert the invariant prefix.
            decorated = wait_for(master, "(HEAD -> ", std::chrono::seconds(8));
            // ttfgkind writes fixed SGR strings; kind 4 (cyan) starts exactly
            // at the H of HEAD, so the stream contains ESC[36m then HEAD.
            if (decorated)
                colored = wait_for(master, "\x1b[36mHEAD", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(decorated); // the log row carries (HEAD -> ...
    CHECK(colored);   // ...and the colorizer painted HEAD cyan
}
```
Caveat on `colored`: the SGR bytes arrive with the same repaint as the text, and both `wait_for` calls accumulate from their call point — if the strict `"\x1b[36mHEAD"` wait proves racy (bytes consumed by the first wait), restructure to ONE `drain_str(master, std::chrono::seconds(2))` after sending `ll`, then search that accumulated string for BOTH needles. Try the two-wait form first; keep whichever is reliable on BOTH macOS and Alpine.

- [ ] **Step 2: Run → FAIL** (`decorated` passes — Tasks 1–2 shipped the text — but `colored` fails: `*magit-log*` is not a color buffer).

- [ ] **Step 3: Implement the log colorizer**

**(a)** Add the cached buffer pointer next to `magit_status_bp`/`magit_commit_bp` (~209's comment block):
```c
static struct buffer	*magit_log_bp;		/* *magit-log*: decoration coloring */
```
**(b)** In `magit_log_build` (~1566), right after the `bclear(bp)` success check:
```c
	magit_log_bp = bp;		/* gate display.c's color hook on this */
	magit_cell_color_reset();	/* freed lines may be reused; drop memo */
```
**(c)** In `magit_is_color_buffer` (~1690), add the pointer to the OR:
```c
	return (bp != NULL && (bp == magit_status_bp || bp == magit_commit_bp ||
	    bp == magit_log_bp ||
	    bp == magit_ediff_merged_bp || bp == magit_ediff_ours_bp ||
	    bp == magit_ediff_theirs_bp));
```
**(d)** The decoration parser, above `magit_cell_color` (KNF: decls at block top; `<ctype.h>` — verify it's included, add if not):
```c
/* Ref-decoration classifier for a *magit-log* row: find the "(refs)" group
 * that immediately follows the short-oid token, validate every comma-separated
 * segment against the ref grammar, and fill kindcol[start..end) with palette
 * kinds (HEAD/-> = 4 cyan, tag = 5 yellow, remote = 1 blue, local = 2 green).
 * Returns 1 if the line got colors, 0 to leave it uncolored. Any grammar
 * violation aborts coloring entirely -- cosmetic-only, so never color a
 * summary that merely looks paren-ish. */
static int
magit_log_decor_fill(const char *text, int len)
{
	int	i = 0, tok, oidlen, start, end, j;

	/* Skip graph art: scan to the first token of 7-12 hex chars. Graph
	 * glyphs (*, |, /, \) never parse as hex. */
	while (i < len) {
		while (i < len && text[i] == ' ')
			i++;
		tok = i;
		while (i < len && text[i] != ' ')
			i++;
		oidlen = i - tok;
		if (oidlen >= 7 && oidlen <= 12) {
			for (j = tok; j < i; j++)
				if (!isxdigit((unsigned char)text[j]))
					break;
			if (j == i)
				break;			/* the oid token */
		}
		if (oidlen == 1 && (text[tok] == '*' || text[tok] == '|' ||
		    text[tok] == '/' || text[tok] == '\\'))
			continue;			/* graph art: keep scanning */
		return (0);				/* connector/odd line */
	}
	if (i + 1 >= len || text[i] != ' ' || text[i + 1] != '(')
		return (0);				/* no decoration group */
	start = i + 1;					/* the '(' */
	end = start + 1;
	while (end < len && text[end] != ')')
		end++;
	if (end >= len)
		return (0);				/* unterminated */

	/* Validate + classify the comma-separated segments inside the parens. */
	i = start + 1;
	while (i < end) {
		int	seg = i, segend = i, k, kind;

		while (segend < end && text[segend] != ',')
			segend++;
		if (segend - seg >= 8 && strncmp(text + seg, "HEAD -> ", 8) == 0) {
			for (k = seg; k < seg + 7 && k < 1024; k++)
				magit_color_kindcol[k] = 4;	/* "HEAD ->" cyan */
			seg += 8;				/* target classified below */
			kind = memchr(text + seg, '/',
			    (size_t)(segend - seg)) != NULL ? 1 : 2;
		} else if (segend - seg == 4 &&
		    strncmp(text + seg, "HEAD", 4) == 0) {
			kind = 4;				/* detached HEAD */
		} else if (segend - seg > 5 &&
		    strncmp(text + seg, "tag: ", 5) == 0) {
			kind = 5;				/* whole "tag: name" yellow */
		} else {
			kind = memchr(text + seg, '/',
			    (size_t)(segend - seg)) != NULL ? 1 : 2;
		}
		for (k = seg; k < segend; k++) {
			unsigned char c = (unsigned char)text[k];

			if (!(isalnum(c) || c == '_' || c == '-' || c == '/' ||
			    c == '.' || c == ':' || c == ' '))
				goto reject;		/* not a ref char */
		}
		for (k = seg; k < segend && k < 1024; k++)
			magit_color_kindcol[k] = (uint8_t)kind;
		i = segend;
		if (i < end && text[i] == ',')
			i++;
		if (i < end && text[i] == ' ')
			i++;
	}
	return (1);
reject:
	memset(magit_color_kindcol, 0, sizeof(magit_color_kindcol));
	return (0);
}
```
**(e)** In `magit_cell_color`'s first-cell block (`if (lp != magit_color_cached_lp) {`), add the log branch FIRST — before the `if (bp == magit_status_bp)` chain (both of its paths return, so the diff flow below is untouched; `magit_log_bp` never aliases the other pointers):
```c
		if (bp == magit_log_bp) {
			/* *magit-log*: color the ref decoration, not diff
			 * syntax. Same memo pattern as the diff path: parse
			 * once on the first cell, O(1) lookups after.
			 * cached_is_diff doubles as "kindcol valid". */
			text = ltext(lp);
			len = llength(lp);
			memset(magit_color_kindcol, 0,
			    sizeof(magit_color_kindcol));
			magit_color_cached_lp = lp;
			magit_color_cached_path = NULL;
			magit_color_cached_is_diff =
			    magit_log_decor_fill(text, len);
			if (!magit_color_cached_is_diff)
				return (0);
			return (ci >= 0 && ci < 1024) ?
			    magit_color_kindcol[ci] : 0;
		}
```
(`text`/`len` are already declared at the function top.)

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(log): color ref decorations in *magit-log*"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-logrich .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: `l l` in a repo with branches/tags — the HEAD row shows `(HEAD -> master, tag: …)` with HEAD cyan, branch green, tag yellow; a filtered log (`l` + `--all`, `l l`) decorates the same way; graph mode still colors; a commit summary starting with parenthesized text is not mis-colored.
- [ ] Update `todo.md`: mark FM-LOG-RICH's decoration done; correct the stale `--all`-graph claim; record the deferred per-file pickaxe UX + decorate-toggle as `[ ]` items.
- [ ] Open the PR based on `neomg`.
