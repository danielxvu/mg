# FM-FINDFILE-NEWFILE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `C-x C-f` on a path whose directory doesn't exist opens the buffer with no prompt (like Emacs); the create-directory prompt moves to save time.

**Architecture:** Two hunks in core `src/file.c` — `readin` (drop the open-time prompt) and `writeout` (turn the save-time "no such directory" hard-error into the create prompt). `do_makedir` (`mkdir -p`, returns TRUE/FALSE) reused as-is. A single-session pty test.

**Tech Stack:** C (core mg `file.c`); the pty editor test harness.

## Global Constraints

- Existing files, and saves into existing directories, are **byte-identical** to today (their branches are skipped).
- The build is `-Wall -Wextra -Werror`-clean: removing the only use of `statbuf` in `readin` means its declaration must be removed too.
- Core `file.c` compiles in ALL build configs — verify macOS (`cpp`), Alpine/musl (`cpp-linux`), AND the legacy plain-C build (`c-legacy`).

---

### Task 1: Move the create-directory prompt from open to save

**Files:**
- Modify: `src/file.c` (`readin` ~197–268; `writeout` ~704–725)
- Test: `tests/test_editor.cpp`

**Interfaces:**
- Behavior only; no new symbols. Consumes existing `eyorn`, `do_makedir`, `xdirname`, `access`, `dobeep`, `ewprintf`.

- [ ] **Step 1: Write the failing pty test**

Add to `tests/test_editor.cpp` (helpers `make_temp_dir`, `wait_for`, `drain_str`, `quit_neomg`, `NEOMG_BINARY`, `fs`, `<fstream>` all already present):
```cpp
TEST_CASE("find-file opens a missing-directory path without prompting; save creates it")
{
    auto dir = make_temp_dir();               // a plain temp dir (no repo needed)
    std::ofstream(dir / "seed.txt") << "seed\n"; // seed file -> default-dir = this dir
    const std::string seed = (dir / "seed.txt").string();
    winsize ws{}; ws.ws_row = 40; ws.ws_col = 100;
    int master = -1;
    pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    REQUIRE(pid >= 0);
    if (pid == 0) { ::setenv("TERM", "xterm", 1);
        ::execl(NEOMG_BINARY, "neomg", seed.c_str(), (char *)nullptr); _exit(127); }

    bool opened = false, no_open_prompt = false, save_prompt = false, wrote = false;
    if (wait_for(master, "seed.txt", std::chrono::seconds(8))) {
        // C-x C-f  nd/f.txt  RET  -- the directory "nd/" does not exist
        (void)!::write(master, "\x18\x06", 2);           // C-x C-f
        (void)!::write(master, "nd/f.txt\r", 9);
        // The buffer must open with NO open-time prompt (Emacs behavior).
        std::string acc = drain_str(master, std::chrono::seconds(2));
        no_open_prompt = acc.find("Missing directory") == std::string::npos;
        opened = acc.find("f.txt") != std::string::npos;  // new file in the modeline
        // Edit + save -> the prompt appears NOW.
        (void)!::write(master, "x", 1);
        (void)!::write(master, "\x18\x13", 2);            // C-x C-s
        save_prompt = wait_for(master, "Missing directory, create",
                               std::chrono::seconds(8));
        if (save_prompt) {
            (void)!::write(master, "y", 1);               // create the directory
            wrote = wait_for(master, "Wrote", std::chrono::seconds(8));
        }
    }
    quit_neomg(master, pid);
    bool on_disk = fs::exists(dir / "nd" / "f.txt");
    fs::remove_all(dir);
    CHECK(opened);         // buffer opened for the missing-dir path
    CHECK(no_open_prompt); // NO prompt at open (was a blocking prompt before)
    CHECK(save_prompt);    // prompt now appears at save time
    CHECK(wrote);          // save wrote the file after creating the directory
    CHECK(on_disk);        // directory + file actually created on disk
}
```

- [ ] **Step 2: Run → FAIL.** `cmake --build build --target test_editor && ./build/tests/test_editor --test-case="*missing-directory path*"`
Expected: FAIL — today `C-x C-f nd/f.txt` prints `Missing directory, create` at OPEN, so `no_open_prompt` is false (and the later save flow is off because the editor is sitting at the open prompt).

- [ ] **Step 3: Move the prompt (two hunks in `src/file.c`)**

**(a) `readin`** — remove the open-time prompt and the now-unused `statbuf`.
Replace the missing-directory branch (~lines 253–265):
```c
		} else {
			(void)xdirname(dp, fname, sizeof(dp));
			(void)strlcat(dp, "/", sizeof(dp));

			/* Missing directory; keep buffer rw, like emacs */
			if (stat(dp, &statbuf) == -1 && errno == ENOENT) {
				if (eyorn("Missing directory, create") == TRUE)
					(void)do_makedir(dp);
			} else if (access(dp, W_OK) == -1 && errno == EACCES) {
				ewprintf("File not found and directory"
				    " write-protected");
				ro = TRUE;
			}
		}
```
with:
```c
		} else {
			(void)xdirname(dp, fname, sizeof(dp));
			(void)strlcat(dp, "/", sizeof(dp));

			/*
			 * File doesn't exist. Open the buffer read-write like
			 * Emacs -- no create-directory prompt here; that moves
			 * to save time (writeout). A missing directory leaves
			 * the buffer writable (access() sets ENOENT, not
			 * EACCES); only a present-but-write-protected directory
			 * forces READONLY.
			 */
			if (access(dp, W_OK) == -1 && errno == EACCES) {
				ewprintf("File not found and directory"
				    " write-protected");
				ro = TRUE;
			}
		}
```
Then delete the now-unused declaration `struct stat	 statbuf;` from `readin`'s top (~line 200). (Grep `readin` to confirm no other use remains.)

**(b) `writeout`** — turn the save-time hard-error into the create prompt.
Replace the `else if (errno == ENOENT)` branch (~lines 718–721):
```c
		} else if (errno == ENOENT) {
   			dobeep();
			ewprintf("%s: no such directory", dp);
			return (FIOERR);
		}
```
with:
```c
		} else if (errno == ENOENT) {
			/*
			 * Missing directory: Emacs defers the create-directory
			 * prompt to save time (find-file opened the buffer
			 * without asking). Create the tree on yes and fall
			 * through to the write; abort otherwise.
			 */
			if (eyorn("Missing directory, create") != TRUE) {
				dobeep();
				ewprintf("%s: no such directory", dp);
				return (FIOERR);
			}
			if (do_makedir(dp) != TRUE) {
				dobeep();
				ewprintf("Unable to create directory %s", dp);
				return (FIOERR);
			}
		}
```
(After a successful `do_makedir`, control falls through to the existing
`ffwopen`/`ffputbuf` write path — the directory now exists.)

- [ ] **Step 4: Run → PASS.** Full suite: `cmake --build build && ctest --test-dir build --output-on-failure`. Watch for a `-Werror` unused-variable failure on `statbuf` (means the declaration removal was missed).

- [ ] **Step 5: Commit**
```bash
git add src/file.c tests/test_editor.cpp
git commit -m "fix(file): open missing-directory paths like Emacs (defer create to save)"
```

---

## Final verification (before PR)

- [ ] macOS: `cmake --preset cpp && cmake --build build && ctest --test-dir build --output-on-failure`.
- [ ] Alpine/musl: `docker build -f docker/Dockerfile.alpine -t mg-findfile .`.
- [ ] Legacy plain-C (core `file.c` compiles here too): `cmake --preset c-legacy && cmake --build --preset c-legacy` — 0 warnings (the `-Werror` set), `mg -h` exits 0.
- [ ] OFF/TSan (editor test): `cmake --build build-tsan && ./build-tsan/tests/test_editor --test-case="*missing-directory path*"`.
- [ ] Manual smoke: `C-x C-f nd/deep/f.txt` (missing tree) → opens with no prompt; `C-x C-s` → `Missing directory, create?` → `y` creates `nd/deep/` and writes; repeat with `n` → aborts; opening/saving an existing file shows no prompt.
- [ ] Update `todo.md`: record FM-FINDFILE-NEWFILE done (a core-editor fidelity fix, distinct from the FM-magit gap roadmap).
- [ ] Open the PR based on `neomg`.
