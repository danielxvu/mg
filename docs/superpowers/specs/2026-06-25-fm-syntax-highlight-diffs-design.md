# FM-SYNTAX-HIGHLIGHT — syntax-highlighted diffs (Zig tokenizer + per-cell color)

## Why

neomg's diff surfaces (the `*magit-status*` inline hunks, the `*magit-commit*`
diff, the ediff panes) render monochrome. Sublime Merge / delta / Magit all
syntax-highlight diffs, and it's the most visible UX gap when reading code in
neomg. Closing it is high-payoff and builds directly on the per-cell color
mechanism the ediff work already introduced.

Two facts shape the whole design:

- **mg has no color** — `ttcolor()` only toggles termcap reverse-video
  (`enter/exit_standout_mode`); a `vtcell` is an int holding a codepoint plus a
  single `MG_HL_BIT` standout flag. So this feature must *add real terminal
  color* (terminfo `set_a_foreground`) to a codebase that today only does
  standout. That is the foundational cost.
- **diff lines already carry their file `path`** (`MG_LINE_DIFF` emits it), so
  per-line language detection by extension is feasible with no new plumbing.

## Decisions (settled during brainstorming)

- **Highlighter:** a bundled, in-tree **lite tokenizer written in Zig**, compiled
  to a static library with a C ABI and linked into the magit engine. (Not
  tree-sitter — too heavy for a micro-editor; not shelling out to bat/delta — a
  runtime dep + ANSI-parse fragility.) Zig is chosen for its trivial C interop
  and because a tokenizer is pure computation with none of the git-format
  correctness-tail risk that ruled out a Zig *read engine*.
- **Color depth:** **16-color via terminfo `set_a_foreground`** — portable, fits
  mg's termcap model, auto-themes to the user's terminal (legible by
  construction), sufficient for a fixed token palette. The per-cell color field
  abstracts the palette, so a 256/truecolor emit path is a clean later upgrade.
- **Scope:** **all diff surfaces** — inline status diffs, the commit-view diff,
  **and the ediff panes** (the ediff reconciliation is the trickiest part; see
  below).
- **When color is computed:** **at display-update time** (Approach A), hooking
  where ediff already injects per-cell attributes — no buffer-model change.
- **Build:** **Zig 0.16.0**, pinned (installed via MacPorts: `/opt/local/bin/zig`)
  — the build invocation and language/ABI syntax target this version. `zig` joins
  `makedepends` / Dockerfiles / CI; the OFF build compiles and links none of it.
  Local reference for implementers: `~/src/zig-docs/` holds the 0.16.0 language
  reference (`langref-0.16.0.html`) and a symlink to the version-matched stdlib
  source (`std/` → `/opt/local/lib/zig/std`; also browsable via `zig std`).

## Architecture — three separated layers

### 1. Zig tokenizer (new; built in a git worktree)

A standalone library under `syntax/` exposing a C ABI via a generated/hand-written
`neomg_syntax.h`:

```c
typedef struct { uint16_t start; uint16_t len; uint8_t kind; } NeomgSpan;
/* token kinds */
enum { NEOMG_NORMAL=0, NEOMG_KEYWORD, NEOMG_STRING, NEOMG_COMMENT,
       NEOMG_NUMBER, NEOMG_TYPE, NEOMG_FUNCTION, NEOMG_PUNCT };

uint8_t neomg_lang_from_path(const char *path);          /* extension -> lang id */
size_t  neomg_highlight_line(uint8_t lang, const char *text, size_t len,
                             NeomgSpan *out, size_t cap); /* -> span count */
```

- Pure, allocation-free (caller-provided span buffer), no dependencies.
- **Line-oriented** (a diff shows fragments; cross-line string/comment state is
  best-effort — inherent to diffs).
- A **generic** lexer (strings, `//` and best-effort `/* */`, numbers,
  punctuation) plus compiled-in keyword/type tables for **C/C++, JS/TS, Python,
  Rust, Go, shell, JSON, CSS, HTML**. An unknown extension falls back to generic.
- UTF-8 safe (operates on bytes; spans are byte offsets; never splits a
  multibyte sequence).
- Tested with Zig's own test runner (per-language token cases, generic fallback,
  UTF-8, span-bound safety).

### 2. Display color layer (`display.c` / `tty.c`, all `ENABLE_NATIVE_MAGIT`-gated)

- **`vtcell` gains a 4-bit color-index field** in free high bits. `MG_HL_BIT`
  (reverse-video) stays a *separate, orthogonal* bit; the codepoint occupies the
  low ~21 bits.
- **`ttfgcolor(idx)`** emits the terminal's `set_a_foreground` for a fixed
  palette and resets to default appropriately; emits nothing when the terminal
  declares no color capability.
- The fixed palette maps token kind → terminfo color number (e.g. keyword=blue,
  string=green, comment=brightblack/grey, number=cyan, type=yellow,
  function=magenta, punct=default).
- **`cellcolor`/`uline` compose two orthogonal attributes** per cell: foreground
  color (from the color-index) **and** reverse-video (from `MG_HL_BIT`). Handled
  in both the full-redraw and incremental-update paths as needed.

### 3. Glue (the display-update hook + `magit_cmd.c`)

- When a magit **diff line** is drawn, determine its language from the file
  `path` the buffer tracks for that line, call `neomg_highlight_line` on the line
  text *after* the `+/-/space` origin column, and OR the per-kind color-index
  into the corresponding cells. The `+/-` gutter retains an add/del cue; tokens
  within the line get syntax colors (the delta/Magit convention).
- **ediff panes:** the same tokenize-and-color call, with the result *composed*
  with the active-region reverse-video — so an active-region cell renders as
  syntax-foreground **+** reverse, not one or the other. This is the reconciliation
  the "everything incl. ediff" scope requires.

## The ediff reconciliation (the hard part, called out)

ediff today sets `MG_HL_BIT` (reverse-video) on the active conflict region and on
word-refinement runs. Syntax color uses the new color-index field. Because they
are **different SGR attributes** (foreground color vs reverse), they compose: the
render layer emits `set_a_foreground <kind>` *and* standout for a cell that is
both syntax-colored and in the active region. The risk is purely in the
render-layer bookkeeping (tracking/emitting two attributes and resetting both);
the cell model already has room for both bits.

## Testing

- **Zig:** unit tests per language (keywords / strings / line+block comments /
  numbers), the generic fallback, UTF-8 safety, and span-bound safety.
- **C glue:** a doctest asserting that a known diff line in a known language maps
  to the expected per-cell color-index sequence (tokenizer → cell mapping),
  terminal-independent.
- **pty/tmux:** visual confirmation that status / commit / ediff render colored
  diffs, and that the ediff active region still reads correctly (syntax-fg +
  reverse composed).
- **OFF build:** 0 magit/zig symbols, no color escapes — everything gated.
- Cross-platform: macOS `ctest --preset cpp`, the Arch/Alpine containers, and the
  OFF (`c-legacy`) build all green.

## Scope / YAGNI

- 16 terminfo colors, fixed ~8-kind palette; no theming UI (256/truecolor is a
  later emit-layer upgrade via the same cell field).
- Line-oriented tokenizing only (cross-hunk multi-line state is best-effort).
- Languages are compiled-in tables; no per-language user config.
- Compute-at-render with no cache in v1; a per-line memo cache is a follow-up
  only if profiling shows redraw cost.

## Build / dependency impact

- `zig` (**0.16.0**) becomes a build dependency: CMake invokes `zig build-lib`
  to produce the static library and links it into `mg_magit`. Added to the AUR
  `makedepends`, the Arch/Alpine Dockerfiles, and CI. The Dockerfiles install
  `zig` from each distro's rolling repos (`pacman -S zig` on Arch,
  `apk add zig` on Alpine edge) — 0.16.0 at time of writing, but not pinned.
  A commented-out tarball fallback is provided in each Dockerfile for anyone
  who needs a hard pin. The Zig sources target 0.16.0's syntax/ABI.
- The OFF/upstream build neither compiles nor links the Zig library or any color
  code — verified by the 0-symbol check.

## Phases

1. ✅ **Zig tokenizer + C ABI + Zig tests** (in a worktree), and the CMake
   integration that builds + links it. No rendering yet; validated by the Zig
   tests and a tiny C smoke test that calls the ABI. _(Task 1 + Task 2, branch
   `fm-syntax-highlight`)_
2. ✅ **Display color layer**: the `vtcell` color-index field, `ttfgcolor` via
   terminfo, and `cellcolor`/`uline` composing color + reverse. Validated by the
   C doctest (cell-mapping) and a non-magit smoke. _(Task 3)_
3. ✅ **Glue for the status + commit diff surfaces**: tokenize-and-color diff
   lines; pty-verify colored diffs. _(Task 4)_
4. ✅ **ediff reconciliation**: compose syntax color with the active-region
   reverse-video; pty-verify the conflict view. _(Task 5)_

Each phase: macOS + Arch/Alpine + the OFF build green.

**Build-dep closure (Task 6):** `zig` added to `docker/Dockerfile.arch`,
`docker/Dockerfile.alpine`, `.github/workflows/bench.yml`, and
`packaging/aur/PKGBUILD`. Arch ships `extra/zig 0.16.0`; Alpine edge ships
`zig-0.16.0-r1`; both are exact-version matches so the distro package is used
directly. A commented fallback tarball URL (`zig-x86_64-linux-0.16.0.tar.xz`)
is left in each Dockerfile for the day either distro lags. **FM-SYNTAX-HIGHLIGHT
complete.**
