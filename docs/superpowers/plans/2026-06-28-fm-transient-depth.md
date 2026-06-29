# FM-TRANSIENT-DEPTH (log slice) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--author=` / `--grep=` / `--all` infixes to the `l` log transient (on top of the existing `-n`), filtering `*magit-log*`.

**Architecture:** Extend the existing git-CLI log path. `log_options` gains three fields; `log_query` appends `--author=`/`--grep=`/`--all` to the `git log` argv; the C ABI grows three params; the editor adds three `log_infixes` entries and `magit_log_build` reads them straight from `log_infixes[]`. The infixes are **sticky** (persist across `magit_log_query_reset`, like `-n`) and **clearable** (empty input in the transient unsets a VALUE infix — a new `clear_on_empty` flag on the infix struct, so `-n` keeps its "empty keeps 100" behavior).

**Tech Stack:** `git log` argv via `detail::run_git`; the existing `magit_transient` infix mechanism.

## Global Constraints

- The three filters default to **inactive** → with no infix set, the log is byte-identical to today.
- Empty `--author=`/`--grep=` ⇒ append NOTHING (never a bare `--author=`).
- **Sticky, not reset:** the new infixes are read straight from `log_infixes[]` in `magit_log_build`. Do NOT add them to `magit_log_query_reset` (it clears `graph`/`range`/`pickaxe`, which are action-set and per-invocation; the infixes persist like `-n`). The spec's line-68 "clear them in query_reset" is WRONG — `query_reset` runs inside the log action AFTER the transient sets the infixes but BEFORE `magit_log_build` reads them, so clearing there would wipe the filters before they apply.
- **Clearable:** the user unsets a VALUE infix by entering empty input in the transient (`clear_on_empty == 1`). Do NOT change the existing `-n`→`magit_log_limit` sync, and `-n` keeps `clear_on_empty == 0` (empty keeps "100").
- **File + filter combine:** when routing to the query path, pass `magit_log_file_path` (not `NULL`) so a sticky `--author`/`--grep` plus a per-file log (`l f`) yields `git log --author=X -- file`, not a silently file-less log.
- `log_infixes` order after this slice: `[0]=-n`, `[1]=--author`, `[2]=--grep`, `[3]=--all`. `magit_log_build` reads `[1].value`, `[2].value`, `[3].on`.
- Infix keys: `'a'` (author), `'m'` (grep), `'A'` (all) — none collide with the log menu's action keys (`G`/`f`/`g`/`h`/`l`/`r`/`s`) or `-n`'s `'n'`.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine — `log_options` + args

**Files:**
- Modify: `src/magit/git.cppm` (`log_options` struct + `log_query` args builder)
- Test: `tests/test_git.cpp` (helpers: `make_repo_with_commit`, `commit_file`, `set_test_config`; `log_query` returns `std::expected<std::vector<log_row>, error>`; `log_row{ std::string text; std::string oid; }`)

**Interfaces:**
- Produces (Task 2 consumes): `mg::git::log_options` gains `std::string author; std::string grep; bool all = false;`; `log_query` honors them.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_git.cpp` (ensure `#include <cstdlib>` and `#include <fstream>` are present — `commit_file` already uses `<fstream>`):
```cpp
TEST_CASE("log_query --grep filters by commit message")
{
    auto dir = make_repo_with_commit("base"); // a.txt, by "Test"
    set_test_config(dir);
    commit_file(dir, "b.txt", "bee\n", "FIXBUG in parser");
    commit_file(dir, "c.txt", "cee\n", "unrelated change");

    mg::git::log_options opts; opts.grep = "FIXBUG";
    auto rows = mg::git::log_query(dir.string(), opts);
    REQUIRE(rows.has_value());
    REQUIRE_FALSE(rows->empty());
    for (const auto &r : *rows)
        CHECK(r.text.find("FIXBUG") != std::string::npos); // every match has the term
    fs::remove_all(dir);
}

TEST_CASE("log_query --author filters by author")
{
    auto dir = make_repo_with_commit("base"); // a.txt, by "Test"
    set_test_config(dir);
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    // A second commit authored by someone other than "Test".
    REQUIRE(run("-c user.name='Zoe Zed' -c user.email=z@example.com "
                "commit --allow-empty -m zed_work") == 0);

    mg::git::log_options za; za.author = "Zoe";
    auto z = mg::git::log_query(d, za);
    REQUIRE(z.has_value());
    CHECK(z->size() == 1);
    for (const auto &r : *z) CHECK(r.text.find("zed_work") != std::string::npos);

    mg::git::log_options ta; ta.author = "Test";
    auto t = mg::git::log_query(d, ta);
    REQUIRE(t.has_value());
    CHECK_FALSE(t->empty());
    for (const auto &r : *t) CHECK(r.text.find("zed_work") == std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("log_query --all includes a commit on a non-HEAD branch")
{
    auto dir = make_repo_with_commit("on main"); // HEAD on its default branch
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("checkout -b side") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m SIDE_ONLY_COMMIT") == 0);
    REQUIRE(run("checkout -") == 0); // back to the original branch; side commit now off-HEAD

    auto has_side = [](const auto &rows) { // generic: avoids naming the row type
        for (const auto &r : rows)
            if (r.text.find("SIDE_ONLY_COMMIT") != std::string::npos) return true;
        return false;
    };
    mg::git::log_options head; // HEAD-only
    auto h = mg::git::log_query(d, head);
    REQUIRE(h.has_value());
    CHECK_FALSE(has_side(*h));

    mg::git::log_options all; all.all = true;
    auto a = mg::git::log_query(d, all);
    REQUIRE(a.has_value());
    CHECK(has_side(*a));
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (`log_options` has no `author`/`grep`/`all`).
Run: `cmake --build build --target test_git && ./build/tests/test_git`
Expected: compile error / FAIL.

- [ ] **Step 3: Add the fields + args**

`src/magit/git.cppm` — extend `log_options` (struct at git.cppm:532):
```cpp
struct log_options {
    std::size_t max_count = 0;   // 0 = no -n limit
    bool        graph     = false;
    std::string range;
    std::string file;
    char        pickaxe   = 0;
    std::string pickaxe_term;
    std::string author;          // --author=<x>  ("" = none)
    std::string grep;            // --grep=<x>     ("" = none)
    bool        all       = false; // --all
};
```
In `log_query` (git.cppm:~1561), after the existing `-S`/`-G` appends and before the `--format=` arg, add:
```cpp
    if (opts.all)
        args.emplace_back("--all");
    if (!opts.author.empty())
        args.emplace_back("--author=" + opts.author);
    if (!opts.grep.empty())
        args.emplace_back("--grep=" + opts.grep);
```

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(log): log_options author/grep/all + git log args"
```

---

### Task 2: Bridge — `mg_magit_log_query_buffer` gains 3 params

**Files:**
- Modify: `src/magit/bridge.h` (decl), `src/magit/bridge.cpp` (impl, ~bridge.cpp:1080), `src/magit_cmd.c` (the ONE existing caller in `magit_log_build`, ~1517 — pass defaults to keep the build green)
- Test: `tests/test_bridge.cpp` (helpers: `make_repo_full`, `make_repo_with_commit`; emit sig `(void *ctx, const char *line, int kind, const char *path, int hunk)`; `MG_LINE_COMMIT` == 9)

**Interfaces:**
- Consumes: `mg::git::log_options` author/grep/all (Task 1).
- Produces (Task 3 consumes): `mg_magit_log_query_buffer(repo, graph, range, file, pickaxe_kind, pickaxe_term, n, const char *author, const char *grep, int all, emit, ctx)`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_bridge.cpp` (ensure `#include <cstdlib>` / `#include <fstream>` present):
```cpp
TEST_CASE("mg_magit_log_query_buffer threads --grep through")
{
    auto dir = make_repo_full();
    std::string d = dir.string();
    REQUIRE(std::system(("git -C '" + d +
        "' commit --allow-empty -m FIND_ME_GREP >/dev/null 2>&1").c_str()) == 0);

    struct row { std::string line; };
    std::vector<row> rows;
    mg_magit_log_query_buffer(d.c_str(), 0, nullptr, nullptr, 0, nullptr, 50,
        nullptr, "FIND_ME_GREP", 0,      // author=NULL, grep set, all=0
        [](void *c, const char *l, int kind, const char *, int) {
            if (kind == MG_LINE_COMMIT)
                static_cast<std::vector<row>*>(c)->push_back({l});
        }, &rows);
    REQUIRE_FALSE(rows.empty());
    for (const auto &r : rows) CHECK(r.line.find("FIND_ME_GREP") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_log_query_buffer applies --author together with a file path")
{
    auto dir = make_repo_with_commit("base"); // a.txt, by "Test"
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    std::ofstream(dir / "a.txt") << "content\nzoe line\n"; // second commit to a.txt, by Zoe
    REQUIRE(run("add a.txt") == 0);
    REQUIRE(run("-c user.name=Zoe -c user.email=z@example.com commit -m zoe_edit") == 0);

    auto count = [&](const char *author) {
        int n = 0;
        mg_magit_log_query_buffer(d.c_str(), 0, nullptr, "a.txt", 0, nullptr, 50,
            author, nullptr, 0,
            [](void *c, const char *, int kind, const char *, int) {
                if (kind == MG_LINE_COMMIT) (*static_cast<int*>(c))++;
            }, &n);
        return n;
    };
    CHECK(count(nullptr) == 2); // both commits touch a.txt (file path applied)
    CHECK(count("Zoe") == 1);   // author AND file both applied (guards the file-passing fix)
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (wrong arg count).

- [ ] **Step 3: Change the decl, impl, and the existing caller**

`src/magit/bridge.h` — update the decl:
```c
int mg_magit_log_query_buffer(const char *repo_path, int graph,
    const char *range, const char *file, int pickaxe_kind,
    const char *pickaxe_term, int n,
    const char *author, const char *grep, int all,
    mg_magit_emit_fn emit, void *ctx);
```
`src/magit/bridge.cpp` (~bridge.cpp:1080) — add the params + set opts (keep the existing body, only the signature and the opts-building lines change):
```cpp
extern "C" int mg_magit_log_query_buffer(const char *repo_path, int graph,
                                         const char *range, const char *file,
                                         int pickaxe_kind,
                                         const char *pickaxe_term, int n,
                                         const char *author, const char *grep,
                                         int all,
                                         mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;
    mg::git::log_options opts;
    opts.graph = graph != 0;
    opts.max_count = n > 0 ? static_cast<std::size_t>(n) : 0;
    if (range != nullptr) opts.range = range;
    if (file != nullptr)  opts.file = file;
    if (pickaxe_kind == 'S' || pickaxe_kind == 'G') {
        opts.pickaxe = static_cast<char>(pickaxe_kind);
        opts.pickaxe_term = pickaxe_term ? pickaxe_term : "";
    }
    if (author != nullptr) opts.author = author;
    if (grep != nullptr)   opts.grep = grep;
    opts.all = all != 0;
    // … rest unchanged (log_query + the emit loop) …
}
```
`src/magit_cmd.c` — the ONE existing caller (`magit_log_build`, ~1517) must keep compiling: insert `NULL, NULL, 0,` before `magit_log_emit, bp` (Task 3 replaces these with real infix reads and the file-passing fix):
```c
        (void)mg_magit_log_query_buffer(cwd, magit_log_graph,
            magit_log_range[0] ? magit_log_range : NULL, NULL,
            magit_log_pickaxe, magit_log_pickaxe_term,
            magit_log_limit,
            NULL, NULL, 0,                 /* author/grep/all — wired in Task 3 */
            magit_log_emit, bp);
```

- [ ] **Step 4: Run → PASS.** Full suite + OFF: `cmake --build build && ctest --test-dir build --output-on-failure` ; `cmake --build build-tsan --target test_bridge && ./build-tsan/tests/test_bridge`.

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.h src/magit/bridge.cpp src/magit_cmd.c tests/test_bridge.cpp
git commit -m "feat(log): mg_magit_log_query_buffer author/grep/all params"
```

---

### Task 3: Editor — clearable infixes + `log_infixes` + `magit_log_build` wiring

**Files:**
- Modify: `src/magit_cmd.c` (`magit_infix` struct + `clear_on_empty`; `magit_transient` VALUE branch; `push_infixes` + `log_infixes`; `magit_log_build` routing + file-fix)
- Test: `tests/test_editor.cpp` (helpers: `make_repo`, `wait_for`, `NEOMG_BINARY`, `forkpty`; no `sh` helper — use `std::system`)

**Interfaces:**
- Consumes: the Task 2 C ABI; the existing `magit_transient`/`log_infixes`/`magit_logmenu`.

The struct/transient/infix-array edits (Steps 2–5) are mechanical and must land before the pty tests can pass; write the failing pty tests first (Step 1) so the task is TDD-anchored, then make them pass.

- [ ] **Step 1: Write the failing pty tests**

Add to `tests/test_editor.cpp`. Two end-to-end tests — `--all` (FLAG infix) and `--grep` set-then-clear (VALUE infix + `clear_on_empty`). Both assert on **presence** (robust). Use the file's established robustness pattern: `C-x 1` (single window), forced repaint (`\x0c` = C-l), generous `wait_for` timeouts.

```cpp
TEST_CASE("l transient --all includes a non-HEAD commit in *magit-log*")
{
    auto repo = make_repo(); // tracked.txt committed
    std::string d = repo.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("checkout -b side") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m SIDEONLY_COMMIT") == 0);
    REQUIRE(run("checkout -") == 0);
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool with_all = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "l", 1);           // open the log transient
            if (wait_for(master, "--all", std::chrono::seconds(8))) { // infix listed
                (void)!::write(master, "A", 1);       // toggle --all
                (void)!::write(master, "l", 1);       // l l: run the log
                (void)!::write(master, "\x0c", 1);    // force repaint
                with_all = wait_for(master, "SIDEONLY_COMMIT", std::chrono::seconds(8));
            }
        }
    }
    (void)!::write(master, "\x18\x03", 2); // C-x C-c
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(with_all);
}

TEST_CASE("l transient --grep filters, then empty input clears it")
{
    auto repo = make_repo(); // tracked.txt committed
    std::string d = repo.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    // Two extra commits with distinct, greppable summaries.
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m AAAA_FIRST") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@example.com "
                "commit --allow-empty -m BBBB_SECOND") == 0);
    const std::string repofile = (repo / "tracked.txt").string();

    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr);
        _exit(127);
    }
    bool filtered = false, cleared = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);            // C-x 1
            (void)!::write(master, "l", 1);                    // transient
            if (wait_for(master, "--grep", std::chrono::seconds(8))) {
                (void)!::write(master, "m", 1);                // --grep infix
                (void)!::write(master, "BBBB\r", 5);           // set grep=BBBB
                (void)!::write(master, "l", 1);                // run
                (void)!::write(master, "\x0c", 1);
                filtered = wait_for(master, "BBBB_SECOND", std::chrono::seconds(8));
                // Now clear it: empty input unsets --grep, so AAAA reappears.
                (void)!::write(master, "l", 1);                // transient again
                if (wait_for(master, "--grep", std::chrono::seconds(8))) {
                    (void)!::write(master, "m", 1);            // --grep
                    (void)!::write(master, "\r", 1);           // empty -> clear
                    (void)!::write(master, "l", 1);            // run
                    (void)!::write(master, "\x0c", 1);
                    cleared = wait_for(master, "AAAA_FIRST", std::chrono::seconds(8));
                }
            }
        }
    }
    (void)!::write(master, "\x18\x03", 2);
    for (int i = 0; i < 20; ++i) { int st = 0; if (::waitpid(pid, &st, WNOHANG) == pid) break; usleep(100000); }
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(filtered); // grep applied (BBBB shown)
    CHECK(cleared);  // grep cleared (AAAA reappears — only possible if the BBBB filter was unset)
}
```
Run → FAIL (`--all`/`--grep` infixes don't exist; `A`/`m` do nothing).

- [ ] **Step 2: Add `clear_on_empty` to the infix struct**

`src/magit_cmd.c` — `struct magit_infix` (line ~38), append a field:
```c
struct magit_infix {
	KCHAR			 key;
	const char		*arg;	/* shown label, e.g. "-f --force" */
	enum magit_infix_kind	 kind;
	int			 on;	/* FLAG: 0/1 toggle state */
	char			 value[64];	/* VALUE: current text */
	int			 clear_on_empty; /* VALUE: empty input unsets (vs keeps) */
};
```

- [ ] **Step 3: Make the transient honor `clear_on_empty`**

`src/magit_cmd.c` — `magit_transient` VALUE branch (lines ~938–951), replace:
```c
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
```
with:
```c
			} else {
				/* Non-empty input replaces the value. Empty input
				 * unsets a clear_on_empty infix (--author/--grep) or
				 * keeps the current value otherwise (-n's limit). */
				val[0] = '\0';
				if (eread("%s (%s): ", val, sizeof(val),
				    EFNEW | EFCR, (char *)m->infixes[i].arg,
				    m->infixes[i].value) != NULL) {
					if (val[0] != '\0')
						(void)strlcpy(m->infixes[i].value,
						    val,
						    sizeof(m->infixes[i].value));
					else if (m->infixes[i].clear_on_empty)
						m->infixes[i].value[0] = '\0';
				}
				if (m == &log_menu)
					magit_log_limit = (int)strtol(
					    log_infixes[0].value, NULL, 10);
			}
```

- [ ] **Step 4: Update the infix arrays for the new field**

`src/magit_cmd.c` — `push_infixes` (line ~790), add the explicit trailing field (FLAG infixes never clear → `0`), avoiding `-Wmissing-field-initializers`:
```c
static struct magit_infix push_infixes[] = {
	{ 'f', "-f --force-with-lease", MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 'u', "-u --set-upstream",     MAGIT_INFIX_FLAG, 0, "", 0 }
};
```
`src/magit_cmd.c` — `log_infixes` (line ~795), replace the single `-n` entry with four (author/grep are clearable; `-n` and the `--all` FLAG are not):
```c
static struct magit_infix log_infixes[] = {
	{ 'n', "-n --max-count", MAGIT_INFIX_VALUE, 0, "100", 0 },
	{ 'a', "--author",       MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'm', "--grep",         MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'A', "--all",          MAGIT_INFIX_FLAG,  0, "",    0 }
};
```

- [ ] **Step 5: Wire them into `magit_log_build`**

`src/magit_cmd.c` — `magit_log_build` (~1514). Extend the routing condition to fire on any new infix, read them from `log_infixes`, AND pass `magit_log_file_path` (not `NULL`) so a file log combines with a sticky filter:
```c
	magit_log_count = 0;
	if (magit_log_graph || magit_log_range[0] != '\0' ||
	    magit_log_pickaxe != '\0' ||
	    log_infixes[1].value[0] != '\0' ||   /* --author */
	    log_infixes[2].value[0] != '\0' ||   /* --grep */
	    log_infixes[3].on) {                  /* --all */
		(void)mg_magit_log_query_buffer(cwd, magit_log_graph,
		    magit_log_range[0] ? magit_log_range : NULL,
		    magit_log_file_path[0] ? magit_log_file_path : NULL,
		    magit_log_pickaxe, magit_log_pickaxe_term,
		    magit_log_limit,
		    log_infixes[1].value[0] ? log_infixes[1].value : NULL,
		    log_infixes[2].value[0] ? log_infixes[2].value : NULL,
		    log_infixes[3].on,
		    magit_log_emit, bp);
	} else if (magit_log_file_path[0] != '\0') {
		/* … per-file async path, unchanged … */
```
(The `log_infixes` indices `[1]`/`[2]`/`[3]` match Step 4's order. `magit_transient` already populates `.value`/`.on`. Do NOT touch `magit_log_query_reset` — the infixes are sticky.)

- [ ] **Step 6: Run the pty tests → PASS.** Full suite `cmake --build build && ctest --test-dir build --output-on-failure`. If a pty test is flaky, harden with the established pattern (event-waits, an extra forced repaint + drain) — do NOT weaken the assertions.

- [ ] **Step 7: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(log): sticky --author/--grep/--all infixes in the l transient"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-transient .` (the repo's established Linux gate).
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: `l` → toggle `--all` (A), set `--author=` (a) + `--grep=` (m), run `l l` → `*magit-log*` reflects the filters; re-enter `--author=`/`--grep=` empty → filter clears; the filters persist across a plain `l l` (sticky); `l f` with a sticky `--author` shows that author's commits to the file; with no infix set the log is byte-unchanged; `-n` still limits.
- [ ] Update `todo.md`: mark FM-TRANSIENT-DEPTH (log slice) `[x]`; confirm the deferred slices (FM-TRANSIENT-PUSH/DIFF/LOG-MORE) remain recorded.
- [ ] Open the PR (stacked on `fm-git-cmd`/#97 until it merges, then re-target `neomg`).
