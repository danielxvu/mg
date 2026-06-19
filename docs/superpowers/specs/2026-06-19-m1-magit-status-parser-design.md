# M1 — `mg.magit` working-tree status parser

**Status:** approved 2026-06-19 · **Task:** M1 (first real feature module) ·
**Branch:** `cpp-refactor`

## Goal

The first genuinely modern-C++ feature module: a greenfield, pure-logic parser
for `git status --porcelain=v1` output, with zero coupling to mg's C core. It
validates the live C++20 module + doctest pipeline (from F3) on real,
editor-relevant work, and lays the value-type foundation the later coroutine
poller (M2) and `extern "C"` modeline bridge will build on.

## Scope

**In:** value types for a file's status, a line parser, and a whole-output
parser, all error-reported via `std::expected`. Ranges/views used internally for
splitting. Tested with hardcoded porcelain fixtures (deterministic, no live git).

**Out (explicit follow-ups):** C-quoted / special-character paths, the `-z` NUL
format, *invoking* git (M2's coroutine poller), and the `extern "C"` bridge to
mg's modeline. Commit-DAG (`git log`) and refs parsing are later M-series slices.

## Module & build

- `export module mg.magit;` at `src/magit/magit.cppm`.
- Built as a module **library** `mg_magit` (so the future bridge can link it),
  with a `test_magit` doctest binary that `import mg.magit;`.
- Gated by `ENABLE_NATIVE_MAGIT` (which already requires `ENABLE_CPP_UPGRADES`).
  The `cpp` CMake preset enables it for dev builds.

## Value types (`namespace mg::magit`)

```cpp
enum class status : char {
    unmodified = ' ', modified = 'M', added = 'A', deleted = 'D',
    renamed = 'R', copied = 'C', unmerged = 'U', untracked = '?', ignored = '!',
};

struct file_status {
    status index;                          // X — staged / index side
    status worktree;                       // Y — unstaged / working-tree side
    std::string path;
    std::optional<std::string> orig_path;  // set for renames/copies (orig -> path)
};

struct parse_error { std::string message; std::string line; };
```

## API

```cpp
std::expected<file_status, parse_error>
parse_status_line(std::string_view line);

std::expected<std::vector<file_status>, parse_error>
parse_status(std::string_view porcelain);   // splits on '\n', short-circuits on
                                             // the first malformed line
```

`parse_status` ignores trailing/empty lines and propagates the first
`parse_error` from `parse_status_line` via monadic chaining.

## Porcelain v1 grammar (the subset we parse)

```
XY␠PATH                     e.g. " M src/foo.c", "MM x", "A  y", "?? z", "D  w"
XY␠ORIG␠->␠PATH             e.g. "R  old.c -> new.c"   (rename/copy)
```

- `X` = index status, `Y` = worktree status (either may be a space).
- `??` = untracked, `!!` = ignored (both columns same char).
- A valid line is ≥ 4 chars: two status chars, a space, then a non-empty path.

## TDD slice order (each = one red→green)

1. ` M src/foo.c` → `{unmodified, modified, "src/foo.c", nullopt}`
2. `MM src/baz.c` → staged **and** unstaged modified
3. `?? new.txt` → `{untracked, untracked, "new.txt"}`
4. `R  old.c -> new.c` → `path="new.c"`, `orig_path="old.c"`
5. multi-line whole-output → `vector<file_status>` (via `parse_status`)
6. malformed (too short / unknown code) → `std::unexpected(parse_error)`

## Testing

doctest via `mg_add_module_test`/a `test_magit` target; fixtures are inline
porcelain strings drawn from real `git status --porcelain` shapes.
