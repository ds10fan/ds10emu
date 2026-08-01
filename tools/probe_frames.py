#!/usr/bin/env python3
"""probe_frames - find a page's button rectangles by their drawn borders.

    make ds10view && python3 tools/probe_frames.py 52_FX [outdir]

The interface draws every button as a framed box, and the frame is in the
page's own tile image - so the rectangle a finger has to land in is visible in
the rendered page, exactly, with no measuring by eye. This renders the page
with out/ds10view and reports every closed rectangle it finds.

That is not a substitute for reading the constructor's table where there is
one; it is for the pages whose controls are placed in code, where the choice
would otherwise be between tapping the real thing a few hundred times and
squinting at a screenshot.
"""
import os
import subprocess
import sys

W, H = 256, 192
MIN_W, MIN_H = 8, 6


def render(page, outdir):
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.join(outdir, page)
    subprocess.run(["./out/ds10view", page, "90_MAP", stem], check=True,
                   stdout=subprocess.DEVNULL)
    return stem + "_bot.ppm"


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    px = parts[3]
    return [[(px[(y * W + x) * 3] + px[(y * W + x) * 3 + 1]
              + px[(y * W + x) * 3 + 2]) // 3 for x in range(W)]
            for y in range(H)]


def frames(g, thr):
    """Closed rectangles: a bright top and bottom edge with bright sides."""
    rows = []
    for y in range(H):
        x = 0
        while x < W:
            if g[y][x] > thr:
                x0 = x
                while x < W and g[y][x] > thr:
                    x += 1
                if x - x0 >= MIN_W:
                    rows.append((y, x0, x - 1))
            else:
                x += 1
    out = []
    for i, (y0, a0, a1) in enumerate(rows):
        for y1, b0, b1 in rows[i + 1:]:
            if y1 - y0 < MIN_H:
                continue
            if abs(a0 - b0) > 1 or abs(a1 - b1) > 1:
                continue
            sides = all(g[y][a0] > thr and g[y][a1] > thr
                        for y in range(y0, y1 + 1))
            if sides:
                out.append((a0, y0, a1 - a0 + 1, y1 - y0 + 1))
                break
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    page = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "out/frames"
    g = load(render(page, outdir))
    got = frames(g, 40)
    print("%s: %d framed rectangles" % (page, len(got)))
    for x, y, w, h in sorted(got, key=lambda r: (r[1], r[0])):
        print("  {%3d, %3d, %3d, %3d}," % (x, y, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
