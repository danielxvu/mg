# FM-LP Log Extras Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--graph`, commit-range, and pickaxe (`-S`/`-G`) modes to mg's native-magit log buffer.

**Architecture:** One new CLI-backed `mg::git::log_query` (reuses `detail::run_git` from FM-GIT-CLI-WRITES) parses `git log [--graph] [-S|-G] [<range>] --format=%x1f%H%x1f%h%x1f%s` into the existing `MG_LINE_COMMIT` row model; graph connector lines come through as `MG_LINE_OTHER` (no oid). `l l`/`l f` and the status buffer's inline "Recent commits" stay on libgit2 (add-alongside). The bridge and UI reuse the existing `*magit-log*` buffer and `magit-log-mode` keymap, so act-at-point (RET/V/T/g) works unchanged.

**Tech Stack:** C++23 module `mg.git` (libgit2 + `posix_spawnp`), the `extern "C"` bridge (`bridge.cpp`/`bridge.h`), the C UI (`magit_cmd.c`), doctest tests, CMake presets `cpp` (ON) / `c-legacy` (OFF), Alpine Docker.

## Global Constraints

- All new code is gated by `#ifdef ENABLE_NATIVE_MAGIT`; the OFF build (`c-legacy`) must keep **0 magit symbols** (`nm ./build-c/src/mg | grep -ic magit` == 0).
- Engine code lives in `src/magit/git.cppm` (module `mg.git`); the subprocess helper `detail::run_git(const std::string&, const std::vector<std::string>&) -> {int code; std::string output;}` already exists there.
- Magit keymap entries MUST stay in **ascending key order** — `magit_assert_keymap_sorted()` panics on first `C-x g` otherwise.
- Build (ON): `cmake --build --preset cpp`; tests: `ctest --preset cpp` (binaries under `build/`, e.g. `./build/tests/test_git`). Single test: `./build/tests/test_git --test-case="NAME"`.
- Cross-platform gate per task that touches the engine/bridge: macOS `ctest --preset cpp` green; Alpine `docker build -f docker/Dockerfile.alpine -t mg-alpine .` green.
- Commit message trailer (every commit):
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp
  ```
- Existing test helpers in `tests/test_git.cpp`: `make_temp_dir()`, `commit_file(const fs::path& dir, const char* name, const std::string& body, const char* msg)`, `make_repo_with_commit(const char* msg)`, `make_repo_for_interactive()` (branch `feature`: C1→C2→C3→C4, `master` = C1), `set_test_config(dir)`.

---

## File Structure

- `src/magit/git.cppm` — add `struct log_row`, `struct log_options`, and `log_query()` (declaration near the other log decls ~line 446; definition near `recent_commits`/`log_file` ~line 1092).
- `tests/test_git.cpp` — engine tests for `log_query`.
- `src/magit/bridge.h` — declare `mg_magit_log_query_buffer`.
- `src/magit/bridge.cpp` — define `mg_magit_log_query_buffer` (near `mg_magit_log_buffer` ~line 883).
- `tests/test_bridge.cpp` — bridge test.
- `src/magit_cmd.c` — module-static query state + `magit_log_query_reset`, four commands (`magit_log_graph_cmd`, `magit_log_range_cmd`, `magit_log_pickaxe_s`, `magit_log_pickaxe_g`), dispatch in `magit_log_build`, reset in `magit_log`/`magit_log_file`, and `magit_logmenu` entries.

---

## Task 1: Engine `log_query` (graph / range / pickaxe)

**Files:**
- Modify: `src/magit/git.cppm` (add types + declaration ~line 446; definition after `log_file` ~line 1130)
- Test: `tests/test_git.cpp`

**Interfaces:**
- Consumes: `detail::run_git(const std::string& repo, const std::vector<std::string>& args) -> detail::git_run{int code; std::string output;}` (already in the module).
- Produces:
  ```cpp
  namespace mg::git {
  struct log_row { std::string text; std::string oid; }; // oid == "" => connector line
  struct log_options {
      std::size_t max_count = 0;   // 0 = no -n
      bool        graph     = false;
      std::string range;           // "" = HEAD default
      std::string file;            // "" = repo-wide
      char        pickaxe   = 0;   // 0 / 'S' / 'G'
      std::string pickaxe_term;
  };
  std::expected<std::vector<log_row>, error>
  log_query(std::string repo, log_options opts);
  }
  ```

- [ ] **Step 1: Add the type + declaration**

In `src/magit/git.cppm`, immediately after the `log_file(...)` declaration (the block ending ~line 446, just before `// Create local branch ...`), add:

```cpp
// FM-LP: a row of `git log` output. A commit row carries its full oid; a pure
// graph-connector line (--graph) has oid == "" and is non-actionable.
struct log_row {
    std::string text;   // <graph art> + short_oid + " " + summary, or a connector line
    std::string oid;    // full 40-hex sha for a commit row; "" otherwise
};

// Options for log_query (the CLI-backed log reader used by l g / l r / l s / l G).
struct log_options {
    std::size_t max_count = 0;   // 0 = no -n limit
    bool        graph     = false;
    std::string range;           // "" = default (HEAD); else e.g. "main..HEAD"
    std::string file;            // "" = repo-wide; else restrict to a path
    char        pickaxe   = 0;   // 0 = none, 'S' = occurrence-count, 'G' = regex
    std::string pickaxe_term;
};

// Run `git log` with the requested options and parse it into rows. graph +
// pickaxe have no libgit2 equivalent, so this goes through the real git binary
// (detail::run_git, argv array -- no shell). A non-zero git exit -> error
// carrying git's output (e.g. a bad range).
std::expected<std::vector<log_row>, error>
log_query(std::string repo, log_options opts);
```

- [ ] **Step 2: Write the failing range test**

In `tests/test_git.cpp`, after the `log_file` test (search `TEST_CASE("log_file`), add:

```cpp
TEST_CASE("log_query range returns exactly the commits in A..B")
{
    auto dir = make_repo_for_interactive(); // feature: C1->C2->C3->C4, master=C1

    mg::git::log_options opts;
    opts.range = "master..HEAD"; // C2, C3, C4
    auto rows = mg::git::log_query(dir.string(), opts);
    REQUIRE(rows.has_value());

    std::vector<std::string> summaries;
    for (const auto &r : *rows)
        if (!r.oid.empty())
            summaries.push_back(r.text); // "<short> <summary>"
    CHECK(summaries.size() == 3);
    // Newest first: C4, C3, C2 -- every row carries a non-empty oid (no graph).
    for (const auto &r : *rows)
        CHECK_FALSE(r.oid.empty());
    fs::remove_all(dir);
}
```

- [ ] **Step 3: Run it — verify it fails**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_git --test-case="log_query range*"`
Expected: link/compile error or FAIL (no `log_query` definition yet).

- [ ] **Step 4: Implement `log_query`**

In `src/magit/git.cppm`, after the `log_file(...)` *definition* (search the body `log_file(std::string path` and find its closing `}`, ~line 1130), add:

```cpp
std::expected<std::vector<log_row>, error>
log_query(std::string repo, log_options opts)
{
    std::vector<std::string> args{"log"};
    if (opts.graph)
        args.emplace_back("--graph");
    if (opts.max_count > 0) {
        args.emplace_back("-n");
        args.emplace_back(std::to_string(opts.max_count));
    }
    if (opts.pickaxe == 'S')
        args.emplace_back("-S" + opts.pickaxe_term);
    else if (opts.pickaxe == 'G')
        args.emplace_back("-G" + opts.pickaxe_term);
    // Leading %x1f so --graph's art lands in field[0] and the same parser
    // handles graph + non-graph lines uniformly.
    args.emplace_back("--format=%x1f%H%x1f%h%x1f%s");
    if (!opts.range.empty())
        args.emplace_back(opts.range);
    if (!opts.file.empty()) {
        args.emplace_back("--");
        args.emplace_back(opts.file);
    }

    auto run = detail::run_git(repo, args);
    if (run.code != 0) {
        std::string msg = run.output.empty() ? "git log failed" : run.output;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
            msg.pop_back();
        return std::unexpected(error{0, std::move(msg)});
    }

    std::vector<log_row> rows;
    const std::string &out = run.output;
    std::size_t start = 0;
    while (start <= out.size()) {
        std::size_t nl = out.find('\n', start);
        std::string line =
            out.substr(start, nl == std::string::npos ? std::string::npos
                                                      : nl - start);
        if (nl == std::string::npos) {
            if (line.empty())
                break;
        }
        // Split on the US (0x1f) separator.
        std::vector<std::string> f;
        std::size_t p = 0;
        for (;;) {
            std::size_t s = line.find('\x1f', p);
            if (s == std::string::npos) {
                f.push_back(line.substr(p));
                break;
            }
            f.push_back(line.substr(p, s - p));
            p = s + 1;
        }
        if (f.size() >= 4) {
            // f[0]=graph art, f[1]=full, f[2]=short, f[3]=summary
            log_row row;
            row.text = f[0] + f[2] + " " + f[3];
            row.oid = f[1];
            rows.push_back(std::move(row));
        } else if (!line.empty()) {
            rows.push_back(log_row{line, ""}); // connector-only line
        }
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return rows;
}
```

- [ ] **Step 5: Run it — verify the range test passes**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_git --test-case="log_query range*"`
Expected: PASS.

- [ ] **Step 6: Write the pickaxe `-S` test**

In `tests/test_git.cpp`, after the range test, add:

```cpp
TEST_CASE("log_query -S finds the commits that add/remove a string")
{
    auto dir = make_repo_with_commit("base"); // a.txt = "content"
    set_test_config(dir);
    commit_file(dir, "f.txt", "alpha\nMAGIC_TOKEN_42\nbeta\n", "add token");
    commit_file(dir, "f.txt", "alpha\nbeta\n", "remove token");
    commit_file(dir, "g.txt", "unrelated\n", "noise");

    mg::git::log_options opts;
    opts.pickaxe = 'S';
    opts.pickaxe_term = "MAGIC_TOKEN_42";
    auto rows = mg::git::log_query(dir.string(), opts);
    REQUIRE(rows.has_value());

    std::vector<std::string> hits;
    for (const auto &r : *rows)
        hits.push_back(r.text);
    REQUIRE(hits.size() == 2); // the add commit and the remove commit only
    CHECK(hits[0].find("remove token") != std::string::npos); // newest first
    CHECK(hits[1].find("add token") != std::string::npos);
    fs::remove_all(dir);
}
```

- [ ] **Step 7: Run it — verify it passes (args already implemented)**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_git --test-case="log_query -S*"`
Expected: PASS.

- [ ] **Step 8: Write the `-G` regex test**

In `tests/test_git.cpp`, after the `-S` test, add:

```cpp
TEST_CASE("log_query -G matches a regex in the diff")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    commit_file(dir, "f.txt", "value = 123\n", "add numeric line");
    commit_file(dir, "g.txt", "plain text\n", "add text");

    mg::git::log_options opts;
    opts.pickaxe = 'G';
    opts.pickaxe_term = "value = [0-9]+";
    auto rows = mg::git::log_query(dir.string(), opts);
    REQUIRE(rows.has_value());

    std::vector<std::string> hits;
    for (const auto &r : *rows)
        hits.push_back(r.text);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].find("add numeric line") != std::string::npos);
    fs::remove_all(dir);
}
```

- [ ] **Step 9: Run it — verify it passes**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_git --test-case="log_query -G*"`
Expected: PASS.

- [ ] **Step 10: Write the graph test**

In `tests/test_git.cpp`, after the `-G` test, add (builds a real merge so the graph has connector lines):

```cpp
TEST_CASE("log_query graph emits connector lines and tags commits")
{
    auto dir = make_repo_with_commit("C1"); // master @ C1, a.txt
    set_test_config(dir);
    // Branch 'side', diverge, then merge it back so HEAD has a merge commit.
    {
        git_libgit2_init();
        git_repository *repo = nullptr;
        REQUIRE(git_repository_open(&repo, dir.string().c_str()) == 0);
        git_oid head;
        REQUIRE(git_reference_name_to_id(&head, repo, "HEAD") == 0);
        git_commit *tip = nullptr;
        REQUIRE(git_commit_lookup(&tip, repo, &head) == 0);
        git_reference *ref = nullptr;
        REQUIRE(git_branch_create(&ref, repo, "side", tip, 0) == 0);
        git_reference_free(ref);
        git_commit_free(tip);
        git_repository_free(repo);
        git_libgit2_shutdown();
    }
    commit_file(dir, "a.txt", "content-main\n", "C2 on master");
    // Switch to side, commit, switch back, merge.
    REQUIRE(mg::git::checkout_branch(dir.string(), "side").has_value());
    commit_file(dir, "b.txt", "content-side\n", "C2 on side");
    REQUIRE(mg::git::checkout_branch(dir.string(), "master").has_value());
    auto merged = mg::git::merge_branch(dir.string(), "side");
    REQUIRE(merged.has_value()); // clean merge -> a merge commit on master

    mg::git::log_options opts;
    opts.graph = true;
    auto rows = mg::git::log_query(dir.string(), opts);
    REQUIRE(rows.has_value());

    bool sawConnector = false, sawMerge = false;
    for (const auto &r : *rows) {
        if (r.oid.empty() && !r.text.empty())
            sawConnector = true;             // a pure | / \ line
        if (r.text.find("Merge branch 'side'") != std::string::npos)
            sawMerge = true;
    }
    CHECK(sawMerge);
    CHECK(sawConnector);
    fs::remove_all(dir);
}
```

- [ ] **Step 11: Run it — verify the graph test passes**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_git --test-case="log_query graph*"`
Expected: PASS (the parser already handles `< 4 field` connector lines).

- [ ] **Step 12: Full engine suite + cross-platform gate**

Run: `ctest --preset cpp 2>&1 | tail -3`
Expected: 100% pass.
Run: `docker build -f docker/Dockerfile.alpine -t mg-alpine . 2>&1 | grep -E "tests passed|FAILED"`
Expected: `100% tests passed`.

- [ ] **Step 13: Commit**

```bash
git add src/magit/git.cppm tests/test_git.cpp
git commit -m "feat(magit): log_query engine -- graph/range/pickaxe via real git (FM-LP)

CLI-backed log reader (run_git + %x1f-delimited parse) for the modes libgit2
cannot do (graph art, -S/-G pickaxe) plus commit-range. Returns log_row{text,
oid}; connector lines have empty oid. Tests: range A..B, -S add/remove, -G
regex, graph with a merge (connector lines + merge commit).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp"
```

---

## Task 2: Bridge `mg_magit_log_query_buffer`

**Files:**
- Modify: `src/magit/bridge.h` (after the `mg_magit_log_file_buffer` decl)
- Modify: `src/magit/bridge.cpp` (after `mg_magit_log_buffer` ~line 900)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Consumes: `mg::git::log_query`, `mg::git::log_options` (Task 1); `mg_magit_emit_fn = void(*)(void* ctx, const char* line, int kind, const char* path, int hunk)`; `MG_LINE_COMMIT` (9), `MG_LINE_OTHER` (0).
- Produces:
  ```c
  int mg_magit_log_query_buffer(const char *repo_path, int graph,
          const char *range, const char *file, int pickaxe_kind,
          const char *pickaxe_term, int n,
          mg_magit_emit_fn emit, void *ctx);
  ```

- [ ] **Step 1: Declare in `bridge.h`**

In `src/magit/bridge.h`, immediately after the `mg_magit_log_file_buffer(...)` declaration, add:

```c
/* FM-LP: CLI-backed log with graph / range / pickaxe. graph: 0/1. range: ""
 * or NULL = HEAD; else "A..B". file: "" or NULL = repo-wide. pickaxe_kind:
 * 0 none / 'S' / 'G' with pickaxe_term. n = -n limit (0 = none). Commit rows
 * emit as MG_LINE_COMMIT (path = full oid); graph connector lines emit as
 * MG_LINE_OTHER (path = NULL). Returns the line count, 0 on failure. */
int mg_magit_log_query_buffer(const char *repo_path, int graph,
        const char *range, const char *file, int pickaxe_kind,
        const char *pickaxe_term, int n,
        mg_magit_emit_fn emit, void *ctx);
```

- [ ] **Step 2: Write the failing bridge test**

In `tests/test_bridge.cpp`, after the existing log-buffer test (search `mg_magit_log_buffer`), add:

```cpp
TEST_CASE("mg_magit_log_query_buffer -S finds the introducing commit")
{
    auto dir = make_repo_with_commit("base");
    set_test_config(dir);
    commit_file(dir, "f.txt", "x\nNEEDLE_XYZ\ny\n", "introduce needle");
    commit_file(dir, "g.txt", "noise\n", "noise");

    struct cap { std::vector<std::string> lines; int others = 0; } c;
    int n = mg_magit_log_query_buffer(
        dir.string().c_str(), /*graph=*/0, /*range=*/nullptr, /*file=*/nullptr,
        /*pickaxe_kind=*/'S', /*pickaxe_term=*/"NEEDLE_XYZ", /*n=*/0,
        [](void *ctx, const char *line, int kind, const char *, int) {
            auto *p = static_cast<cap *>(ctx);
            p->lines.emplace_back(line);
            if (kind == MG_LINE_OTHER)
                p->others++;
        },
        &c);
    REQUIRE(n == 1);
    CHECK(c.lines[0].find("introduce needle") != std::string::npos);
    fs::remove_all(dir);
}
```

- [ ] **Step 3: Run it — verify it fails**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_bridge --test-case="mg_magit_log_query_buffer*"`
Expected: link/compile error (function undefined).

- [ ] **Step 4: Implement in `bridge.cpp`**

In `src/magit/bridge.cpp`, after `mg_magit_log_buffer`'s closing `}` (~line 900), add:

```cpp
extern "C" int mg_magit_log_query_buffer(const char *repo_path, int graph,
                                         const char *range, const char *file,
                                         int pickaxe_kind,
                                         const char *pickaxe_term, int n,
                                         mg_magit_emit_fn emit, void *ctx)
{
    if (repo_path == nullptr || emit == nullptr)
        return 0;

    mg::git::log_options opts;
    opts.graph = graph != 0;
    opts.max_count = n > 0 ? static_cast<std::size_t>(n) : 0;
    if (range != nullptr)
        opts.range = range;
    if (file != nullptr)
        opts.file = file;
    if (pickaxe_kind == 'S' || pickaxe_kind == 'G') {
        opts.pickaxe = static_cast<char>(pickaxe_kind);
        opts.pickaxe_term = pickaxe_term ? pickaxe_term : "";
    }

    auto rows = mg::git::log_query(repo_path, std::move(opts));
    if (!rows)
        return 0;
    int count = 0;
    for (const auto &r : *rows) {
        if (r.oid.empty())
            emit(ctx, r.text.c_str(), MG_LINE_OTHER, nullptr, -1);
        else
            emit(ctx, r.text.c_str(), MG_LINE_COMMIT, r.oid.c_str(), -1);
        ++count;
    }
    return count;
}
```

- [ ] **Step 5: Run it — verify it passes**

Run: `cmake --build --preset cpp 2>&1 | tail -3 && ./build/tests/test_bridge --test-case="mg_magit_log_query_buffer*"`
Expected: PASS.

- [ ] **Step 6: Full suite + cross-platform gate**

Run: `ctest --preset cpp 2>&1 | tail -3`
Expected: 100% pass.
Run: `docker build -f docker/Dockerfile.alpine -t mg-alpine . 2>&1 | grep -E "tests passed|FAILED"`
Expected: `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/magit/bridge.h src/magit/bridge.cpp tests/test_bridge.cpp
git commit -m "feat(magit): mg_magit_log_query_buffer bridge (FM-LP)

Wraps mg::git::log_query; commit rows -> MG_LINE_COMMIT (path = oid), graph
connector lines -> MG_LINE_OTHER (path = NULL). Test: -S pickaxe finds the
introducing commit through the bridge.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp"
```

---

## Task 3: UI — `l g` / `l r` / `l s` / `l G`

**Files:**
- Modify: `src/magit_cmd.c` (forward decls ~line 125-132; module-static state ~line 186; `magit_log`/`magit_log_file` ~line where defined; `magit_log_build` dispatch; `magit_logmenu` ~line 359)

**Interfaces:**
- Consumes: `mg_magit_log_query_buffer` (Task 2); existing `magit_log_emit`, `magit_log_limit`, `magit_log_file_path`, `magit_log_open`, `magit_log_build`, `eread(...)`, `EFNEW|EFCR`, `ABORT`.
- Produces: commands `magit_log_graph_cmd`, `magit_log_range_cmd`, `magit_log_pickaxe_s`, `magit_log_pickaxe_g`; helper `magit_log_query_reset(void)`; new `magit_logmenu` keys `g`, `r`, `s`, `G` (`l` is the menu prefix).

- [ ] **Step 1: Add forward declarations**

In `src/magit_cmd.c`, near the other `magit_log_*` forward declarations (~line 125-132), add:

```c
static int	magit_log_graph_cmd(int, int);
static int	magit_log_range_cmd(int, int);
static int	magit_log_pickaxe_s(int, int);
static int	magit_log_pickaxe_g(int, int);
static void	magit_log_query_reset(void);
```

- [ ] **Step 2: Add module-static query state**

In `src/magit_cmd.c`, near `magit_log_file_path` / `magit_log_limit` (~line 186-189), add:

```c
static int	magit_log_graph;		/* l g: --graph mode */
static char	magit_log_range[256];		/* l r: "A..B" ("" = none) */
static char	magit_log_pickaxe;		/* 0 / 'S' / 'G' */
static char	magit_log_pickaxe_term[256];	/* the -S/-G term */
```

- [ ] **Step 3: Add the reset helper and the four commands**

In `src/magit_cmd.c`, just before `magit_log(int f, int n)` (search `^magit_log(int f, int n)`), add:

```c
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
```

- [ ] **Step 4: Reset query state in the plain log commands**

In `src/magit_cmd.c`, edit `magit_log` to reset query state. Change:

```c
magit_log(int f, int n)
{
	magit_log_file_path[0] = '\0';
	return (magit_log_open(f, n));
}
```
to:
```c
magit_log(int f, int n)
{
	magit_log_query_reset();
	magit_log_file_path[0] = '\0';
	return (magit_log_open(f, n));
}
```

And in `magit_log_file`, add `magit_log_query_reset();` as the first statement of the function body (before `char *path = NULL;` — declarations may stay; place the call right after the declarations, before `kind = magit_at_point(...)`):

```c
magit_log_file(int f, int n)
{
	char	*path = NULL;
	int	 kind, hunk;

	magit_log_query_reset();
	kind = magit_at_point(&path, &hunk);
	/* ...unchanged... */
```

- [ ] **Step 5: Dispatch the query mode in `magit_log_build`**

In `src/magit_cmd.c`, in `magit_log_build`, replace the `if (magit_log_file_path[0] != '\0') { ... } else { ... }` block that calls the bridge with a three-way dispatch. Change:

```c
	if (magit_log_file_path[0] != '\0') {
		/* ...async file log... */
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
```
to:
```c
	if (magit_log_graph || magit_log_range[0] != '\0' ||
	    magit_log_pickaxe != '\0') {
		(void)mg_magit_log_query_buffer(cwd, magit_log_graph,
		    magit_log_range[0] ? magit_log_range : NULL, NULL,
		    magit_log_pickaxe, magit_log_pickaxe_term,
		    magit_log_limit, magit_log_emit, bp);
	} else if (magit_log_file_path[0] != '\0') {
		/* ...async file log... */
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
```

- [ ] **Step 6: Add the menu entries (ascending key order!)**

In `src/magit_cmd.c`, before the `magit_logmenu` struct (~line 356), add the PF arrays:

```c
static PF log_g[] = { magit_log_graph_cmd };
static PF log_r[] = { magit_log_range_cmd };
static PF log_s[] = { magit_log_pickaxe_s };
static PF log_G[] = { magit_log_pickaxe_g };
```

Then replace the `magit_logmenu` definition. Keys MUST be ascending by the
`{ first, last, ... }` first-key: uppercase `'G'` (0x47) sorts before lowercase
`'f'`/`'g'`/`'l'`/`'r'`/`'s'`. Change:

```c
static struct KEYMAPE (2) magit_logmenu = {
	2,
	2,
	rescan,
	{
		{ 'f', 'f', log_f, NULL },	/* l f: log file */
		{ 'l', 'l', log_l, NULL }	/* l l: log all */
	}
};
```
to:
```c
static struct KEYMAPE (6) magit_logmenu = {
	6,
	6,
	rescan,
	{
		{ 'G', 'G', log_G, NULL },	/* l G: pickaxe -G (regex) */
		{ 'f', 'f', log_f, NULL },	/* l f: log file */
		{ 'g', 'g', log_g, NULL },	/* l g: graph log */
		{ 'l', 'l', log_l, NULL },	/* l l: log all */
		{ 'r', 'r', log_r, NULL },	/* l r: log range */
		{ 's', 's', log_s, NULL }	/* l s: pickaxe -S (string) */
	}
};
```

- [ ] **Step 7: Build ON + verify keymap assertion does not trip**

Run: `cmake --build --preset cpp 2>&1 | tail -3`
Expected: builds clean.
Run: `printf 'C-x g' ...` — instead, run the tmux smoke test in Step 8 which exercises `C-x g` (the keymap-sorted assert fires on first `C-x g`).

- [ ] **Step 8: tmux smoke test (graph + pickaxe)**

Run this script (sets up a repo with a merge + a token, drives mg):

```bash
MG=$PWD/build/src/mg
TMP=$(mktemp -d); cd "$TMP"
git init -q; git config user.name T; git config user.email t@e
git branch -M master
echo base > a.txt; git add a.txt; git commit -qm C1
git checkout -q -b side; echo NEEDLE_ABC > n.txt; git add n.txt; git commit -qm "add needle"
git checkout -q master; echo main > m.txt; git add m.txt; git commit -qm "C2 main"
git merge -q --no-edit side
tmux kill-session -t mglog 2>/dev/null || true
tmux new-session -d -s mglog -x 120 -y 40 -c "$TMP" "$MG"
sleep 1; tmux send-keys -t mglog C-x g; sleep 1.5
tmux send-keys -t mglog l; sleep 0.3; tmux send-keys -t mglog g; sleep 1.5
echo "=== l g (graph) ==="; tmux capture-pane -t mglog -p | sed -n '1,20p'
tmux send-keys -t mglog q; sleep 0.3
tmux send-keys -t mglog l; sleep 0.3; tmux send-keys -t mglog s; sleep 0.5
tmux send-keys -t mglog "NEEDLE_ABC"; tmux send-keys -t mglog Enter; sleep 1.5
echo "=== l s NEEDLE_ABC (pickaxe) ==="; tmux capture-pane -t mglog -p | sed -n '1,12p'
tmux kill-session -t mglog 2>/dev/null || true
cd /; rm -rf "$TMP"
```
Expected: the `l g` capture shows graph art (`*`, `|`, `\`/`/`) and a "Merge branch 'side'" line; the `l s` capture shows the "add needle" commit.

- [ ] **Step 9: OFF build clean**

Run: `cmake --build --preset c-legacy 2>&1 | tail -2 && nm ./build-c/src/mg | grep -ic magit`
Expected: `0`.

- [ ] **Step 10: Full suite + Alpine gate**

Run: `ctest --preset cpp 2>&1 | tail -3`
Expected: 100% pass.
Run: `docker build -f docker/Dockerfile.alpine -t mg-alpine . 2>&1 | grep -E "tests passed|FAILED"`
Expected: `100% tests passed`.

- [ ] **Step 11: Commit**

```bash
git add src/magit_cmd.c
git commit -m "feat(magit): l g graph / l r range / l s,l G pickaxe log (FM-LP)

New log-menu entries backed by mg_magit_log_query_buffer; module-static query
state dispatched in magit_log_build; l l / l f reset it to stay on libgit2.
Keymap kept ascending (G f g l r s). tmux-verified: l g renders graph art +
the merge commit, l s finds the introducing commit. OFF build 0 magit symbols.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp"
```

---

## Task 4: Docs / ledger + PR

**Files:**
- Modify: `todo.md` (FM-LP entry: mark graph/range/pickaxe done)
- Modify: `docs/superpowers/specs/2026-06-25-fm-lp-log-extras-design.md` (mark phases done)

- [ ] **Step 1: Update the spec phases to ✅ DONE and the todo ledger**

Mark all three phases done in the spec; in `todo.md`, update the FM-LP residual line — "`--graph` rendering, commit-range logs, `-S`/`-G` pickaxe search" — to record them as completed with `l g`/`l r`/`l s`/`l G`.

- [ ] **Step 2: Commit docs**

```bash
git add todo.md docs/superpowers/specs/2026-06-25-fm-lp-log-extras-design.md
git commit -m "docs(plan): FM-LP log extras complete (graph/range/pickaxe)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Pj1ZHaD7aNhVWJ69SFKJcp"
```

- [ ] **Step 3: Branch, push, open the stacked PR**

```bash
git push -u origin HEAD
```
Open a PR titled `feat(magit): log extras — graph / commit-range / pickaxe (FM-LP)`, base = `fm-git-cli-writes` (the current stack tip), summarizing the three modes, the CLI-backed `log_query`, and the macOS/Alpine/OFF gates. End the body with the Generated-with footer.

---

## Self-Review notes

- **Spec coverage:** graph (Task 1 step 10 + Task 3 `l g`), commit-range (Task 1 step 2 + `l r`), pickaxe `-S`/`-G` (Task 1 steps 6/8 + `l s`/`l G`), bridge (Task 2), `-n` infix reuse (Task 3 dispatch passes `magit_log_limit`), connector-line `MG_LINE_OTHER` handling (Task 2 + magit_log_emit already stores no oid), keymap ascending order (Task 3 step 6), OFF build (Task 3 step 9). All spec sections map to a task.
- **Type consistency:** `log_row{text,oid}` / `log_options{max_count,graph,range,file,pickaxe,pickaxe_term}` used identically in Tasks 1–2; bridge signature `mg_magit_log_query_buffer(repo,graph,range,file,pickaxe_kind,pickaxe_term,n,emit,ctx)` matches between Task 2 decl/def and Task 3 call.
- **Out-of-scope honored:** no `--decorate`, no `--all`, file restriction wired in the engine (`opts.file` / `-- <file>`) but no UI prompt (the bridge passes `NULL`), graph only via its own `l g`.
