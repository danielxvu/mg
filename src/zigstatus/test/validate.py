#!/usr/bin/env python3
# Byte-for-byte validation: build a repo in a known state, compare `zigstatus`
# to `git status --porcelain` (normalized: sorted "XY path" lines).
import subprocess, sys, tempfile, os, shutil

ZIG = "/opt/local/bin/zig"
BIN = os.path.join(os.path.dirname(__file__), "..", "zig-out", "bin", "zigstatus")

def sh(cwd, *args): subprocess.run(args, cwd=cwd, check=True, capture_output=True)

def git_status(repo):
    out = subprocess.run(["git", "-C", repo, "status", "--porcelain"],
                         capture_output=True, text=True).stdout
    return sorted(l for l in out.splitlines() if l)

def zig_status(repo):
    out = subprocess.run([os.path.abspath(BIN), repo], capture_output=True, text=True).stdout
    return sorted(l for l in out.splitlines() if l)

def make_repo(setup):
    d = tempfile.mkdtemp(prefix="zigstatus-")
    sh(d, "git", "init", "-q"); sh(d, "git", "config", "user.email", "t@e"); sh(d, "git", "config", "user.name", "t")
    setup(d)
    return d

CASES = {}
def case(fn): CASES[fn.__name__] = fn; return fn

@case
def untracked(d):
    open(os.path.join(d, "tracked.txt"), "w").write("x\n")
    sh(d, "git", "add", "tracked.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "new1.txt"), "w").write("a\n")
    open(os.path.join(d, "new2.md"), "w").write("b\n")

@case
def modified(d):
    open(os.path.join(d, "a.txt"), "w").write("one\n")
    sh(d, "git", "add", "a.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("one\ntwo\n")  # worktree change

@case
def modified_same_size(d):  # racy: same byte length, different content
    open(os.path.join(d, "a.txt"), "w").write("aaaa\n")
    sh(d, "git", "add", "a.txt"); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("bbbb\n")

@case
def deleted(d):
    open(os.path.join(d, "a.txt"), "w").write("x\n")
    open(os.path.join(d, "b.txt"), "w").write("y\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    os.remove(os.path.join(d, "b.txt"))  # tracked, deleted from worktree

@case
def untracked_dir(d):
    open(os.path.join(d, "x.txt"), "w").write("x\n")
    sh(d, "git", "add", "x.txt"); sh(d, "git", "commit", "-qm", "init")
    os.makedirs(os.path.join(d, "sub/deep"))
    open(os.path.join(d, "sub/deep/f.txt"), "w").write("f\n")  # untracked tree

@case
def ignored_file(d):
    open(os.path.join(d, ".gitignore"), "w").write("*.log\nbuild/\n")
    open(os.path.join(d, "keep.txt"), "w").write("k\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "debug.log"), "w").write("noise\n")   # ignored -> absent
    open(os.path.join(d, "real.txt"), "w").write("r\n")        # untracked -> ??
    os.makedirs(os.path.join(d, "build")); open(os.path.join(d, "build/o.o"), "w").write("x")  # ignored dir -> absent

@case
def mixed(d):  # modified + deleted + untracked + ignored together
    for n in ("a.txt", "b.txt", "c.txt"): open(os.path.join(d, n), "w").write("x\n")
    open(os.path.join(d, ".gitignore"), "w").write("*.tmp\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.txt"), "w").write("x\nmod\n")     # _M
    os.remove(os.path.join(d, "b.txt"))                        # _D
    open(os.path.join(d, "u.txt"), "w").write("u\n")          # ??
    open(os.path.join(d, "skip.tmp"), "w").write("i\n")       # ignored

@case
def nested_ignore(d):  # a nested .gitignore re-includes via negation
    os.makedirs(os.path.join(d, "pkg"))
    open(os.path.join(d, ".gitignore"), "w").write("*.gen\n")
    open(os.path.join(d, "pkg/.gitignore"), "w").write("!keep.gen\n")
    open(os.path.join(d, "pkg/x.txt"), "w").write("x\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    open(os.path.join(d, "a.gen"), "w").write("g\n")          # ignored by root *.gen
    open(os.path.join(d, "pkg/keep.gen"), "w").write("k\n")   # re-included -> ??

# --- fail-closed cases ----------------------------------------------------
# States the v2-only walker cannot model faithfully: it MUST exit non-zero (the
# -1 sentinel) so the C++ caller falls back to libgit2, rather than print wrong
# output. Asserted on the exit code, not on byte-equality with `git status`.
FAILCLOSED = {}
def failclosed(fn): FAILCLOSED[fn.__name__] = fn; return fn

def zig_rc(repo):
    return subprocess.run([os.path.abspath(BIN), repo],
                          capture_output=True, text=True).returncode

@failclosed
def conflict(d):  # merge conflict -> stage 1/2/3 entries in the index
    open(os.path.join(d, "a.txt"), "w").write("base\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    base = subprocess.run(["git", "-C", d, "rev-parse", "--abbrev-ref", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    sh(d, "git", "checkout", "-qb", "feature")
    open(os.path.join(d, "a.txt"), "w").write("feature\n")
    sh(d, "git", "commit", "-qam", "feature")
    sh(d, "git", "checkout", "-q", base)
    open(os.path.join(d, "a.txt"), "w").write("master\n")
    sh(d, "git", "commit", "-qam", "master")
    # merge conflicts; git returns nonzero, so don't use sh() (which checks).
    subprocess.run(["git", "merge", "feature"], cwd=d, capture_output=True)

@failclosed
def index_v4(d):  # path-prefix-compressed index version 4
    open(os.path.join(d, "a.txt"), "w").write("x\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    sh(d, "git", "update-index", "--index-version", "4")

@failclosed
def split_index(d):  # v2 header + "link" extension -> entries live in shared file
    open(os.path.join(d, "a.txt"), "w").write("x\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    sh(d, "git", "update-index", "--split-index")

@failclosed
def submodule(d):  # gitlink entry (mode 0160000) -> nested repo, not worktree
    # Build the submodule source as a sibling of d so it's cleaned with the tree.
    up = os.path.join(os.path.dirname(d), os.path.basename(d) + "-sub")
    os.makedirs(up)
    sh(up, "git", "init", "-q"); sh(up, "git", "config", "user.email", "t@e"); sh(up, "git", "config", "user.name", "t")
    open(os.path.join(up, "f.txt"), "w").write("x\n")
    sh(up, "git", "add", "."); sh(up, "git", "commit", "-qm", "up")
    open(os.path.join(d, "a.txt"), "w").write("base\n")
    sh(d, "git", "add", "."); sh(d, "git", "commit", "-qm", "init")
    sh(d, "git", "-c", "protocol.file.allow=always", "submodule", "add", up, "sub")

def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    fails = 0
    for name, setup in CASES.items():
        if only and name != only: continue
        d = make_repo(setup)
        try:
            g, z = git_status(d), zig_status(d)
            if g == z: print(f"ok   {name}")
            else:
                fails += 1; print(f"FAIL {name}\n  git: {g}\n  zig: {z}")
        finally: shutil.rmtree(d, ignore_errors=True)
    for name, setup in FAILCLOSED.items():
        if only and name != only: continue
        d = make_repo(setup)
        try:
            rc = zig_rc(d)
            if rc != 0: print(f"ok   {name} (fail-closed rc={rc})")
            else:
                fails += 1; print(f"FAIL {name}: expected nonzero exit (fall back), got rc=0")
        finally:
            shutil.rmtree(d, ignore_errors=True)
            shutil.rmtree(d + "-sub", ignore_errors=True) # submodule source, if any
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
