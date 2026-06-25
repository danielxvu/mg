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
    sys.exit(1 if fails else 0)

if __name__ == "__main__": main()
