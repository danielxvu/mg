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
