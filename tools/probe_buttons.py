#!/usr/bin/env python3
"""probe_buttons - find a page's controls by tapping it and watching.

    python3 tools/probe_buttons.py [outdir]

Some of the interface's buttons are not placed by the shared widget helper the
other screens use, so there is no table to read them out of: their rectangles
live inside the screen update's own code. Rather than measure them off a
rendered screen - which is how seven wrong coordinates got into this project -
this taps a grid of points on the real thing and reports which ones changed
anything, and where.

What comes out is a map of "a tap here does something, and it redrew this
region", which is enough to say where a control is and usually what it is.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
SAVE = "songs/DS10PLUS.SAV"

# Boot, SINGLE PLAYER, session S-01, confirm - the same as tools/sweep.py.
BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""

# The synth sequencer, from the map's SYN1 SEQ box.
OPEN = [(30, 33)]

# The band above the grid, which is where the page's own buttons are.
PROBE = [(x, 8) for x in range(4, 256, 4)]

SETTLE = 150


def reopen(out, t):
    """Back to the map and into the sequencer again.

    This does not put notes on the grid first, though a control that clears
    the pattern would be easier to spot if it did: a tap on the grid toggles
    the cell, so re-entering the same notes before every probe alternates them
    on and off and makes every second baseline differ. Distinguishing a clear
    from anything else needs a run that enters notes once and never taps the
    grid again.
    """
    out.append("%d key L\n%d up L\n" % (t, t + 20))
    for x, y in OPEN:
        out.append("%d touch %d %d\n%d release\n" % (t + 120, x, y, t + 160))
    return t + 340


def script():
    out = [BOOT]
    t = reopen(out, 2800)
    out.append("%d snap\n" % t)          # the baseline
    t += 60
    for x, y in PROBE:
        # Each tap starts from the same screen, so one that changes the page
        # does not poison every probe after it.
        t = reopen(out, t)
        out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 30))
        out.append("%d snap\n" % (t + SETTLE))
        # A second look, to tell a control that started something moving from
        # one that just redrew.
        out.append("%d snap\n" % (t + SETTLE + 120))
        t += SETTLE + 200
    return "".join(out), t + 60


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out/probe"
    text, frames = script()
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "probe.txt")
    with open(path, "w") as f:
        f.write(text)
    cmd = ["./out/ndsref", ROM, "-f", str(frames), "-o", outdir, "-s", path,
           "-e", "100000", "-b", "100000"]
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    subprocess.run(cmd, check=True)

    import glob
    snaps = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    if len(snaps) < 2:
        print("no snapshots")
        return
    def diff(a, b):
        x0 = y0 = 10 ** 9
        x1 = y1 = -1
        n = 0
        for k in range(256 * 192):
            if a[k * 4:k * 4 + 3] != b[k * 4:k * 4 + 3]:
                n += 1
                px, py = k % 256, k // 256
                x0, x1 = min(x0, px), max(x1, px)
                y0, y1 = min(y0, py), max(y1, py)
        return n, (x0, y0, x1, y1)

    base = snap.load(snaps[0]).bot
    print("%d taps against the baseline" % ((len(snaps) - 1) // 2))
    for i in range(len(PROBE)):
        if 1 + i * 2 + 1 >= len(snaps):
            break
        a = snap.load(snaps[1 + i * 2]).bot
        b = snap.load(snaps[1 + i * 2 + 1]).bot
        n, box = diff(base, a)
        moving, _ = diff(a, b)
        if not n and not moving:
            continue
        print("  tap (%3d,%2d) -> %6d px vs baseline in (%d,%d)-(%d,%d)%s"
              % (PROBE[i][0], PROBE[i][1], n, box[0], box[1], box[2], box[3],
                 "   still moving (%d px)" % moving if moving else ""))


if __name__ == "__main__":
    main()
