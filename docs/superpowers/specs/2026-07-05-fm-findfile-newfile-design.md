# FM-FINDFILE-NEWFILE — open missing-directory paths like Emacs

## Why

Opening a file whose directory does not exist makes neomg **block** with
`Missing directory, create? (y or n)` at open time (`readin` in `src/file.c`).
GNU Emacs (verified, 30.2) does not: `find-file` on such a path opens the buffer
immediately (a `(New file)` buffer, unmodified, writable) and defers any
create-directory question to **save** time. For a Magit/Emacs-inspired editor
this open-time prompt is a surprising divergence. This moves the prompt from open
to save, matching Emacs.

This is core find-file behavior inherited verbatim from upstream OpenBSD mg
(`src/file.c` is untouched by the neomg fork); it is not caused by any native-
magit change. Relative-name resolution is unchanged (both editors resolve a
relative name against the current buffer's directory) — only the *reaction to a
missing directory* changes.

## Decided model (from brainstorming)

Move the create-directory prompt from open (`readin`) to save (`writeout`):
- **Open:** no prompt; the buffer opens read-write like Emacs.
- **Save into a missing directory:** prompt `Missing directory, create?`; `y`
  creates the directory tree (`do_makedir`, i.e. `mkdir -p`) and writes; `n` (or
  a failed create) aborts with the existing "no such directory" error.

## Architecture

Both changes are in `src/file.c`; nothing else is touched. `do_makedir`
(`src/dir.c`) already creates intermediate directories and returns `TRUE`/`FALSE`
— used as-is.

### 1. Open — `readin` (~lines 249–268)

The current missing-directory branch:
```c
    } else {
        (void)xdirname(dp, fname, sizeof(dp));
        (void)strlcat(dp, "/", sizeof(dp));
        /* Missing directory; keep buffer rw, like emacs */
        if (stat(dp, &statbuf) == -1 && errno == ENOENT) {
            if (eyorn("Missing directory, create") == TRUE)
                (void)do_makedir(dp);
        } else if (access(dp, W_OK) == -1 && errno == EACCES) {
            ewprintf("File not found and directory write-protected");
            ro = TRUE;
        }
    }
```
becomes — drop the prompt/create; keep only the write-protected → READONLY case:
```c
    } else {
        (void)xdirname(dp, fname, sizeof(dp));
        (void)strlcat(dp, "/", sizeof(dp));
        /*
         * File doesn't exist. Open the buffer read-write like Emacs -- no
         * create-directory prompt here; that moves to save time (writeout).
         * A missing directory leaves the buffer writable (access() returns
         * ENOENT, not EACCES); only a present-but-write-protected directory
         * forces READONLY.
         */
        if (access(dp, W_OK) == -1 && errno == EACCES) {
            ewprintf("File not found and directory write-protected");
            ro = TRUE;
        }
    }
```
`statbuf` may become unused in this function after the change — verify and remove
its declaration if so (avoid an unused-variable warning; the build is `-Wall
-Wextra -Werror`-clean).

### 2. Save — `writeout` (~lines 711–722)

The current missing-directory branch hard-errors:
```c
    if (stat(fn, &statbuf) == -1 && errno == ENOENT) {
        errno = 0;
        (void)xdirname(dp, fn, sizeof(dp));
        (void)strlcat(dp, "/", sizeof(dp));
        if (access(dp, W_OK) && errno == EACCES) {
            dobeep();
            ewprintf("Directory %s write-protected", dp);
            return (FIOERR);
        } else if (errno == ENOENT) {
            dobeep();
            ewprintf("%s: no such directory", dp);
            return (FIOERR);
        }
    }
```
The `else if (errno == ENOENT)` branch becomes the create prompt:
```c
        } else if (errno == ENOENT) {
            /*
             * Missing directory: Emacs asks to create it at save time
             * (find-file opened the buffer without prompting). Create the
             * tree on yes and fall through to the write; abort otherwise.
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
After a successful `do_makedir`, control falls through to the existing
`ffwopen`/`ffputbuf` write path unchanged (the directory now exists).

## Error handling / edges

- **Existing file** (open or save): the whole missing-directory branch is skipped
  (`access`/`stat` succeed) — byte-identical behavior.
- **Open, dir exists but write-protected:** buffer opens READONLY (unchanged,
  `EACCES` branch kept).
- **Save, decline the create:** existing `"<dir>: no such directory"` error, save
  aborted (nothing written, nothing created).
- **Save, `do_makedir` fails** (e.g. a path component is a file, or permission):
  `"Unable to create directory <dir>"`, save aborted.
- **Save into an existing directory:** the `writeout` ENOENT branch is skipped —
  unchanged.
- `do_makedir` runs `adjustname` + `mkdir -p` internally; `dp` here is an
  absolute path with a trailing slash (from `xdirname` + `strlcat`), which
  `do_makedir` already handles (it strips trailing slashes).

## Testing

A single-session pty test in `tests/test_editor.cpp` (the first core find-file
test there; no magit round-trips, so musl-safe):

1. Launch neomg in a fresh temp dir. `C-x C-f` a relative path `nd/f.txt` whose
   directory `nd/` does not exist.
2. **Open assertion:** the `Missing directory` prompt is **absent**, and the
   buffer opened (the new file name `f.txt` renders in the modeline). Assert by
   draining briefly and checking the accumulated output contains `f.txt` and does
   **not** contain `Missing directory`.
3. Type a character, then `C-x C-s`.
4. **Save assertion:** the `Missing directory, create` prompt appears **now**;
   send `y`; assert `Wrote` appears and — checked from the test process — the
   directory `nd/` and file `nd/f.txt` now exist on disk.
5. Drained exit via `quit_neomg`.

Also a negative check (same or a second session): with the directory already
present, `C-x C-f`/save shows no prompt (guards against the prompt firing when it
shouldn't) — optional if the first test's structure already covers the
existing-dir path implicitly.

Verify macOS (`cpp`) + Alpine/musl (`cpp-linux`, Docker) + the legacy plain-C
build (`c-legacy`, since this touches core `file.c` that the OFF build compiles).

## Out of scope

- Prompt wording / `(New file)` echo message parity.
- Any magit buffer behavior (unaffected).
- A user option to restore the old open-time prompt (YAGNI).
