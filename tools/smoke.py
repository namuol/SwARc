#!/usr/bin/env python3
"""Smoke tests for recomps built on SwARc: scripted runs with pass/fail checks.

    python3 -I tools/smoke.py [--update] [--only NAME[,NAME]] REPO...

Each REPO (a recomp's checkout) lists its cases in smoke/cases.json:

    {"run": "tools/try-run.sh", "prefix": "TIBSUN",
     "cases": [{"name": "skirmish", "input": "tools/input/skirmish-start.txt",
                "seconds": 56, "min_frames": 300, "shot": "end"}]}

`run` and `prefix` can be set per case too. A case runs the game in the
background with <PREFIX>_TEST_INPUT for `seconds` (the run script rebuilds
first), then fails if:
  - the build failed, or the game exited before `seconds` (unless "exits": true);
  - the run log shows a crash or fatal error (both save a report);
  - fewer than `min_frames` frames were presented (counted every 50);
  - the input script's "shot <shot>" frame is missing, or differs from the
    reference smoke/ref/<name>.bmp by more than `threshold` (default 0.10:
    the share of sampled pixels that differ clearly), or is blank (one colour).
    "region": [x0, y0, x1, y1] (fractions of the frame) compares only that
    part: the sidebar of a match whose start position is random, say.
A case with no reference saves its shot as the reference (NEW: look at it).
--update replaces the references with this run's shots. References are the
game's own pixels: keep smoke/ref/ out of git.

Results go to REPO/build/smoke/<name>/ (shot, run log, frames). Exit status
1 if any case failed.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
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


def run_case(repo, defaults, case, update):
    name = case["name"]
    prefix = case.get("prefix", defaults.get("prefix"))
    script = case.get("run", defaults.get("run", "tools/try-run.sh"))
    seconds = int(case["seconds"])
    inp = os.path.join(repo, case["input"])
    out = os.path.join(repo, "build", "smoke", name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    env = dict(os.environ)
    env.update({f"{prefix}_TEST_INPUT": inp, f"{prefix}_SHOT_DIR": out,
                f"{prefix}_SHOT_EVERY": "50", f"{prefix}_BACKGROUND": "1"})
    started = time.time()
    proc = subprocess.run([os.path.join(repo, script), str(seconds)], cwd=repo, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    took = time.time() - started
    with open(os.path.join(out, "try-run.txt"), "w") as f:
        f.write(proc.stdout)
    log_path = os.path.join(repo, "build", "run.log")
    log = ""
    if os.path.exists(log_path):
        shutil.copy(log_path, os.path.join(out, "run.log"))
        with open(log_path, errors="replace") as f:
            log = f.read()

    problems, notes = [], []
    if "BUILD FAILED" in proc.stdout:
        problems.append("build failed")
    elif "exited with" in proc.stdout and not case.get("exits"):
        problems.append("game exited early (" + proc.stdout.split("exited with")[1].split()[0] + ")")
    if "report saved to" in log:  # every crash and fatal error saves one
        problems.append("crash or fatal error (report in run.log)")
    frames = [int(f[6:11]) for f in os.listdir(out) if f.startswith("frame_") and f.endswith(".bmp")]
    presented = max(frames, default=0)
    if presented < case.get("min_frames", 1):
        problems.append(f"~{presented} frames presented, want {case.get('min_frames', 1)}")
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("repos", nargs="+")
    ap.add_argument("--update", action="store_true", help="replace references with this run's shots")
    ap.add_argument("--only", help="comma-separated case names")
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    failed = 0
    results = []
    for repo in args.repos:
        repo = os.path.abspath(repo)
        with open(os.path.join(repo, "smoke", "cases.json")) as f:
            spec = json.load(f)
        for case in spec["cases"]:
            if only and case["name"] not in only:
                continue
            label = f"{os.path.basename(repo)}/{case['name']}"
            print(f"... {label} ({case['seconds']} s)", flush=True)
            status, took, problems, notes, out = run_case(repo, spec, case, args.update)
            failed += status == "FAIL"
            results.append((label, status, took, problems, notes, out))
            print(f"{status:4}  {label}  {took:.0f} s  " + "; ".join(problems + notes), flush=True)
    print()
    for label, status, took, problems, notes, out in results:
        print(f"{status:4}  {label}")
        for p in problems:
            print(f"        {p}")
        if status == "FAIL":
            print(f"        see {out}")
    print(f"\n{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
