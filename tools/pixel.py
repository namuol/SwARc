#!/usr/bin/env python3
"""Prints pixels of a BMP (a smoke shot or reference) as TEST_INPUT "wait" wants them.

    python3 -I tools/pixel.py FILE.bmp X,Y [X,Y ...]

Prints "X,Y=rrggbb ..." to paste into a wait line, after the frame's size.
Pick pixels on still parts of the screen the next step needs (a button, a
panel), not on animation or the cursor."""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    step = struct.unpack_from("<H", data, 28)[0] // 8
    pitch = (w * step + 3) & ~3
    top_down, h = h < 0, abs(h)
    out = []
    for arg in sys.argv[2:]:
        x, y = (int(v) for v in arg.split(","))
        row = y if top_down else h - 1 - y
        b, g, r = data[off + row * pitch + x * step: off + row * pitch + x * step + 3]
        out.append(f"{x},{y}={r:02x}{g:02x}{b:02x}")
    print(f"# {w}x{h}")
    print(" ".join(out))


if __name__ == "__main__":
    main()
