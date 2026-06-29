# FM-TRANSIENT-PUSH Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--tags`/`--dry-run` infixes to the `P` push transient and `--autostash`/`--ff-only` to the `F` pull transient.

**Architecture:** Push/pull network ops run through `magit_run_net(op, cwd, a, b, banner)` → `mg_magit_push_cli`/`mg_magit_pull_cli` (over `git_terminal`, tty). This plan: (1) the CLI bridge fns gain flag params; (2) a new captured `push_dry_run` (engine, mirroring `run_git_command`) routes a dry-run preview to `*magit-process*`; (3) the editor adds the infixes, replaces `magit_run_net`'s two int flags with a flags bitmask, and branches `--dry-run`.

**Tech Stack:** C++23 named modules; `detail::run_git` (captured) + `git_terminal` (tty); `proclog::record`; the `magit_transient` infix engine.

## Global Constraints

- All four new infixes default **off** → `P`/`F` behave **byte-identically** to today when nothing is toggled.
- `--dry-run` runs **captured** (`detail::run_git`: stdin→/dev/null, no tty) → `*magit-process*`; it never uses `git_terminal` and never suspends the tty. Auth limitation (non-interactive only; password-HTTPS fails fast) is the documented FM-GIT-CMD behavior.
- `--tags` is a CLI-path feature; the libgit2 push fallback (git off PATH) stays a plain push (no `--tags`) — a documented limitation, not a regression.
- Keep `F p`/`F r` as actions; `--autostash`/`--ff-only` are orthogonal modifiers. Don't police nonsensical combos (git reports them).
- Infix keys: push `t` (--tags), `d` (--dry-run); pull `a` (--autostash), `f` (--ff-only). None collide with push action `p` or pull actions `p`/`r`, or existing push infixes `f`/`u`. NOTE: pull's `--ff-only` key is `f`, which is fine (pull menu has no `f` action), but the push menu's existing `-f` (force) already uses `f` — the two menus are separate `magit_infix[]` arrays, so there's no clash.
- The infix struct's trailing field is `clear_on_empty` (from FM-TRANSIENT-DEPTH); FLAG infixes set it `0`.
- pty tests: single transient session, presence-only assertions, no buffer round-trip (FM-TRANSIENT-DEPTH musl lesson).
- Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + OFF (`cpp-tsan`).

---

### Task 1: Engine + bridge — push `--tags` + captured `--dry-run`

**Files:**
- Modify: `src/magit/git.cppm` (new `push_dry_run`), `src/magit/bridge.h` + `src/magit/bridge.cpp` (`mg_magit_push_cli` +tags; new `mg_magit_push_dry_run`), `src/magit_cmd.c` (the `magit_run_net` MNET_PUSH call — pass `tags=0` default to keep the build green)
- Test: `tests/test_git.cpp`, `tests/test_bridge.cpp`

**Interfaces:**
- Produces: `mg::git::push_dry_run(std::string repo, bool force, bool set_upstream, bool tags)` → exit code (records a `$` proclog entry with captured output); `mg_magit_push_cli(repo, force, set_upstream, int tags)`; `mg_magit_push_dry_run(repo, force, set_upstream, tags)`.

- [ ] **Step 1: Write the failing engine test**

In `tests/test_git.cpp` add (it has `make_repo_with_commit`, `set_test_config`, `<cstdlib>`):
```cpp
TEST_CASE("push_dry_run previews without mutating the remote and logs a $ entry")
{
    auto dir = make_repo_with_commit("c1"); // a.txt committed, HEAD on default branch
    set_test_config(dir);
    std::string d = dir.string();
    auto bare = d + "-remote.git";
    auto run = [&](const std::string &c) {
        return std::system(c.c_str());
    };
    REQUIRE(run("git init --bare '" + bare + "' >/dev/null 2>&1") == 0);
    REQUIRE(run("git -C '" + d + "' remote add origin '" + bare + "' >/dev/null 2>&1") == 0);
    // initial real push so upstream + the remote branch exist
    REQUIRE(run("git -C '" + d + "' push -u origin HEAD >/dev/null 2>&1") == 0);
    // a new commit that a real push WOULD send
    commit_file(dir, "b.txt", "bee\n", "c2 not yet pushed");
    auto remote_tip = [&]() {
        // capture the bare remote's branch tip before/after
        std::string cmd = "git -C '" + bare + "' rev-parse --all 2>/dev/null";
        FILE *p = ::popen(cmd.c_str(), "r");
        std::string out; char buf[256]; size_t k;
        while (p && (k = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, k);
        if (p) ::pclose(p);
        return out;
    };
    auto before = remote_tip();

    mg::magit::proclog::clear();
    int code = mg::git::push_dry_run(d, /*force*/false, /*set_upstream*/false, /*tags*/false);
    CHECK(code == 0);                       // dry-run to a reachable local remote succeeds
    CHECK(remote_tip() == before);          // remote tip UNCHANGED -- dry-run mutated nothing

    bool logged = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("push --dry-run") != std::string::npos)
            logged = true;
    CHECK(logged);
    fs::remove_all(dir); fs::remove_all(bare);
}
```
(`proclog::entry` is `{ char kind; std::string command; std::string output; bool ok; long duration_ms; }`; `snapshot()` → `std::vector<entry>`, `clear()` empties it. Iterate-and-match by `kind=='$'` + `command.find(...)` — the idiom the existing FM-PROCESS-LOG tests use; don't assume `front()` ordering.)

- [ ] **Step 2: Run → FAIL** (`push_dry_run` undefined). `cmake --build build --target test_git`.

- [ ] **Step 3: Implement `push_dry_run` (engine)**

In `src/magit/git.cppm`, add a declaration in the `mg::git` interface near `run_git_command` (the decl ~line 872):
```cpp
// Captured `git push --dry-run [flags] origin HEAD`: previews the push (no
// mutation), records a '$' proclog entry with the output, returns the exit code.
int push_dry_run(std::string repo, bool force, bool set_upstream, bool tags);
```
Add the implementation next to `run_git_command` (~line 3574), mirroring it:
```cpp
int push_dry_run(std::string repo, bool force, bool set_upstream, bool tags)
{
    std::vector<std::string> args{"push", "--dry-run"};
    if (force)        args.emplace_back("--force-with-lease");
    if (set_upstream) args.emplace_back("-u");
    if (tags)         args.emplace_back("--tags");
    args.emplace_back("origin");
    args.emplace_back("HEAD");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    auto t0 = std::chrono::steady_clock::now();
    auto run = detail::run_git(repo, args);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               run.output, run.code == 0, ms);
    return run.code;
}
```

- [ ] **Step 4: Run → engine test PASS.**

- [ ] **Step 5: Write the failing bridge test**

In `tests/test_bridge.cpp` add:
```cpp
TEST_CASE("mg_magit_push_cli appends --tags when requested")
{
    // No remote: the push fails fast (captured by git_terminal's child), but the
    // proclog entry records the exact command we built -- which is what we assert.
    auto dir = make_repo_with_commit("c1");
    std::string d = dir.string();
    mg::magit::proclog::clear();
    (void)mg_magit_push_cli(d.c_str(), /*force*/0, /*set_upstream*/0, /*tags*/1);
    bool tagged = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("push") != std::string::npos &&
            e.command.find("--tags") != std::string::npos)
            tagged = true;
    CHECK(tagged);
    fs::remove_all(dir);
}

TEST_CASE("mg_magit_push_dry_run records a push --dry-run entry")
{
    auto dir = make_repo_with_commit("c1");
    std::string d = dir.string();
    mg::magit::proclog::clear();
    (void)mg_magit_push_dry_run(d.c_str(), 0, 0, 0); // no remote -> fails, still logged
    bool logged = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("push --dry-run") != std::string::npos)
            logged = true;
    CHECK(logged);
    fs::remove_all(dir);
}
```

- [ ] **Step 6: Run → FAIL** (wrong arity / `mg_magit_push_dry_run` undefined).

- [ ] **Step 7: Change the push CLI ABI + add the dry-run forwarder**

`src/magit/bridge.h` — update/add the decls:
```c
int mg_magit_push_cli(const char *repo_path, int force, int set_upstream, int tags);
int mg_magit_push_dry_run(const char *repo_path, int force, int set_upstream, int tags);
```
`src/magit/bridge.cpp` — `mg_magit_push_cli` (~1433): add the `tags` param + the `--tags` append (before `origin`):
```cpp
extern "C" int mg_magit_push_cli(const char *repo_path, int force,
                                 int set_upstream, int tags)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"push"};
    if (force)        args.emplace_back("--force-with-lease");
    if (set_upstream) args.emplace_back("-u");
    if (tags)         args.emplace_back("--tags");
    args.emplace_back("origin");
    args.emplace_back("HEAD");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}
```
Add the dry-run forwarder (next to it):
```cpp
extern "C" int mg_magit_push_dry_run(const char *repo_path, int force,
                                     int set_upstream, int tags)
{
    if (repo_path == nullptr)
        return -1;
    return mg::git::push_dry_run(repo_path, force != 0, set_upstream != 0,
                                 tags != 0);
}
```
`src/magit_cmd.c` — the existing `magit_run_net` MNET_PUSH call (~line 3511) must keep compiling: pass the new `tags` arg as `0` for now (Task 3 wires it):
```c
	case MNET_PUSH:
		code = mg_magit_push_cli(cwd, a, b, /*tags*/0);
		break;
```

- [ ] **Step 8: Run → bridge tests PASS.** Full suite + OFF: `cmake --build build && ctest --test-dir build --output-on-failure`; `cmake --build build-tsan --target test_bridge && ./build-tsan/tests/test_bridge`.

- [ ] **Step 9: Commit**
```bash
git add src/magit/git.cppm src/magit/bridge.h src/magit/bridge.cpp src/magit_cmd.c tests/test_git.cpp tests/test_bridge.cpp
git commit -m "feat(push): --tags + captured push_dry_run (engine + bridge)"
```

---

### Task 2: Bridge — pull `--autostash` / `--ff-only`

**Files:**
- Modify: `src/magit/bridge.h` + `src/magit/bridge.cpp` (`mg_magit_pull_cli` +autostash +ff_only), `src/magit_cmd.c` (the two `magit_run_net` pull calls — pass `0, 0` defaults)
- Test: `tests/test_bridge.cpp`

**Interfaces:**
- Produces: `mg_magit_pull_cli(repo, rebase, int autostash, int ff_only)`.

- [ ] **Step 1: Write the failing test**

In `tests/test_bridge.cpp`:
```cpp
TEST_CASE("mg_magit_pull_cli appends --autostash and --ff-only when requested")
{
    auto dir = make_repo_with_commit("c1"); // no remote -> pull fails fast, still logged
    std::string d = dir.string();
    mg::magit::proclog::clear();
    (void)mg_magit_pull_cli(d.c_str(), /*rebase*/0, /*autostash*/1, /*ff_only*/1);
    bool both = false;
    for (auto &e : mg::magit::proclog::snapshot())
        if (e.kind == '$' && e.command.find("--autostash") != std::string::npos &&
            e.command.find("--ff-only") != std::string::npos)
            both = true;
    CHECK(both);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run → FAIL** (wrong arity).

- [ ] **Step 3: Change the pull CLI ABI**

`src/magit/bridge.h`:
```c
int mg_magit_pull_cli(const char *repo_path, int rebase, int autostash, int ff_only);
```
`src/magit/bridge.cpp` — `mg_magit_pull_cli` (~1453): add params + appends (after `--rebase`, before `--no-edit`):
```cpp
extern "C" int mg_magit_pull_cli(const char *repo_path, int rebase,
                                 int autostash, int ff_only)
{
    if (repo_path == nullptr)
        return -1;
    std::vector<std::string> args{"pull"};
    if (rebase)    args.emplace_back("--rebase");
    if (autostash) args.emplace_back("--autostash");
    if (ff_only)   args.emplace_back("--ff-only");
    args.emplace_back("--no-edit");
    args.emplace_back("origin");
    std::vector<std::string> cmd{"git"};
    cmd.insert(cmd.end(), args.begin(), args.end());
    int rc = mg::git::git_terminal(repo_path, std::move(args));
    mg::magit::proclog::record('$', mg::magit::proclog::argv_to_command(cmd),
                               "(output shown in terminal)", rc == 0, 0);
    return rc;
}
```
`src/magit_cmd.c` — the two `magit_run_net` pull calls (~lines 3514, 3517) pass `0, 0` for now (Task 3 wires them):
```c
	case MNET_PULL:
		code = mg_magit_pull_cli(cwd, 0, /*autostash*/0, /*ff_only*/0);
		break;
	case MNET_PULL_REBASE:
		code = mg_magit_pull_cli(cwd, 1, /*autostash*/0, /*ff_only*/0);
		break;
```

- [ ] **Step 4: Run → PASS.** Full suite + OFF (`test_bridge`).

- [ ] **Step 5: Commit**
```bash
git add src/magit/bridge.h src/magit/bridge.cpp src/magit_cmd.c tests/test_bridge.cpp
git commit -m "feat(pull): mg_magit_pull_cli --autostash/--ff-only params"
```

---

### Task 3: Editor — infixes + flags bitmask + the `--dry-run` branch

**Files:**
- Modify: `src/magit_cmd.c` (`push_infixes` +2; new `pull_infixes` wired into `pull_menu`; `magit_run_net` flags refactor; `magit_do_push`/`magit_push` dry-run branch; `magit_pull`/`magit_pull_rebase` read infixes)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Consumes: Task 1/2 ABI (`mg_magit_push_cli`+tags, `mg_magit_push_dry_run`, `mg_magit_pull_cli`+autostash+ff_only); the existing `magit_transient`/`magit_process`.

- [ ] **Step 1: Write the failing pty tests**

In `tests/test_editor.cpp` (uses `make_repo`, `wait_for`, `NEOMG_BINARY`, `forkpty`; the `grep_repo_session` helper pattern from FM-TRANSIENT-DEPTH for single-session, presence-only drives). Two tests:
```cpp
TEST_CASE("P transient lists --tags and --dry-run infixes")
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
            (void)!::write(master, "P", 1);           // push transient
            listed = wait_for(master, "--dry-run", std::chrono::seconds(8)); // infix rendered
        }
    }
    (void)!::write(master, "\x18\x03", 2);
    for (int i=0;i<20;++i){int st=0;if(::waitpid(pid,&st,WNOHANG)==pid)break;usleep(100000);}
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(listed);
}

TEST_CASE("P transient --dry-run opens *magit-process* with a push --dry-run entry")
{
    auto repo = make_repo(); // no origin -> the dry-run push fails fast but is still logged
    const std::string repofile = (repo / "tracked.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", repofile.c_str(), (char *)nullptr); _exit(127); }
    bool shown = false;
    if (wait_for(master, "tracked.txt", std::chrono::seconds(8))) {
        (void)!::write(master, "\x1bxmagit-status\r", 15);
        if (wait_for(master, "On branch", std::chrono::seconds(8))) {
            (void)!::write(master, "\x18" "1", 2);   // C-x 1
            (void)!::write(master, "P", 1);           // push transient
            if (wait_for(master, "--dry-run", std::chrono::seconds(8))) {
                (void)!::write(master, "d", 1);       // toggle --dry-run
                (void)!::write(master, "p", 1);       // P p: run (captured dry-run)
                (void)!::write(master, "\x0c", 1);    // force repaint
                shown = wait_for(master, "push --dry-run", std::chrono::seconds(8));
            }
        }
    }
    (void)!::write(master, "\x18\x03", 2);
    for (int i=0;i<20;++i){int st=0;if(::waitpid(pid,&st,WNOHANG)==pid)break;usleep(100000);}
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(shown); // *magit-process* shows the captured "push --dry-run" command
}
```
Third test — the pull transient renders its new infixes (symmetric with the first):
```cpp
TEST_CASE("F transient lists --autostash and --ff-only infixes")
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
            (void)!::write(master, "F", 1);           // pull transient
            listed = wait_for(master, "--autostash", std::chrono::seconds(8));
        }
    }
    (void)!::write(master, "\x18\x03", 2);
    for (int i=0;i<20;++i){int st=0;if(::waitpid(pid,&st,WNOHANG)==pid)break;usleep(100000);}
    ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(master);
    fs::remove_all(repo);
    CHECK(listed);
}
```
Run → FAIL (infixes/branch don't exist).

- [ ] **Step 2: Add the push infixes + the pull infixes**

`src/magit_cmd.c` — extend `push_infixes` (~line 791):
```c
static struct magit_infix push_infixes[] = {
	{ 'f', "-f --force-with-lease", MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 'u', "-u --set-upstream",     MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 't', "--tags",                MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 'd', "--dry-run",             MAGIT_INFIX_FLAG, 0, "", 0 }
};
/* Pull infixes: read by magit_pull / magit_pull_rebase (0 = autostash, 1 = ff-only). */
static struct magit_infix pull_infixes[] = {
	{ 'a', "--autostash", MAGIT_INFIX_FLAG, 0, "", 0 },
	{ 'f', "--ff-only",   MAGIT_INFIX_FLAG, 0, "", 0 }
};
```
Wire `pull_infixes` into `pull_menu` (~line 848), replacing its `NULL, 0`:
```c
static struct magit_menu pull_menu = { "Pull", (KEYMAP *)&magit_pullmenu,
	pull_items, MENU_N(pull_items), pull_infixes, MENU_N(pull_infixes) };
```
(`pull_infixes` must be declared above `pull_menu`; place it just before the `pull_menu` definition or with `push_infixes`.)

- [ ] **Step 3: Add the flags bitmask + refactor `magit_run_net`**

`src/magit_cmd.c` — near the `enum magit_net_op` (~line 3488), add:
```c
/* magit_run_net flag bits (op-specific). */
#define MNET_FORCE     0x01  /* push: --force-with-lease */
#define MNET_UPSTREAM  0x02  /* push: -u */
#define MNET_TAGS      0x04  /* push: --tags */
#define MNET_AUTOSTASH 0x08  /* pull: --autostash */
#define MNET_FF_ONLY   0x10  /* pull: --ff-only */
```
Change `magit_run_net`'s signature from `(enum magit_net_op op, const char *cwd, int a, int b, const char *banner)` to `(enum magit_net_op op, const char *cwd, int flags, const char *banner)`, and its dispatch:
```c
	switch (op) {
	case MNET_PUSH:
		code = mg_magit_push_cli(cwd, (flags & MNET_FORCE) != 0,
		    (flags & MNET_UPSTREAM) != 0, (flags & MNET_TAGS) != 0);
		break;
	case MNET_PULL:
		code = mg_magit_pull_cli(cwd, 0, (flags & MNET_AUTOSTASH) != 0,
		    (flags & MNET_FF_ONLY) != 0);
		break;
	case MNET_PULL_REBASE:
		code = mg_magit_pull_cli(cwd, 1, (flags & MNET_AUTOSTASH) != 0,
		    (flags & MNET_FF_ONLY) != 0);
		break;
	case MNET_FETCH:
	default:
		code = mg_magit_fetch_cli(cwd);
		break;
	}
```
Update the forward declaration of `magit_run_net` if one exists (search `magit_run_net(`).

- [ ] **Step 4: Update the callers + the push dry-run branch**

`src/magit_cmd.c` — `magit_fetch` (~3541): `magit_run_net(MNET_FETCH, cwd, 0, "Fetching from origin...")`.

`magit_pull` (~3570) — build flags from `pull_infixes`:
```c
	int flags = (pull_infixes[0].on ? MNET_AUTOSTASH : 0) |
	            (pull_infixes[1].on ? MNET_FF_ONLY : 0);
	code = magit_run_net(MNET_PULL, cwd, flags, "Pulling from origin...");
```
`magit_pull_rebase` (~3633) — same flags, `MNET_PULL_REBASE`:
```c
	int flags = (pull_infixes[0].on ? MNET_AUTOSTASH : 0) |
	            (pull_infixes[1].on ? MNET_FF_ONLY : 0);
	code = magit_run_net(MNET_PULL_REBASE, cwd, flags,
	    "Pulling (rebase) from origin...");
```
`magit_do_push` — change its signature to take `flags` and decode for the libgit2 fallback:
```c
static int
magit_do_push(int flags, int f, int n)
{
	char	cwd[PATH_MAX];
	int	code;

	if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
		return (FALSE);
	code = magit_run_net(MNET_PUSH, cwd, flags, "Pushing to origin...");
	if (code == -2)
		return (FALSE);
	if (code == -1) {			/* git unavailable -> libgit2 (no --tags) */
		ewprintf("Pushing to origin...");
		if (mg_magit_push(cwd, "origin", (flags & MNET_FORCE) != 0,
		    (flags & MNET_UPSTREAM) != 0) != 1) {
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
```
`magit_push` — read the four push infixes; branch `--dry-run` to the captured path + `*magit-process*`:
```c
static int
magit_push(int f, int n)
{
	int	force = push_infixes[0].on;
	int	upstream = push_infixes[1].on;
	int	tags = push_infixes[2].on;
	int	flags;

	if (push_infixes[3].on) {	/* --dry-run: captured preview -> *magit-process* */
		char	cwd[PATH_MAX];

		if (getbufcwd(cwd, sizeof(cwd)) != TRUE)
			return (FALSE);
		(void)mg_magit_push_dry_run(cwd, force, upstream, tags);
		return (magit_process(f, n));
	}
	flags = (force ? MNET_FORCE : 0) | (upstream ? MNET_UPSTREAM : 0) |
	    (tags ? MNET_TAGS : 0);
	return (magit_do_push(flags, f, n));
}
```
(Declare `int mg_magit_push_dry_run(const char *, int, int, int);` in the bridge header block the editor includes — Task 1 added it to `bridge.h`, so it's already visible.)

- [ ] **Step 5: Run the pty tests → PASS.** Full suite `cmake --build build && ctest --test-dir build --output-on-failure`. Harden any flaky pty test with the established pattern (drain/forced repaint); never weaken an assertion.

- [ ] **Step 6: Commit**
```bash
git add src/magit_cmd.c tests/test_editor.cpp
git commit -m "feat(push/pull): --tags/--dry-run + --autostash/--ff-only transient infixes"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-transient-push .`.
- [ ] OFF: `cmake --build build-tsan && ctest --test-dir build-tsan -R "git|bridge|editor"` (+ the two new transient pty tests explicitly under TSan).
- [ ] Manual smoke: `P` shows `-f`/`-u`/`--tags`/`--dry-run`; toggling `--dry-run` + `P p` opens `*magit-process*` with the preview; `F` shows `--autostash`/`--ff-only`; with no infix set push/pull are unchanged.
- [ ] Update `todo.md`: mark FM-TRANSIENT-PUSH done; confirm the deferred items (`--no-ff`/`--force`, chosen remote/refspec, fetch transient) are recorded.
- [ ] Open the PR stacked on `fm-transient-depth` (#98) — bottom-up merge order through GitHub.
