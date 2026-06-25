#!/usr/bin/env python3
"""
neomg status-latency benchmark harness.

Measures the cost of building a git *status view* across tools on real repos,
with proper stats (min / median / stdev over K runs). This is the reproducible
companion to the README's performance table -- run it yourself and get the same
shape of numbers on your own repos and machine.

    bench/bench.py REPO [REPO ...] [--runs N] [--tools a,b,c]
                   [--neomg-bin PATH] [--scope DIR] [--json OUT]

All measurements are READ-ONLY (git status / magit refresh / neomg status / TUI
launch); nothing mutates the repos. Tools that aren't installed (or can't be set
up, e.g. no network for the isolated Magit install) are skipped and marked n/a
rather than failing the run.

Metrics:
  - neomg     : the real engine paths via the committed `neomg_bench` --
                `full_buffer` (full status view, == Magit's work) and
                `warm_scoped` (incremental, one-dir rescan on a warm handle).
  - git       : `git status --porcelain` -- a change list, NOT a full view
                (lighter task; reference only). Default config and fsmonitor-warm.
  - magit     : real Emacs + Magit `magit-refresh` (full view; isolated install).
  - lazygit   : rough launch -> first-render via a tmux pty (a DIFFERENT metric;
    gitui       includes process startup, not just status -- labelled as such).
"""
import argparse, json, os, shutil, statistics, subprocess, sys, tempfile, time

# ---------------------------------------------------------------- stats utils

def stats(samples):
    s = sorted(x for x in samples if x is not None)
    if not s:
        return None
    return {
        "n": len(s),
        "min": round(min(s), 3),
        "median": round(statistics.median(s), 3),
        "stdev": round(statistics.stdev(s), 3) if len(s) > 1 else 0.0,
    }

def fmt(st):
    if not st:
        return "n/a"
    return f"{st['median']:.1f} ms (min {st['min']:.1f}, sd {st['stdev']:.1f}, n={st['n']})"

# ---------------------------------------------------------------- measurement

def time_cmd(cmd, runs, warm=2, cwd=None):
    """Median-able wall times (ms) for a subprocess, after `warm` warmup runs."""
    for _ in range(warm):
        subprocess.run(cmd, cwd=cwd, capture_output=True)
    out = []
    for _ in range(runs):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, cwd=cwd, capture_output=True)
        dt = (time.perf_counter() - t0) * 1000
        if r.returncode == 0:
            out.append(dt)
    return out

def current_branch(repo):
    r = subprocess.run(["git", "-C", repo, "rev-parse", "--abbrev-ref", "HEAD"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""

def first_subdir(repo):
    for e in sorted(os.listdir(repo)):
        p = os.path.join(repo, e)
        if e != ".git" and os.path.isdir(p):
            return e
    return "."

# --- neomg ------------------------------------------------------------------

def measure_neomg(repo, scope, runs, neomg_bin):
    """Run neomg_bench --raw once; it emits per-iteration `metric\tvalue` lines
    (the warm/scoped loops must stay in one process to keep the handle warm)."""
    if not neomg_bin or not os.path.exists(neomg_bin):
        return {}
    r = subprocess.run([neomg_bin, repo, scope, "--runs", str(runs), "--raw"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return {}
    buckets = {}
    for line in r.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) == 2:
            try:
                buckets.setdefault(parts[0], []).append(float(parts[1]))
            except ValueError:
                pass
    return {k: stats(v) for k, v in buckets.items()}

# --- git --------------------------------------------------------------------

def measure_git(repo, runs):
    out = {}
    out["git_default"] = stats(time_cmd(
        ["git", "-C", repo, "status", "--porcelain"], runs))
    # fsmonitor warm: warm the daemon, measure, then stop it (leave repo as-is).
    fsm = ["git", "-C", repo, "-c", "core.fsmonitor=true",
           "-c", "core.untrackedCache=true", "status", "--porcelain"]
    out["git_fsmonitor"] = stats(time_cmd(fsm, runs, warm=4))
    subprocess.run(["git", "-C", repo, "-c", "core.fsmonitor=true",
                    "fsmonitor--daemon", "stop"], capture_output=True)
    return out

# --- magit (isolated install, cached across repos) --------------------------

_MAGIT_DIR = None  # cached isolated package dir for this process

def magit_setup():
    global _MAGIT_DIR
    if _MAGIT_DIR is not None:
        return _MAGIT_DIR or None
    if not shutil.which("emacs"):
        _MAGIT_DIR = ""
        return None
    d = tempfile.mkdtemp(prefix="neomg-bench-magit-")
    install = f"""(progn
      (setq user-emacs-directory "{d}/" package-user-dir "{d}/elpa")
      (require 'package)
      (add-to-list 'package-archives '("melpa" . "https://melpa.org/packages/") t)
      (package-initialize)(package-refresh-contents)(package-install 'magit))"""
    r = subprocess.run(["emacs", "--batch", "--eval", install],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isdir(os.path.join(d, "elpa")):
        shutil.rmtree(d, ignore_errors=True)
        _MAGIT_DIR = ""
        return None
    _MAGIT_DIR = d
    return d

def magit_cleanup():
    if _MAGIT_DIR:
        shutil.rmtree(_MAGIT_DIR, ignore_errors=True)

def measure_magit(repo, runs):
    d = magit_setup()
    if not d:
        return {}
    el = f"""(progn
      (setq user-emacs-directory "{d}/" package-user-dir "{d}/elpa")
      (require 'package)(package-initialize)(require 'magit)
      (let ((default-directory "{repo}/"))
        (magit-status-setup-buffer default-directory)
        (with-current-buffer (magit-get-mode-buffer 'magit-status-mode)
          (dotimes (i {runs})
            (let ((t0 (float-time))) (magit-refresh)
              (message "SAMPLE %.3f" (* 1000 (- (float-time) t0))))))))"""
    r = subprocess.run(["emacs", "--batch", "--eval", el],
                       capture_output=True, text=True)
    samples = []
    for line in r.stderr.splitlines():
        if line.startswith("SAMPLE "):
            try:
                samples.append(float(line.split()[1]))
            except (ValueError, IndexError):
                pass
    return {"magit_refresh": stats(samples)}

# --- TUIs (rough launch -> first render via tmux) ---------------------------

def measure_tui(tool, repo, runs):
    if not shutil.which(tool) or not shutil.which("tmux"):
        return None
    marker = current_branch(repo) or ""
    samples = []
    for i in range(runs):
        sess = f"neomgbench_{tool}_{i}_{os.getpid()}"
        subprocess.run(["tmux", "kill-session", "-t", sess],
                       capture_output=True)
        t0 = time.perf_counter()
        subprocess.run(["tmux", "new-session", "-d", "-s", sess,
                        "-x", "200", "-y", "50", "-c", repo, tool],
                       capture_output=True)
        dt = None
        deadline = t0 + 12.0
        while time.perf_counter() < deadline:
            cap = subprocess.run(["tmux", "capture-pane", "-p", "-t", sess],
                                 capture_output=True, text=True)
            txt = cap.stdout
            # "rendered" = the branch name shows AND there's substantial content
            if (marker and marker in txt and len(txt.strip()) > 40):
                dt = (time.perf_counter() - t0) * 1000
                break
            time.sleep(0.02)
        subprocess.run(["tmux", "kill-session", "-t", sess], capture_output=True)
        if dt is not None:
            samples.append(dt)
    return stats(samples)

# ---------------------------------------------------------------- driver

ALL_TOOLS = ["neomg", "git", "magit", "lazygit", "gitui"]

def run_repo(repo, scope, runs, tools, neomg_bin):
    repo = os.path.abspath(repo)
    nfiles = subprocess.run(["git", "-C", repo, "ls-files"],
                            capture_output=True, text=True).stdout.count("\n")
    res = {"repo": repo, "files": nfiles, "scope": scope, "runs": runs,
           "metrics": {}}
    m = res["metrics"]
    if "neomg" in tools:
        m.update(measure_neomg(repo, scope, runs, neomg_bin))
    if "git" in tools:
        m.update(measure_git(repo, runs))
    if "magit" in tools:
        m.update(measure_magit(repo, runs))
    for t in ("lazygit", "gitui"):
        if t in tools:
            m[f"{t}_launch"] = measure_tui(t, repo, runs)
    return res

def md_table(results):
    rows = [
        ("neomg — full status view", "full_buffer", "same-basis vs Magit"),
        ("neomg — incremental (1 dir)", "warm_scoped", "Magit has no equivalent"),
        ("neomg — repo_status (list)", "repo_status", "libgit2 list only"),
        ("Emacs + Magit — full refresh", "magit_refresh", "same-basis vs neomg"),
        ("git status (change list)", "git_default", "lighter task; reference"),
        ("git status (fsmonitor warm)", "git_fsmonitor", "reference"),
        ("lazygit — launch→render", "lazygit_launch", "rough; incl. startup"),
        ("gitui — launch→render", "gitui_launch", "rough; incl. startup"),
    ]
    out = []
    for r in results:
        out.append(f"\n### {os.path.basename(r['repo'])} "
                   f"({r['files']:,} files, scope `{r['scope']}`, n={r['runs']})\n")
        out.append("| Measurement | Result | Note |")
        out.append("|---|---|---|")
        for label, key, note in rows:
            st = r["metrics"].get(key)
            out.append(f"| {label} | {fmt(st)} | {note} |")
    return "\n".join(out)

def main():
    ap = argparse.ArgumentParser(description="neomg status-latency benchmark")
    ap.add_argument("repos", nargs="+")
    ap.add_argument("--runs", type=int, default=9)
    ap.add_argument("--tools", default=",".join(ALL_TOOLS))
    ap.add_argument("--scope", default=None, help="subdir for neomg incremental scope")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--neomg-bin",
                    default=os.path.join(here, "..", "build", "tests", "neomg_bench"))
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    tools = [t.strip() for t in args.tools.split(",") if t.strip()]
    neomg_bin = os.path.abspath(args.neomg_bin) if args.neomg_bin else None
    results = []
    try:
        for repo in args.repos:
            scope = args.scope or first_subdir(os.path.abspath(repo))
            print(f"# measuring {repo} (scope {scope}) ...", file=sys.stderr)
            results.append(run_repo(repo, scope, args.runs, tools, neomg_bin))
    finally:
        magit_cleanup()

    print(md_table(results))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\n(raw results -> {args.json})", file=sys.stderr)

if __name__ == "__main__":
    main()
