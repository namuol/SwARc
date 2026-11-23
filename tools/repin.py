#!/usr/bin/env python3
"""Move recomps (game checkouts) to a new SwARc commit and check each one.

    python3 -I tools/repin.py [--ref REF] [--commit | --push] REPO...

For each REPO, one after another (builds and games are too heavy to run
side by side):

  clean    the checkout has no uncommitted changes; its old pin is an
           ancestor of the new one (moving it would not drop SwARc commits
           it has, e.g. from a branch that never landed)
  pin      ./recomp pin <sha>: SwARc at the new commit, generated files
           (install.sh, recomp, kit.yml) synced
  lift     tools/lift.sh, when the repo has one (lifter changes alter the
           generated code)
  build    cmake <build-dir> (generated files are globbed), then
           cmake --build <build-dir>
  smoke    tools/smoke.py --headless on the repo, muted, when it has
           smoke/cases.json and a game/ folder; otherwise "untested"
  commit   (--commit) "Pin SwARc <sha>" with tools/swarc.sh, the generated
           files and anything the stages changed that git tracks
  push     (--push, which implies --commit) git push origin HEAD

REF (default origin/main) is resolved once, so every repo gets the same
commit. A repo already at that commit is skipped (--force runs it anyway).
A failed stage stops that repo, leaves its changes uncommitted for a look,
and the run goes on with the next. Exit status 1 if any repo failed.

On a terminal it draws a status screen: each repo's stage, the stage's
output, and an estimate of the time left, from the time each stage took
the last runs (~/.cache/swarc/repin-times.json). Otherwise (or --plain) it
prints one line per stage, for logs and agents. Every stage's output goes
to LOG_DIR/<repo>/<stage>.log; the end prints a Markdown table, also
written to LOG_DIR/summary.md.

Options:
  --ref REF          the SwARc commit to pin (default origin/main)
  --commit           commit each repo that passed
  --push             commit and push each repo that passed
  --trailer LINE     add LINE at the end of the commit message (repeatable,
                     e.g. a Co-Authored-By line the repo asks for)
  --no-lift / --no-smoke   leave a stage out (that repo reports "skipped")
  --build-dir DIR    the configured build folder (default out/release)
  --force            run repos already at the new commit, and repos whose
                     old pin is not an ancestor of it
  --log-dir DIR      where the logs go (default a new folder in $TMPDIR)
  --plain            no status screen

Games run with <PREFIX>_HEADLESS=1 (smoke.py --headless) and
SDL_AUDIO_DRIVER=dummy unless set: no window, no sound.
"""

import argparse
import collections
import datetime
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SWARC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGES = ["clean", "pin", "lift", "build", "smoke", "commit", "push"]
# Seconds a stage takes when no run has timed it yet (an M-series Mac).
GUESS = {"clean": 2, "pin": 10, "lift": 120, "build": 240, "smoke": 90,
         "commit": 2, "push": 5}
TIMES = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"),
                     "swarc", "repin-times.json")
SYNCED = ["tools/swarc.sh", "install.sh", "recomp", ".github/workflows/kit.yml"]


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def old_pin(repo):
    """The pin HEAD has (an uncommitted change to it is not the old pin)."""
    m = re.search(r"^PIN=(\w+)", git(repo, "show", "HEAD:tools/swarc.sh"), re.M)
    if not m:
        raise RuntimeError("no PIN= in tools/swarc.sh")
    return m.group(1)


def resolve(ref, repos):
    """REF as a full SHA, from the first repo's SwARc checkout (fetched)."""
    for repo in repos:
        ext = os.path.join(repo, "external", "SwARc")
        if os.path.exists(os.path.join(ext, ".git")):
            git(ext, "fetch", "-q", "origin")
            return git(ext, "rev-parse", "--verify", ref + "^{commit}")
    git(SWARC, "fetch", "-q", "origin")
    return git(SWARC, "rev-parse", "--verify", ref + "^{commit}")


def fmt(s):
    if s is None:
        return "?"
    s = int(round(s))
    return f"{s // 60}m{s % 60:02d}s" if s >= 60 else f"{s}s"


class Times:
    """Seconds each repo's stages took, for the estimates."""

    def __init__(self):
        try:
            with open(TIMES) as f:
                self.data = json.load(f)
        except (OSError, ValueError):
            self.data = {}

    def guess(self, repo, stage):
        mine = self.data.get(repo, {}).get(stage)
        if mine is not None:
            return mine
        others = [v[stage] for v in self.data.values() if stage in v]
        return sum(others) / len(others) if others else GUESS[stage]

    def record(self, repo, stage, seconds):
        self.data.setdefault(repo, {})[stage] = round(seconds, 1)

    def save(self):
        try:
            os.makedirs(os.path.dirname(TIMES), exist_ok=True)
            with open(TIMES + ".tmp", "w") as f:
                json.dump(self.data, f, indent=1, sort_keys=True)
            os.replace(TIMES + ".tmp", TIMES)
        except OSError:
            pass


class Repo:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.name = os.path.basename(self.path.rstrip("/"))
        self.old = None
        self.stages = {}          # stage -> "wait" | "run" | "ok" | "fail" | "skip" | "n/a"
        self.notes = {}           # stage -> short text
        self.took = {}            # stage -> seconds
        self.started = None
        self.status = "waiting"   # waiting | running | passed | failed | skipped
        self.why = ""
        self.pushed = False


class Run:
    def __init__(self, args, repos, sha):
        self.args = args
        self.repos = repos
        self.sha = sha
        self.times = Times()
        self.tail = collections.deque(maxlen=200)
        self.command = ""
        self.stage_started = None
        self.current = None
        self.proc = None
        self.lock = threading.Lock()
        self.started = time.time()
        self.stopping = False
        self.plain = args.plain or not sys.stdout.isatty()
        self.env = dict(os.environ)
        self.env.setdefault("SDL_AUDIO_DRIVER", "dummy")

    # ---- planning --------------------------------------------------------
    def wanted(self, repo):
        a = self.args
        has = lambda p: os.path.exists(os.path.join(repo.path, p))
        plan = {
            "clean": "wait",
            "pin": "wait",
            "lift": "wait" if has("tools/lift.sh") and not a.no_lift else "n/a",
            "build": "wait",
            "smoke": ("wait" if has("smoke/cases.json") and has("game") and not a.no_smoke
                      else "n/a"),
            "commit": "wait" if a.commit else "n/a",
            "push": "wait" if a.push else "n/a",
        }
        if plan["lift"] == "n/a":
            repo.notes["lift"] = "--no-lift" if a.no_lift else "no tools/lift.sh"
        if plan["smoke"] == "n/a":
            repo.notes["smoke"] = ("--no-smoke" if a.no_smoke else
                                   "no game/ folder" if has("smoke/cases.json") else
                                   "no smoke/cases.json")
        return plan

    def left(self):
        """Seconds the stages not yet finished should still take."""
        total = 0.0
        for r in self.repos:
            for st in STAGES:
                state = r.stages.get(st)
                if state == "wait":
                    total += self.times.guess(r.name, st)
                elif state == "run":
                    total += max(0.0, self.times.guess(r.name, st) - (time.time() - self.stage_started))
        return total

    # ---- running a command -----------------------------------------------
    def sh(self, repo, stage, cmd):
        """Runs cmd in the repo, output to its log and the tail; returns its status."""
        self.command = " ".join(cmd)
        log = os.path.join(self.args.log_dir, repo.name, stage + ".log")
        os.makedirs(os.path.dirname(log), exist_ok=True)
        with open(log, "a") as f:
            f.write(f"$ {self.command}\n")
            f.flush()
            self.proc = subprocess.Popen(cmd, cwd=repo.path, env=self.env, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                         start_new_session=True)
            for raw in self.proc.stdout:
                line = raw.decode(errors="replace")
                f.write(line)
                for part in line.rstrip("\n").split("\r"):
                    if part.strip():
                        self.tail.append(part)
            rc = self.proc.wait()
            self.proc = None
            f.write(f"[exit {rc}]\n")
        return rc

    def last(self, n=3):
        return " | ".join(list(self.tail)[-n:])

    # ---- the stages ------------------------------------------------------
    def stage(self, repo, st):
        if st == "clean":
            if git(repo.path, "status", "--porcelain"):
                return False, "uncommitted changes"
            if repo.old == self.sha and not self.args.force:
                return None, "already at " + self.sha[:7]
            ext = os.path.join(repo.path, "external", "SwARc")
            if os.path.exists(os.path.join(ext, ".git")):
                git(ext, "fetch", "-q", "origin")
                r = subprocess.run(["git", "-C", ext, "merge-base", "--is-ancestor", repo.old, self.sha])
                if r.returncode and not self.args.force:
                    return False, f"old pin {repo.old[:7]} is not an ancestor of {self.sha[:7]}"
            return True, ""
        if st == "pin":
            rc = self.sh(repo, st, ["./recomp", "pin", self.sha])
            return rc == 0, "" if rc == 0 else self.last()
        if st == "lift":
            rc = self.sh(repo, st, ["tools/lift.sh"])
            return rc == 0, "" if rc == 0 else self.last()
        if st == "build":
            bd = self.args.build_dir
            if not os.path.isdir(os.path.join(repo.path, bd)):
                return False, f"no {bd} (configure it once first)"
            rc = self.sh(repo, st, ["cmake", bd])
            if rc == 0:
                rc = self.sh(repo, st, ["cmake", "--build", bd])
            return rc == 0, "" if rc == 0 else self.last()
        if st == "smoke":
            rc = self.sh(repo, st, [sys.executable, "-I", os.path.join(SWARC, "tools", "smoke.py"),
                                    "--headless", "."])
            m = [l for l in self.tail if re.match(r"\d+/\d+ passed", l)]
            note = m[-1] if m else ""
            return rc == 0, note if rc == 0 else (note or self.last())
        if st == "commit":
            if not git(repo.path, "status", "--porcelain"):
                return None, "nothing changed"
            git(repo.path, "add", "-u")
            for p in SYNCED:
                if os.path.exists(os.path.join(repo.path, p)):
                    git(repo.path, "add", "--", p)
            msg = f"Pin SwARc {self.sha[:7]}"
            if self.args.trailer:
                msg += "\n\n" + "\n".join(self.args.trailer)
            rc = self.sh(repo, st, ["git", "commit", "-q", "-m", msg])
            return rc == 0, git(repo.path, "rev-parse", "--short", "HEAD") if rc == 0 else self.last()
        if st == "push":
            rc = self.sh(repo, st, ["git", "push", "origin", "HEAD"])
            repo.pushed = rc == 0
            return rc == 0, "" if rc == 0 else self.last()
        raise AssertionError(st)

    def one(self, repo):
        repo.status = "running"
        repo.started = time.time()
        self.current = repo
        for st in STAGES:
            if self.stopping:
                repo.status, repo.why = "failed", "interrupted"
                break
            if repo.stages[st] == "n/a":
                continue
            repo.stages[st] = "run"
            self.stage_started = time.time()
            self.tail.clear()
            self.command = ""
            self.say(f"{repo.name}: {st} ...")
            try:
                ok, note = self.stage(repo, st)
            except Exception as e:  # a git error, a missing file
                ok, note = False, str(e)
            took = time.time() - self.stage_started
            repo.took[st] = took
            if note:
                repo.notes[st] = note
            if ok is None:  # nothing to do: the rest of the repo is skipped
                repo.stages[st] = "skip"
                for later in STAGES[STAGES.index(st) + 1:]:
                    if repo.stages[later] == "wait":
                        repo.stages[later] = "skip"
                if st == "clean":
                    repo.status, repo.why = "skipped", note
                self.say(f"{repo.name}: {st} skipped ({note}) {fmt(took)}")
                if st == "clean":
                    break
                continue
            if not ok:
                repo.stages[st] = "fail"
                for later in STAGES[STAGES.index(st) + 1:]:
                    if repo.stages[later] == "wait":
                        repo.stages[later] = "skip"
                repo.status, repo.why = "failed", f"{st}: {note}"
                self.say(f"{repo.name}: {st} FAILED after {fmt(took)}: {note}")
                log = os.path.join(self.args.log_dir, repo.name, st + ".log")
                if os.path.exists(log):
                    self.say(f"{repo.name}: log {log}")
                break
            repo.stages[st] = "ok"
            if st not in ("clean", "commit"):
                self.times.record(repo.name, st, took)
                self.times.save()
            self.say(f"{repo.name}: {st} ok {fmt(took)}" + (f" ({note})" if note else "")
                     + f"; about {fmt(self.left())} left")
        else:
            repo.status = "passed"
        self.current = None

    # ---- output ----------------------------------------------------------
    def say(self, text):
        if self.plain:
            stamp = datetime.datetime.now().strftime("%H:%M:%S")
            print(f"[{stamp}] {text}", flush=True)

    GLYPH = {"wait": "·", "run": "▶", "ok": "✓", "fail": "✗", "skip": "-", "n/a": " "}
    COLOUR = {"ok": "32", "fail": "31", "run": "33", "skip": "2", "wait": "2", "n/a": "2"}

    def screen(self):
        cols, rows = shutil.get_terminal_size((100, 30))
        c = lambda code, s: f"\033[{code}m{s}\033[0m"
        width = max(len(r.name) for r in self.repos)
        done = sum(r.status in ("passed", "failed", "skipped") for r in self.repos)
        out = [c("1", f"SwARc → {self.sha[:7]}") +
               f"   {done}/{len(self.repos)} repos   {fmt(time.time() - self.started)} so far,"
               f" about {fmt(self.left())} left",
               c("2", f"logs: {self.args.log_dir}"), "",
               " " * (width + 2) + "  ".join(s[:5].ljust(5) for s in STAGES)]
        for r in self.repos:
            cells = []
            for st in STAGES:
                state = r.stages.get(st, "n/a")
                text = self.GLYPH[state]
                if state == "run":
                    text += fmt(time.time() - self.stage_started)
                elif state in ("ok", "fail") and st in ("lift", "build", "smoke"):
                    text += fmt(r.took.get(st))
                cells.append(c(self.COLOUR[state], text.ljust(5)[:6].ljust(5)))
            tail = {"passed": c("32", "passed"), "failed": c("31", "failed: " + r.why),
                    "skipped": c("2", "skipped: " + r.why), "running": c("33", "running"),
                    "waiting": c("2", "waiting")}[r.status]
            out.append(f"{r.name.ljust(width)}  " + "  ".join(cells) + "  " + tail)
        if self.current:
            out += ["", c("1", f"{self.current.name}") + c("2", f"  $ {self.command}")]
            room = max(3, rows - len(out) - 2)
            out += [c("2", "  " + l) for l in list(self.tail)[-room:]]
        out = [l if len(re.sub(r"\033\[[\d;]*m", "", l)) <= cols else
               l[:cols + (len(l) - len(re.sub(r"\033\[[\d;]*m", "", l))) - 1] + "\033[0m…"
               for l in out]
        sys.stdout.write("\033[H\033[2J" + "\n".join(out[:rows - 1]) + "\n")
        sys.stdout.flush()

    def summary(self):
        lines = ["| repo | pin | lift | build | smoke | commit | pushed |",
                 "|---|---|---|---|---|---|---|"]
        for r in self.repos:
            def cell(st):
                state = r.stages.get(st, "n/a")
                note = r.notes.get(st, "")
                word = {"ok": "ok", "fail": "FAILED", "skip": "skipped", "n/a": "—",
                        "wait": "not run", "run": "interrupted"}[state]
                if st in ("lift", "build", "smoke") and state in ("ok", "fail"):
                    word += f" ({fmt(r.took.get(st))})"
                return f"{word}: {note}" if note else word
            pin = f"{(r.old or '?')[:7]} → {self.sha[:7]}"
            if r.status == "skipped":
                pin += f" (skipped: {r.why})"
            elif r.stages.get("clean") == "fail":
                pin += f" (not moved: {r.notes.get('clean', '')})"
            pushed = "yes" if r.pushed else (r.why if r.status == "failed" else "no")
            lines.append(f"| {r.name} | {pin} | {cell('lift')} | {cell('build')} | {cell('smoke')}"
                         f" | {cell('commit')} | {pushed} |")
        return "\n".join(l.replace("\n", " ") for l in lines)

    # ---- main ------------------------------------------------------------
    def go(self):
        for r in self.repos:
            try:
                r.old = old_pin(r.path)
            except RuntimeError as e:
                r.old = None
                r.stages = {st: "n/a" for st in STAGES}
                r.stages["clean"] = "fail"
                r.notes["clean"] = str(e)
                r.status, r.why = "failed", str(e)
                continue
            r.stages = self.wanted(r)

        worker = threading.Thread(target=self.work, daemon=True)
        worker.start()
        if not self.plain:
            sys.stdout.write("\033[?25l")
        try:
            while worker.is_alive():
                if not self.plain:
                    self.screen()
                worker.join(0.5)
        except KeyboardInterrupt:
            self.stopping = True
            p = self.proc
            if p:
                try:
                    os.killpg(p.pid, signal.SIGTERM)
                except OSError:
                    pass
            worker.join(10)
        finally:
            if not self.plain:
                self.screen()
                sys.stdout.write("\033[?25h\n")
        table = self.summary()
        with open(os.path.join(self.args.log_dir, "summary.md"), "w") as f:
            f.write(table + "\n")
        print(table)
        print(f"\n{fmt(time.time() - self.started)} in all; logs in {self.args.log_dir}")
        return 1 if any(r.status == "failed" for r in self.repos) or self.stopping else 0

    def work(self):
        for r in self.repos:
            if r.status == "failed":
                continue
            self.one(r)
            if self.stopping:
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 epilog="See the top of tools/repin.py for the stages.")
    ap.add_argument("repos", nargs="+", help="recomp checkouts (folders with tools/swarc.sh)")
    ap.add_argument("--ref", default="origin/main")
    ap.add_argument("--commit", action="store_true")
    ap.add_argument("--push", action="store_true")
    ap.add_argument("--trailer", action="append", default=[])
    ap.add_argument("--no-lift", action="store_true")
    ap.add_argument("--no-smoke", action="store_true")
    ap.add_argument("--build-dir", default="out/release")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--log-dir")
    ap.add_argument("--plain", action="store_true")
    args = ap.parse_args()
    if args.push:
        args.commit = True
    for p in args.repos:
        if not os.path.isfile(os.path.join(p, "tools", "swarc.sh")):
            ap.error(f"{p}: not a recomp checkout (no tools/swarc.sh)")
    names = [os.path.basename(os.path.abspath(p)) for p in args.repos]
    if len(set(names)) != len(names):
        ap.error("two repos with the same folder name")
    args.log_dir = os.path.abspath(args.log_dir or tempfile.mkdtemp(prefix="swarc-repin-"))
    os.makedirs(args.log_dir, exist_ok=True)
    try:
        sha = resolve(args.ref, [os.path.abspath(p) for p in args.repos])
    except RuntimeError as e:
        sys.exit(f"repin: {e}")
    sys.exit(Run(args, [Repo(p) for p in args.repos], sha).go())


if __name__ == "__main__":
    main()
