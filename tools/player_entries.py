#!/usr/bin/env python3
"""The functions a developer's lift has that the player's build would miss.

    python3 -I tools/player_entries.py REPO --lifter PATH [--game DIR]
                                       [--reference DIR] [--write]

The player's build (player/build.sh) lifts without Ghidra: symbols come from
the PE files alone (pe_symbols.py --seeds), plus the manifests the game's
recomplauncher.json names. Functions reached only through pointers the
lifter can't see (vtables built at run time, callbacks stored by code) are
then unlifted, and the installed game stops at "indirect branch to unlifted
address" where the developer's build ran.

This lifts REPO's game as build.sh does, into REPO/build/player-lift
(the game copy: --game, default REPO/game), and compares the dispatch table
(table.cpp) with the developer's lift (--reference, default
REPO/build/generated). It prints how many functions each image lacks; with
--write it adds them to the image's manifest as `entry` lines, at the
image's preferred base as manifests hold them. An image without a manifest
in recomplauncher.json gets manifests/<name>.manifest, which the JSON must
then name (printed). Run it again after changing the images or the game
files, or after a lifter change that finds fewer functions.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def pe_layout(path):
    """(preferred base, image size) from a PE's optional header."""
    with open(path, "rb") as f:
        data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        sys.exit("player_entries: %s is not a PE file" % path)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    base = struct.unpack_from("<Q", data, opt + 24)[0] if magic == 0x20B else \
        struct.unpack_from("<I", data, opt + 28)[0]
    size = struct.unpack_from("<I", data, opt + 56)[0]
    return base, size


def functions(generated):
    """Every address a lift's dispatch table has a function for."""
    with open(os.path.join(generated, "table.cpp"), errors="replace") as f:
        text = f.read()
    return {int(m.group(1), 16) for m in re.finditer(r"\{0x([0-9A-Fa-f]+)u, &gen::f_", text)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("repo")
    ap.add_argument("--lifter", required=True, help="recomp_lift")
    ap.add_argument("--game", help="the game copy (default: REPO/game)")
    ap.add_argument("--reference", help="the developer's lift (default: REPO/build/generated)")
    ap.add_argument("--write", action="store_true", help="add the missing functions to the manifests")
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    game = os.path.abspath(a.game or os.path.join(repo, "game"))
    reference = os.path.abspath(a.reference or os.path.join(repo, "build", "generated"))
    work = os.path.join(repo, "build", "player-lift")
    with open(os.path.join(repo, "recomplauncher.json")) as f:
        x = json.load(f)["x-swarc"]

    os.makedirs(os.path.join(work, "redist"), exist_ok=True)
    empty = os.path.join(work, "empty.manifest")
    open(empty, "w").close()
    images, args = [], []
    for i, img in enumerate(x["lift"]["images"]):
        src = os.path.join(game, img["file"])
        if img.get("redist"):  # as build.sh: extracted from the game's redistributable
            src = os.path.join(work, "redist", os.path.basename(img["file"]))
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "vcredist.py"),
                            os.path.join(game, img["redist"]), os.path.dirname(src),
                            os.path.basename(img["file"])], check=True, stdout=subprocess.DEVNULL)
        sym = os.path.join(work, "symbols-" + img["file"].replace("/", "_"))
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "pe_symbols.py"), src,
                        "--out", sym, "--seeds", "--quiet"], check=True)
        manifest = os.path.join(repo, img["manifest"]) if "manifest" in img else empty
        if not os.path.exists(manifest):  # one --write will make
            manifest = empty
        args += ["--exe" if i == 0 else "--module", src, "--symbols", sym, "--manifest", manifest]
        if img.get("base"):
            args += ["--base", img["base"]]
        pref, size = pe_layout(src)
        base = int(img["base"], 16) if img.get("base") else pref
        images.append((img, base, pref, size))

    out = os.path.join(work, "generated")
    log = os.path.join(work, "lift.log")
    with open(log, "w") as f:
        r = subprocess.run([a.lifter] + x["lift"].get("args", []) + args + ["--out", out],
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit("player_entries: the lift failed (%s)" % log)

    missing = {}
    for va in sorted(functions(reference) - functions(out)):
        for img, base, pref, size in images:
            if base <= va < base + size:
                missing.setdefault(img["file"], []).append(va - base + pref)
                break
    for img, _, _, _ in images:
        print("%s: %d functions only in the reference lift" % (img["file"], len(missing.get(img["file"], []))))
    if not a.write:
        return
    for img, _, _, _ in images:
        vas = missing.get(img["file"])
        if not vas:
            continue
        rel = img.get("manifest") or os.path.join(
            "manifests", os.path.splitext(os.path.basename(img["file"]))[0].lower() + ".manifest")
        path = os.path.join(repo, rel)
        old = open(path).read() if os.path.exists(path) else \
            "# %s: fixes to the analysis.\n" % os.path.basename(img["file"])
        have = {int(v, 16) for v in re.findall(r"^entry\s+([0-9A-Fa-f]+)", old, re.M)}
        new = [va for va in vas if va not in have]
        if not new:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(old.rstrip("\n") + "\n\n# Functions the player's build (no Ghidra) doesn't find by itself\n"
                    "# (tools/player_entries.py): reached through pointers it can't see.\n")
            for va in new:
                f.write("entry %X\n" % va)
        print("%s: %d entries" % (rel, len(new)))
        if "manifest" not in img:
            print('    add "manifest": "%s" to %s in recomplauncher.json' % (rel, img["file"]))


if __name__ == "__main__":
    main()
