#!/usr/bin/env python3
"""Smoke tests for recomps built on SwARc: scripted runs with pass/fail checks.

    python3 -I tools/smoke.py [-j N] [--update] [--only NAME[,NAME]] REPO...

Each REPO (a recomp's checkout) lists its cases in smoke/cases.json:

    {"run": "tools/try-run.sh", "prefix": "TIBSUN",
     "cases": [{"name": "skirmish", "input": "smoke/skirmish.txt",
                "seconds": 45, "min_frames": 100}]}

`run` and `prefix` can be set per case too. A case runs the game in the
background with <PREFIX>_TEST_INPUT (the run script rebuilds first). The
input script should wait for each screen ("wait": the platform's
TEST_INPUT, e.g. win32's user32_window.cpp) and end the run itself
("exit"), so a case takes as long as the game does; `seconds` is then
only a timeout. See docs/smoke-tests.md for writing fast, steady cases.

A case fails if:
  - the build failed, or the game exited before `seconds` (unless the
    script's own "exit" ended it, or "exits": true);
  - the script has an "exit" that never came, a "wait" that timed out (the
    pixels it found are reported), or a bad line;
  - the run log shows a crash or fatal error (both save a report);
  - fewer than `min_frames` frames were presented;
  - the input script's "shot <shot>" frame is missing, or differs from the
    reference smoke/ref/<name>.bmp by more than `threshold` (default 0.10:
    the share of sampled pixels that differ clearly), or is blank (one colour).
    "region": [x0, y0, x1, y1] (fractions of the frame) compares only that
    part: the sidebar of a match whose start position is random, say;
  - its "check" (a command; "{out}" is the case's result folder, holding
    the frames kept with "shot_every": N) exits non-zero.
Other case keys: "args" (game arguments, after "--"), "env" (variables for
the game, "{prefix}" filled in: <PREFIX>_TEST_HIDE hides files from the
game, e.g. intro videos), "fresh_data": true (new per-user data each run,
as $XDG_DATA_HOME) and "data" (a folder copied in as that data first:
settings that skip a first-run screen; "data" at the top level is every
case's default).

Repo-level keys for running a repo's cases side by side:
  "build": a command (list) that builds what every case runs. smoke.py
     runs it once, before the repo's cases, and then sets
     <PREFIX>_NO_BUILD=1: the run script must skip its own build then.
  "parallel": true: the cases may run at once. The run script must write
     its log to $<PREFIX>_RUN_LOG (smoke.py passes the case's own), and
     each case gets its own per-user data ($XDG_DATA_HOME, "data" copied
     in), so no case sees another's settings or saves.
Without "parallel" a repo's cases run one after another and the log is
read from build/run.log.
--headless runs the games with <PREFIX>_HEADLESS=1: their windows are never
shown, so runs never take the focus (frames, waits and shots work as shown).
A case with no reference saves its shot as the reference (NEW: look at it).
--update replaces the references with this run's shots. References are the
game's own pixels: keep smoke/ref/ out of git.

-j N runs up to N games at once (cases of repos without "parallel" still
one after another). Games then load slower, so timeouts (`seconds` and the
waits') are multiplied by the number running at once: a pass costs no
more, a failure takes longer. Timings: docs/smoke-timings.md.

Runs are muted (SDL_AUDIO_DRIVER=dummy) unless SDL_AUDIO_DRIVER is set.

Results go to REPO/build/smoke/<name>/ (shot, run log, check output). Exit
status 1 if any case failed.
"""

import argparse
import concurrent.futures
import re
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time

SAMPLE = 4        # compare every 4th pixel in each direction
CLEAR_DIFF = 48   # a sampled pixel differs clearly past this (max channel delta)


def read_bmp(path):
    """(width, height, rows): rows top-down, each a list of (r, g, b)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError(f"{path}: {bpp} bpp")
    step = bpp // 8
    pitch = (w * step + 3) & ~3
    top_down = h < 0
    h = abs(h)
    rows = []
    for y in range(0, h, SAMPLE):
        src = y if top_down else h - 1 - y
        row = data[off + src * pitch: off + src * pitch + w * step]
        b, g, r = row[0::step * SAMPLE], row[1::step * SAMPLE], row[2::step * SAMPLE]
        rows.append(list(zip(r, g, b)))
    return w, h, rows


def crop(rows, region):
    """`region` [x0, y0, x1, y1] as fractions of the frame (None: all of it)."""
    if not region:
        return rows
    x0, y0, x1, y1 = region
    n = len(rows)
    m = len(rows[0]) if rows else 0
    return [r[int(x0 * m):int(x1 * m)] for r in rows[int(y0 * n):int(y1 * n)]]


def compare(shot, ref, region=None):
    """Share of sampled pixels that differ clearly (1.0 if the sizes differ)."""
    sw, sh, a = read_bmp(shot)
    rw, rh, b = read_bmp(ref)
    if (sw, sh) != (rw, rh):
        return 1.0, f"size {sw}x{sh}, reference {rw}x{rh}"
    a, b = crop(a, region), crop(b, region)
    total = differ = 0
    for ra, rb in zip(a, b):
        for pa, pb in zip(ra, rb):
            total += 1
            if max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2])) > CLEAR_DIFF:
                differ += 1
    return differ / max(total, 1), ""


def blank(path):
    """True if the frame is (almost) one colour: a black loading screen, say."""
    _, _, rows = read_bmp(path)
    counts = {}
    for row in rows:
        for px in row:
            counts[px] = counts.get(px, 0) + 1
    total = sum(counts.values())
    return max(counts.values()) / max(total, 1) > 0.97


def shot_name(input_path):
    with open(input_path) as f:
        for line in f:
            parts = line.split("#")[0].split()
            if len(parts) >= 3 and parts[1] == "shot":
                return parts[2]
    return None


def script_exits(input_path):
    """True if the input script ends the run itself ("exit")."""
    with open(input_path) as f:
        return any(line.split("#")[0].split()[1:2] == ["exit"] for line in f)


def run_case(repo, defaults, case, update, scale=1, headless=False):
    name = case["name"]
    prefix = case.get("prefix", defaults.get("prefix"))
    script = case.get("run", defaults.get("run", "tools/try-run.sh"))
    # With N repos at once each game loads slower: timeouts (the case's and
    # its waits', <PREFIX>_TEST_WAIT_SCALE) are scaled by N.
    seconds = int(case["seconds"]) * scale
    inp = os.path.join(repo, case["input"])
    out = os.path.join(repo, "build", "smoke", name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    env = dict(os.environ)
    # Muted (SDL's dummy audio driver) unless SDL_AUDIO_DRIVER is set.
    env.setdefault('SDL_AUDIO_DRIVER', 'dummy')
    # Per-user data (saves, settings) made anew for each run, from the
    # "data" folder if given (copied as $XDG_DATA_HOME), so no earlier run,
    # play session or case running alongside changes what the game shows.
    # Cases of a "parallel" repo always get their own.
    seed = case.get("data", defaults.get("data"))
    if case.get("fresh_data") or seed or defaults.get("parallel"):
        data = os.path.join(out, "data")
        if seed:
            shutil.copytree(os.path.join(repo, seed), data)
        else:
            os.makedirs(data)
        env["XDG_DATA_HOME"] = data
    # "env": variables for the game, "{prefix}" filled in (e.g.
    # {"{prefix}_TEST_HIDE": "resource\\video\\intro\\"}: logo videos a game
    # finds by listing a folder are left out).
    for k, v in case.get("env", {}).items():
        env[k.replace("{prefix}", prefix)] = v
    ends = script_exits(inp)
    env.update({f"{prefix}_TEST_INPUT": inp, f"{prefix}_SHOT_DIR": out,
                f"{prefix}_BACKGROUND": "1", f"{prefix}_TEST_WAIT_SCALE": str(scale)})
    # --headless: the game's windows are never shown (<PREFIX>_HEADLESS), so
    # a run never takes the focus; frames, waits and shots work as before.
    if headless:
        env[f"{prefix}_HEADLESS"] = "1"
    # The run's log: the case's own if the run script takes one (a
    # "parallel" repo's must), else the repo's build/run.log.
    log_path = os.path.join(repo, "build", "run.log")
    if defaults.get("parallel"):
        log_path = os.path.join(out, "run.log")
        env[f"{prefix}_RUN_LOG"] = log_path
    if defaults.get("build"):  # built once before the cases (run_repo_build)
        env[f"{prefix}_NO_BUILD"] = "1"
    if case.get("shot_every"):  # frames the "check" looks at
        env[f"{prefix}_SHOT_EVERY"] = str(case["shot_every"])
    elif not ends:  # frames counted from the saved ones; an exit logs the count
        env[f"{prefix}_SHOT_EVERY"] = "50"
    started = time.time()
    args = case.get("args", [])
    proc = subprocess.run([os.path.join(repo, script), str(seconds)] + (["--"] + args if args else []),
                          cwd=repo, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    took = time.time() - started
    with open(os.path.join(out, "try-run.txt"), "w") as f:
        f.write(proc.stdout)
    log = ""
    if os.path.exists(log_path):
        if log_path != os.path.join(out, "run.log"):
            shutil.copy(log_path, os.path.join(out, "run.log"))
        with open(log_path, errors="replace") as f:
            log = f.read()

    problems, notes = [], []
    if "BUILD FAILED" in proc.stdout:
        problems.append("build failed")
    elif "exited with" in proc.stdout and not case.get("exits"):
        status_code = proc.stdout.split("exited with")[1].split()[0]
        if not (ends and status_code == "0" and "TEST_INPUT: exit" in log):
            problems.append("game exited early (" + status_code + ")")
    elif ends and "TEST_INPUT: exit" not in log:
        problems.append(f"the script's exit never came within {seconds} s")
    for m in re.finditer(r"TEST_INPUT: wait \(line (\d+)\) timed out after ([\d.]+) s; the frame has(.*)", log):
        problems.append(f"wait at line {m.group(1)} timed out ({m.group(2)} s):{m.group(3)}")
    if "TEST_INPUT: bad" in log:
        problems.append("the input script has a bad line (see run.log)")
    waits = [f"L{m.group(1)} {float(m.group(2)):.1f}s"
             for m in re.finditer(r"TEST_INPUT: wait \(line (\d+)\) matched after ([\d.]+) s", log)]
    if waits:
        notes.append("waits " + ", ".join(waits))
    if "report saved to" in log:  # every crash and fatal error saves one
        problems.append("crash or fatal error (report in run.log)")
    frames = [int(f[6:11]) for f in os.listdir(out) if f.startswith("frame_") and f.endswith(".bmp")]
    presented = max(frames, default=0)
    m = re.search(r"TEST_INPUT: exit at [\d.]+ s, (\d+) frames presented", log)
    if m:
        presented = int(m.group(1))
    if presented < case.get("min_frames", 1):
        problems.append(f"~{presented} frames presented, want {case.get('min_frames', 1)}")
    if case.get("check") and not problems:
        cmd = [a.replace("{out}", out) for a in case["check"]]
        chk = subprocess.run(cmd, cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        with open(os.path.join(out, "check.txt"), "w") as f:
            f.write(chk.stdout)
        if chk.returncode != 0:
            last = [l for l in chk.stdout.splitlines() if l.strip()][-1:] or ["(no output)"]
            problems.append(f"check failed: {last[0]}")
    for f in os.listdir(out):  # the frame counting needs no pixels kept
        if f.startswith("frame_"):
            os.remove(os.path.join(out, f))

    status = "PASS"
    shot = case.get("shot") or shot_name(inp)
    if shot:
        shot_path = os.path.join(out, shot + ".bmp")
        ref_dir = os.path.join(repo, "smoke", "ref")
        ref_path = os.path.join(ref_dir, name + ".bmp")
        if not os.path.exists(shot_path):
            problems.append(f"no '{shot}' frame (the input script's shot never happened)")
        elif blank(shot_path):
            problems.append(f"the '{shot}' frame is blank (one colour): still loading, or nothing drawn")
        elif update or not os.path.exists(ref_path):
            os.makedirs(ref_dir, exist_ok=True)
            shutil.copy(shot_path, ref_path)
            if not problems:
                status = "NEW"
            notes.append("reference saved: " + os.path.relpath(ref_path, repo))
        else:
            share, why = compare(shot_path, ref_path, case.get("region"))
            limit = case.get("threshold", 0.10)
            notes.append(f"screen differs {share:.1%} (limit {limit:.0%})")
            if share > limit:
                problems.append(why or f"screen differs from the reference ({share:.1%} > {limit:.0%})")
    if problems:
        status = "FAIL"
    return status, took, problems, notes, out


def build_repo(repo, spec):
    """Runs the repo's "build" command once; None if it built, else why not."""
    if not spec.get("build"):
        return None
    out = os.path.join(repo, "build", "smoke")
    os.makedirs(out, exist_ok=True)
    proc = subprocess.run(spec["build"], cwd=repo, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    with open(os.path.join(out, "build.txt"), "w") as f:
        f.write(proc.stdout)
    if proc.returncode == 0:
        return None
    errors = [l for l in proc.stdout.splitlines() if "error" in l][:5]
    return "build failed: " + ("; ".join(errors) or f"exit {proc.returncode}") + \
        " (see " + os.path.join(out, "build.txt") + ")"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("repos", nargs="+")
    ap.add_argument("--update", action="store_true", help="replace references with this run's shots")
    ap.add_argument("--headless", action="store_true",
                    help="never show the games' windows, so runs never take the focus")
    ap.add_argument("--only", help="comma-separated case names")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="games run at once (a repo's cases at once only if its cases.json has \"parallel\")")
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    jobs = max(1, args.jobs)
    started = time.time()

    repos = []  # (repo, spec, its cases)
    for r in args.repos:
        repo = os.path.abspath(r)
        with open(os.path.join(repo, "smoke", "cases.json")) as f:
            spec = json.load(f)
        cases = [c for c in spec["cases"] if not only or c["name"] in only]
        if cases:
            repos.append((repo, spec, cases))

    # Builds first, all repos at once (most are up to date: a second each).
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(repos))) as pool:
        broken = dict(zip([r for r, _, _ in repos],
                          pool.map(lambda x: build_repo(x[0], x[1]), repos)))

    # Then the cases: up to `jobs` games at once. A repo without "parallel"
    # runs one case at a time (its cases share build/run.log). Cases start
    # in the order given, repo by repo, skipping those of a busy repo.
    pending = [(repo, spec, case) for repo, spec, cases in repos for case in cases]
    # Timeouts scale with how many games can run at once.
    scale = min(jobs, len(pending)) if pending else 1
    results = {}
    busy = set()
    lock = threading.Condition()

    def take():
        with lock:
            while True:
                if not pending:
                    return None
                for i, (repo, spec, case) in enumerate(pending):
                    if spec.get("parallel") or repo not in busy:
                        busy.add(repo)
                        return pending.pop(i)
                lock.wait()

    def worker():
        while True:
            item = take()
            if item is None:
                return
            repo, spec, case = item
            label = f"{os.path.basename(repo)}/{case['name']}"
            print(f"... {label} (up to {case['seconds']} s)", flush=True)
            if broken.get(repo):
                status, took, problems, notes = "FAIL", 0.0, [broken[repo]], []
                out = os.path.join(repo, "build", "smoke")
            else:
                status, took, problems, notes, out = run_case(repo, spec, case, args.update, scale, args.headless)
            print(f"{status:4}  {label}  {took:.0f} s  " + "; ".join(problems + notes), flush=True)
            with lock:
                results[id(case)] = (label, status, took, problems, notes, out)
                if not spec.get("parallel"):
                    busy.discard(repo)
                lock.notify_all()

    order = [id(c) for _, _, cases in repos for c in cases]
    threads = [threading.Thread(target=worker) for _ in range(min(jobs, max(1, len(pending))))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    results = [results[k] for k in order]
    failed = sum(1 for r in results if r[1] == "FAIL")
    print()
    for label, status, took, problems, notes, out in results:
        print(f"{status:4}  {label}  {took:.0f} s")
        for p in problems:
            print(f"        {p}")
        if status == "FAIL":
            print(f"        see {out}")
    print(f"\n{len(results) - failed}/{len(results)} passed in {time.time() - started:.0f} s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
