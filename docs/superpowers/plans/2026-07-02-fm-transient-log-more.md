# FM-TRANSIENT-LOG-MORE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--since`/`--until`/`--reverse`/`--merges`/`--no-merges` to the `l` log transient, and refactor the log-query C ABI to a struct.

**Architecture:** `log_options` gains 5 fields; `log_query` appends the guarded git args; the log-query C ABI becomes `mg_magit_log_query_buffer(const struct mg_log_query *q, emit, ctx)` (the deferred cleanup); `log_infixes` gains 5 entries; `magit_log_build` fills the struct from the infixes.

**Tech Stack:** C++23 module (`mg.git`); the C ABI struct; the `magit_transient` infix engine; the pty test harness.

## Global Constraints

- All five default off/empty → the `git log` argv (and output) is **byte-identical** to before.
- Empty `--since=`/`--until=` ⇒ append nothing (never a bare `--since=`).
- New infix keys must not collide with the log actions (`G`/`f`/`g`/`h`/`l`/`r`/`s`) or the existing infixes (`n`/`a`/`m`/`A`): use `S` (since), `U` (until), `v` (reverse), `M` (merges), `N` (no-merges) — verify none collide.
- `log_infixes` order after this: `[0]=-n`, `[1]=--author`, `[2]=--grep`, `[3]=--all`, `[4]=--since`, `[5]=--until`, `[6]=--reverse`, `[7]=--merges`, `[8]=--no-merges`.
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine — `log_options` fields + `log_query` args

**Files:**
- Modify: `src/magit/git.cppm` (`log_options` ~532; `log_query` args ~1568)
- Test: `tests/test_git.cpp`

**Interfaces:**
- Produces: `log_options` gains `std::string since; std::string until; bool reverse=false; bool merges=false; bool no_merges=false;`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_git.cpp` (helpers `make_repo_with_commit`, `commit_file`, `set_test_config`, `<cstdlib>`; `log_query` returns `std::expected<std::vector<log_row>, error>`; `log_row{text, oid}`):
```cpp
TEST_CASE("log_query --since/--until/--reverse/--no-merges/--merges")
{
    auto dir = make_repo_with_commit("c0"); // a.txt @ ~now (default sig)
    set_test_config(dir);
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    // An old dated commit, a side branch, and a --no-ff merge (a merge commit).
    REQUIRE(run("-c user.name=T -c user.email=t@e "
        "-c 'commit.gpgsign=false' commit --allow-empty --date='2001-01-01T00:00:00' "
        "-m OLD_COMMIT --author='T <t@e>' 2>/dev/null "
        "|| GIT_COMMITTER_DATE='2001-01-01T00:00:00' git -C '" + d +
        "' commit --allow-empty --date='2001-01-01T00:00:00' -m OLD_COMMIT") == 0);
    REQUIRE(run("checkout -b side") == 0);
    commit_file(dir, "s.txt", "s\n", "SIDE_COMMIT");
    REQUIRE(run("checkout -") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@e merge --no-ff --no-edit "
                "-m MERGE_COMMIT side") == 0);

    auto has = [](const std::vector<mg::git::log_row> &rows, const char *needle) {
        for (auto &r : rows) if (r.text.find(needle) != std::string::npos) return true;
        return false;
    };
    // --no-merges excludes the merge; --merges returns only merge commits.
    mg::git::log_options nom; nom.no_merges = true;
    auto rn = mg::git::log_query(d, nom); REQUIRE(rn.has_value());
    CHECK_FALSE(has(*rn, "MERGE_COMMIT"));
    mg::git::log_options mo; mo.merges = true;
    auto rm = mg::git::log_query(d, mo); REQUIRE(rm.has_value());
    CHECK(has(*rm, "MERGE_COMMIT"));
    for (auto &r : *rm) CHECK(r.text.find("OLD_COMMIT") == std::string::npos);
    // --until excludes commits after a date; the 2001 commit predates it.
    mg::git::log_options un; un.until = "2001-06-01";
    auto ru = mg::git::log_query(d, un); REQUIRE(ru.has_value());
    CHECK(has(*ru, "OLD_COMMIT"));
    CHECK_FALSE(has(*ru, "MERGE_COMMIT")); // merge is @ ~now, after 2001-06
    // --since after 2001 drops the old commit.
    mg::git::log_options si; si.since = "2010-01-01";
    auto rs = mg::git::log_query(d, si); REQUIRE(rs.has_value());
    CHECK_FALSE(has(*rs, "OLD_COMMIT"));
    // --reverse flips order: oldest (OLD_COMMIT / c0) first.
    mg::git::log_options base, rev; rev.reverse = true;
    auto rb = mg::git::log_query(d, base); auto rr = mg::git::log_query(d, rev);
    REQUIRE(rb.has_value()); REQUIRE(rr.has_value());
    REQUIRE(!rb->empty()); REQUIRE(!rr->empty());
    CHECK(rb->front().oid != rr->front().oid); // newest-first vs oldest-first
    fs::remove_all(dir);
}
```
(Confirm the `commit --date`/`GIT_COMMITTER_DATE` incantation works in the test env; simplify to whichever the suite already uses if one exists. The key behaviors: no_merges excludes / merges isolates the merge; until/since bound by date; reverse flips `front()`.)

- [ ] **Step 2: Run → FAIL** (`log_options` lacks the fields).

- [ ] **Step 3: Add fields + args**

`src/magit/git.cppm` — extend `log_options` (after `all`, ~541):
```cpp
    std::string since;           // --since=<date>  ("" = none)
    std::string until;           // --until=<date>  ("" = none)
    bool        reverse   = false; // --reverse
    bool        merges    = false; // --merges (only merge commits)
    bool        no_merges = false; // --no-merges
```
In `log_query`, after the existing `--all`/`--author=`/`--grep=` appends and before `--format`:
```cpp
    if (opts.reverse)   args.emplace_back("--reverse");
    if (opts.merges)    args.emplace_back("--merges");
    if (opts.no_merges) args.emplace_back("--no-merges");
    if (!opts.since.empty()) args.emplace_back("--since=" + opts.since);
    if (!opts.until.empty()) args.emplace_back("--until=" + opts.until);
```

- [ ] **Step 4: Run → PASS.** Full suite `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit** `git commit -m "feat(log): log_options since/until/reverse/merges/no_merges + args"`

---

### Task 2: Bridge — the `mg_log_query` struct refactor

**Files:**
- Modify: `src/magit/bridge.h` (struct + decl), `src/magit/bridge.cpp` (impl ~1080), `src/magit_cmd.c` (the `magit_log_build` caller ~1552 — fill+pass the struct, new fields defaulted)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Consumes: Task 1's `log_options` fields.
- Produces: `struct mg_log_query { … }` + `mg_magit_log_query_buffer(const struct mg_log_query *q, emit, ctx)`.

- [ ] **Step 1: Write the failing test**

In `tests/test_bridge.cpp` (emit callback; `MG_LINE_COMMIT`). The struct carries the repo path (the old ABI's first param) as its first field `repo`:
```cpp
TEST_CASE("mg_magit_log_query_buffer (struct) threads --no-merges")
{
    auto dir = make_repo_with_commit("c0");
    std::string d = dir.string();
    auto run = [&](const std::string &c) {
        return std::system(("git -C '" + d + "' " + c + " >/dev/null 2>&1").c_str());
    };
    REQUIRE(run("checkout -b side") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@e commit --allow-empty -m SIDE") == 0);
    REQUIRE(run("checkout -") == 0);
    REQUIRE(run("-c user.name=T -c user.email=t@e merge --no-ff --no-edit -m MERGE_X side") == 0);

    auto count_merge = [&](int no_merges) {
        int m = 0;
        mg_log_query q{};
        q.repo = d.c_str();
        q.n = 50;
        q.no_merges = no_merges;
        mg_magit_log_query_buffer(&q,
            [](void *c, const char *l, int kind, const char *, int) {
                if (kind == MG_LINE_COMMIT &&
                    std::string(l).find("MERGE_X") != std::string::npos)
                    (*static_cast<int*>(c))++;
            }, &m);
        return m;
    };
    CHECK(count_merge(1) == 0); // --no-merges excludes the merge commit
    CHECK(count_merge(0) >= 1); // without it, the merge appears
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL.**

- [ ] **Step 3: Add the struct + refactor decl/impl/caller**

`src/magit/bridge.h` — replace the `mg_magit_log_query_buffer` decl (~101) with:
```c
struct mg_log_query {
    const char *repo;
    int         graph;
    const char *range;         /* NULL/"" = HEAD */
    const char *file;          /* NULL/"" = repo-wide */
    int         pickaxe_kind;  /* 0 / 'S' / 'G' */
    const char *pickaxe_term;
    int         n;             /* -n limit, 0 = none */
    const char *author;        /* NULL = none */
    const char *grep;          /* NULL = none */
    int         all;
    const char *since;         /* NULL = none */
    const char *until;         /* NULL = none */
    int         reverse;
    int         merges;
    int         no_merges;
};
int mg_magit_log_query_buffer(const struct mg_log_query *q,
                              mg_magit_emit_fn emit, void *ctx);
```
`src/magit/bridge.cpp` (~1080) — take the struct, map each field (NULL strings stay empty):
```cpp
extern "C" int mg_magit_log_query_buffer(const struct mg_log_query *q,
                                         mg_magit_emit_fn emit, void *ctx)
{
    if (q == nullptr || q->repo == nullptr || emit == nullptr)
        return 0;
    mg::git::log_options opts;
    opts.graph = q->graph != 0;
    opts.max_count = q->n > 0 ? static_cast<std::size_t>(q->n) : 0;
    if (q->range) opts.range = q->range;
    if (q->file)  opts.file = q->file;
    if (q->pickaxe_kind == 'S' || q->pickaxe_kind == 'G') {
        opts.pickaxe = static_cast<char>(q->pickaxe_kind);
        opts.pickaxe_term = q->pickaxe_term ? q->pickaxe_term : "";
    }
    if (q->author) opts.author = q->author;
    if (q->grep)   opts.grep = q->grep;
    opts.all = q->all != 0;
    if (q->since) opts.since = q->since;
    if (q->until) opts.until = q->until;
    opts.reverse = q->reverse != 0;
    opts.merges = q->merges != 0;
    opts.no_merges = q->no_merges != 0;
    // … rest unchanged: log_query(q->repo, opts) + the emit loop …
}
```
`src/magit_cmd.c` — the `magit_log_build` caller (~1552): fill a `struct mg_log_query` from the current state, new fields defaulted (Task 3 wires them):
```c
		struct mg_log_query q = {0};
		q.repo = cwd;
		q.graph = magit_log_graph;
		q.range = magit_log_range[0] ? magit_log_range : NULL;
		q.file = magit_log_file_path[0] ? magit_log_file_path : NULL;
		q.pickaxe_kind = magit_log_pickaxe;
		q.pickaxe_term = magit_log_pickaxe_term;
		q.n = magit_log_limit;
		q.author = log_infixes[1].value[0] ? log_infixes[1].value : NULL;
		q.grep = log_infixes[2].value[0] ? log_infixes[2].value : NULL;
		q.all = log_infixes[3].on;
		/* since/until/reverse/merges/no_merges wired in Task 3 */
		(void)mg_magit_log_query_buffer(&q, magit_log_emit, bp);
```
(`bridge.h` is C-included by `magit_cmd.c`, so `struct mg_log_query` is visible. `{0}` zero-inits the new fields.)

- [ ] **Step 4: Run → PASS.** Full suite + OFF (`test_bridge`).

- [ ] **Step 5: Commit** `git commit -m "refactor(log): mg_magit_log_query_buffer takes a struct; +since/until/reverse/merges"`

---

### Task 3: Editor — the 5 new infixes

**Files:**
- Modify: `src/magit_cmd.c` (`log_infixes` ~804; `magit_log_build` routing + struct fill)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: Task 2's `struct mg_log_query`.

- [ ] **Step 1: Write the failing pty test**

In `tests/test_editor.cpp` — the `l` transient renders the new infixes:
```cpp
TEST_CASE("l transient lists the extra log args (--since/--reverse/--no-merges)")
{
    auto repo = make_repo();
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool listed = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "l", 1);           // log transient
            listed = wait_for(master, "--no-merges", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed); // the new infixes render in the l transient
}
```
Run → FAIL (`--no-merges` isn't an infix yet).

- [ ] **Step 2: Add the infixes**

`src/magit_cmd.c` — extend `log_infixes` (~804), keys verified non-colliding:
```c
static struct magit_infix log_infixes[] = {
	{ 'n', "-n --max-count", MAGIT_INFIX_VALUE, 0, "100", 0 },
	{ 'a', "--author",       MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'm', "--grep",         MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'A', "--all",          MAGIT_INFIX_FLAG,  0, "",    0 },
	{ 'S', "--since",        MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'U', "--until",        MAGIT_INFIX_VALUE, 0, "",    1 },
	{ 'v', "--reverse",      MAGIT_INFIX_FLAG,  0, "",    0 },
	{ 'M', "--merges",       MAGIT_INFIX_FLAG,  0, "",    0 },
	{ 'N', "--no-merges",    MAGIT_INFIX_FLAG,  0, "",    0 }
};
```

- [ ] **Step 3: Wire them into `magit_log_build`**

Extend the routing condition (the `if (magit_log_graph || … log_infixes[3].on)`) with the new infixes, and fill the struct fields:
```c
	    log_infixes[4].value[0] != '\0' ||   /* --since */
	    log_infixes[5].value[0] != '\0' ||   /* --until */
	    log_infixes[6].on || log_infixes[7].on || log_infixes[8].on) {
```
and after the existing `q.all = …;`:
```c
		q.since = log_infixes[4].value[0] ? log_infixes[4].value : NULL;
		q.until = log_infixes[5].value[0] ? log_infixes[5].value : NULL;
		q.reverse = log_infixes[6].on;
		q.merges = log_infixes[7].on;
		q.no_merges = log_infixes[8].on;
```

- [ ] **Step 4: Run → PASS.** Full suite.

- [ ] **Step 5: Commit** `git commit -m "feat(log): --since/--until/--reverse/--merges/--no-merges infixes"`

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-logmore .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"`.
- [ ] Manual smoke: `l` shows the new infixes; set `--since=`/`--no-merges`/`--reverse` and run `l l` → the log reflects them; with nothing set the log is unchanged.
- [ ] Update `todo.md`: mark FM-TRANSIENT-LOG-MORE done.
- [ ] Open the PR based on `neomg`.
