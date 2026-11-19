#!/usr/bin/env python3
"""Picks pixels for a TEST_INPUT "wait": where the screen to wait for is
still and every screen before it differs.

    python3 -I tools/waitpick.py --after A.bmp [A2.bmp ...] --before B.bmp [B2.bmp ...]
                                 [--count 3] [--region x0,y0,x1,y1]

--after: frames of the screen to wait for (two or more from different
moments, so animated parts are left out). --before: frames of what comes
earlier (loading screens, the previous menu, a fade). Prints the best few
points, spread over the screen, as "x,y=rrggbb" to paste after
"<t> wait <timeout>". Frames come from a run with <ID>_SHOT_EVERY (tools/
smoke.py keeps them with --keep-frames)."""
import argparse
import struct


def load(path):
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    step = struct.unpack_from("<H", data, 28)[0] // 8
    pitch = (w * step + 3) & ~3
    top_down, h = h < 0, abs(h)

    def px(x, y):
        row = y if top_down else h - 1 - y
        o = off + row * pitch + x * step
        return data[o + 2], data[o + 1], data[o]
    return w, h, px


def dist(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--after", nargs="+", required=True)
    ap.add_argument("--before", nargs="+", required=True)
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--region", help="x0,y0,x1,y1 in pixels: only look there")
    ap.add_argument("--step", type=int, default=4)
    ap.add_argument("--exact", action="store_true",
                    help="no neighbour check: thin text and lines qualify (the game must draw them "
                         "at the same pixels every time, as menus do)")
    a = ap.parse_args()
    after = [load(p) for p in a.after]
    before = [load(p) for p in a.before]
    w, h = after[0][0], after[0][1]
    if any((f[0], f[1]) != (w, h) for f in after):
        raise SystemExit("the after frames differ in size")
    # A before frame of another size (another display mode) differs at any
    # point outside it; inside, its pixel is compared as usual.
    x0, y0, x1, y1 = (2, 2, w - 2, h - 2)
    if a.region:
        x0, y0, x1, y1 = (int(v) for v in a.region.split(","))
    still = []  # (x, y, colour, per-before-frame distance)
    for y in range(max(y0, 2), min(y1, h - 2), a.step):
        for x in range(max(x0, 2), min(x1, w - 2), a.step):
            ref = after[0][2](x, y)
            # Dark pixels would also match fades and loading screens.
            if max(ref) < 64:
                continue
            # Still: the same in every "after" frame, and so are its neighbours
            # (a pixel off by one still matches).
            ok = True
            for f in after:
                for dx, dy in ((0, 0),) if a.exact else ((0, 0), (-2, 0), (2, 0), (0, -2), (0, 2)):
                    if dist(f[2](x + dx, y + dy), ref) > 10:
                        ok = False
                        break
                if not ok:
                    break
            if ok:
                d = [dist(f[2](x, y), ref) if x < f[0] and y < f[1] else 255 for f in before]
                still.append((x, y, ref, d))
    if not still:
        raise SystemExit("nothing on the after frames is still (and not dark)")
    cands = sorted(((min(d), x, y, ref) for x, y, ref, d in still if min(d) >= 80), reverse=True)
    picked = []
    if cands:
        min_gap = max(w, h) // 8
        for gap in (min_gap, min_gap // 2, 0):
            for m, x, y, ref in cands:
                if len(picked) >= a.count:
                    break
                if all(abs(x - px) + abs(y - py) >= gap for _, px, py, _ in picked) and \
                        (x, y) not in [(p[1], p[2]) for p in picked]:
                    picked.append((m, x, y, ref))
        print(f"# {len(cands)} candidates; picked margins " + ", ".join(str(p[0]) for p in picked))
    else:
        # No one pixel tells every before frame apart: a set of them that
        # does, each before frame differing clearly at one at least (a wait
        # needs all of its points to match).
        left = set(range(len(before)))
        while left:
            best = max(still, key=lambda c: (sum(1 for i in left if c[3][i] >= 80), min(c[3])))
            covers = {i for i in left if best[3][i] >= 80}
            if not covers:
                raise SystemExit("no set of pixels tells these before frames apart: "
                                 + ", ".join(a.before[i] for i in sorted(left)))
            picked.append((min(best[3]), best[0], best[1], best[2]))
            left -= covers
        print(f"# no single pixel will do; {len(picked)} that together tell every before frame apart")
    print(" ".join(f"{x},{y}={r:02x}{g:02x}{b:02x}" for _, x, y, (r, g, b) in picked))


if __name__ == "__main__":
    main()
