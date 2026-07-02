# FM-TRANSIENT-FETCH Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `f` a fetch transient with `--prune`/`--tags`/`--all`; `f f` fetches with the toggled args.

**Architecture:** Mirror FM-TRANSIENT-PUSH. Bridge `mg_magit_fetch_cli` gains flag params; the editor adds a `fetch_menu`/`fetch_infixes`, rebinds `f` to open the transient, and threads flags through `magit_run_net`'s `MNET_FETCH` case. No captured path (fetch is tty).

**Tech Stack:** C++23 bridge; the mg keymap + `magit_transient`; the pty test harness.

## Global Constraints

- Default off → `f f` runs `git fetch origin`, byte-identical to today's `f`.
- `--all` omits `origin` from the argv (`git fetch --all …`); otherwise `origin` is last.
- Fetch `--tags` reuses `MNET_TAGS` (0x04); add `MNET_PRUNE` (0x20) and `MNET_ALL` (0x40).
- The libgit2 fetch fallback stays a plain fetch (flags are CLI-path only, documented).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Bridge — `mg_magit_fetch_cli` flag params

**Files:**
- Modify: `src/magit/bridge.h` (decl), `src/magit/bridge.cpp` (impl, ~1420), `src/magit_cmd.c` (the `magit_run_net` `MNET_FETCH` caller — pass `0, 0, 0` defaults to keep the build green)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Produces: `mg_magit_fetch_cli(repo, int prune, int tags, int all)`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_bridge.cpp` (proclog `entry{kind,command,output,ok,duration_ms}`; iterate-and-match by `kind=='$'`):
```cpp
TEST_CASE("mg_magit_fetch_cli threads --prune/--tags and omits origin for --all")
{
    auto dir = make_repo_with_commit("c1"); // no remote -> fails fast, still logged
    std::string d = dir.string();

    mg::magit::proclog::clear();
    (void)mg_magit_fetch_cli(d.c_str(), /*prune*/1, /*tags*/1, /*all*/0);
    bool flagged = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("fetch") != std::string::npos &&
            e.command.find("--prune") != std::string::npos &&
            e.command.find("--tags") != std::string::npos &&
            e.command.find("origin") != std::string::npos)
            flagged = true;
    CHECK(flagged);

    mg::magit::proclog::clear();
    (void)mg_magit_fetch_cli(d.c_str(), /*prune*/0, /*tags*/0, /*all*/1);
    bool allNoOrigin = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("fetch") != std::string::npos &&
            e.command.find("--all") != std::string::npos &&
            e.command.find("origin") == std::string::npos) // --all omits origin
            allNoOrigin = true;
    CHECK(allNoOrigin);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (wrong arity).

- [ ] **Step 3: Change the decl, impl, caller**

`src/magit/bridge.h` (~229):
```c
int mg_magit_fetch_cli(const char *repo_path, int prune, int tags, int all);
```
`src/magit/bridge.cpp` (~1420):
```cpp
extern "C" int mg_magit_fetch_cli(const char *repo_path, int prune, int tags,
                                  int all)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"fetch"};
    if (all)   args.emplace_back("--all");
    if (prune) args.emplace_back("--prune");
    if (tags)  args.emplace_back("--tags");
    if (!all)  args.emplace_back("origin"); // --all fetches every remote
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}
```
`src/magit_cmd.c` — the `MNET_FETCH` case (~3537) keeps compiling with defaults:
```c
	case MNET_FETCH:
	default:
		code = mg_magit_fetch_cli(cwd, /*prune*/0, /*tags*/0, /*all*/0);
		break;
```

- [ ] **Step 4: Run → PASS.** Full suite + OFF: `cmake --build build && ctest --test-dir build --output-on-failure`; `cmake --build build-tsan --target test_bridge && ./build-tsan/tests/test_bridge`.

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.h src/magit/bridge.cpp src/magit_cmd.c tests/test_bridge.cpp
git commit -m "feat(fetch): mg_magit_fetch_cli --prune/--tags/--all params"
```

---

### Task 2: Editor — the fetch transient

**Files:**
- Modify: `src/magit_cmd.c` (MNET bits; `fetch_infixes`/`fetch_items`/`magit_fetchmenu`/`fetch_menu`; `magit_menu_fetch`; `magit_fetch` reads infixes; rebind `magit_f`; `MNET_FETCH` decode)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: Task 1's `mg_magit_fetch_cli(repo, prune, tags, all)`; the existing `magit_transient`, `magit_run_net` flags bitmask.

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp`:
```cpp
TEST_CASE("f opens the fetch transient with --prune/--tags/--all")
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
            (void)!::write(master, "f", 1);           // open the fetch transient
            listed = wait_for(master, "--prune", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    fs::remove_all(repo);
    CHECK(listed); // f now opens the fetch transient (renders --prune)
}
```
Run → FAIL (`f` fetches directly today — `--prune` never renders).

- [ ] **Step 2: Add the MNET bits + the fetch menu**

`src/magit_cmd.c` — after `#define MNET_FF_ONLY 0x10` (~3503):
```c
#define MNET_PRUNE     0x20  /* fetch: --prune */
#define MNET_ALL       0x40  /* fetch: --all */
```
Forward-declare `magit_menu_fetch` next to `magit_menu_pull` (~110):
```c
static int	magit_menu_fetch(int, int);
```
Add the fetch action PF next to `magit_f` (~347). Rebind `magit_f`, and add `fetch_f`:
```c
static PF magit_f[] = { magit_menu_fetch };	/* f -> fetch menu prefix (was magit_fetch) */
static PF fetch_f[] = { magit_fetch };		/* f f: do the fetch */
```
Add the menu structures next to `pull_menu`/`magit_pullmenu`/`pull_infixes`/`pull_items`:
```c
static struct KEYMAPE (1) magit_fetchmenu = {
	1,
	1,
	rescan,
	{
		{ 'f', 'f', fetch_f, NULL }	/* f f: fetch */
	}
};
static struct magit_infix fetch_infixes[] = {
	{ 'p', "--prune", MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 't', "--tags",  MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 'a', "--all",   MAGIT_INFIX_FLAG, 0, "", 0 }
};
static const struct magit_menu_item fetch_items[] = {
	{ 'f', "fetch" }
};
static struct magit_menu fetch_menu = { "Fetch", (KEYMAP *)&magit_fetchmenu,
	fetch_items, MENU_N(fetch_items), fetch_infixes, MENU_N(fetch_infixes) };
```
(Place each near its `pull_*` analogue so declaration order stays valid: `magit_fetchmenu` before `fetch_menu`; `fetch_infixes`/`fetch_items` before `fetch_menu`; `fetch_f` after `magit_fetch`'s forward decl. `fetch_infixes` keys `p`/`t`/`a` don't collide with the menu's single action `f`.)

- [ ] **Step 3: Wire `magit_menu_fetch`, `magit_fetch`, and the `MNET_FETCH` decode**

Add `magit_menu_fetch` next to `magit_menu_pull` (~996):
```c
static int magit_menu_fetch(int f, int n)  { return (magit_transient(&fetch_menu, f, n)); }
```
Change `magit_fetch` to read the infixes into flags (the `magit_run_net` call, ~3559):
```c
	int flags = (fetch_infixes[0].on ? MNET_PRUNE : 0) |
	            (fetch_infixes[1].on ? MNET_TAGS  : 0) |
	            (fetch_infixes[2].on ? MNET_ALL   : 0);
	code = magit_run_net(MNET_FETCH, cwd, flags, "Fetching from origin...");
```
(Add `int flags;` to `magit_fetch`'s top declaration block, KNF-style.)
Change the `MNET_FETCH` case (~3537) to decode:
```c
	case MNET_FETCH:
	default:
		code = mg_magit_fetch_cli(cwd, (flags & MNET_PRUNE) != 0,
		    (flags & MNET_TAGS) != 0, (flags & MNET_ALL) != 0);
		break;
```

- [ ] **Step 4: Run → PASS.** Full suite `cmake --build build && ctest --test-dir build --output-on-failure`.

- [ ] **Step 5: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(fetch): f opens a fetch transient (--prune/--tags/--all)"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-fetch .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "bridge|editor"`.
- [ ] Manual smoke: `f` opens the fetch transient; toggle `--prune`/`--tags`/`--all`; `f f` fetches with them; with nothing toggled `f f` is a plain `git fetch origin`.
- [ ] Update `todo.md`: record FM-TRANSIENT-FETCH done.
- [ ] Open the PR based on `neomg`.
