#!/usr/bin/env python3
"""probe_buttons - find a page's controls by tapping it and watching.

    python3 tools/probe_buttons.py [outdir] [page] [y0 y1 ystep]

`page` is a map box caption - "SYN1  SEQ", "PATTERN", "MIXER" - naming the
screen to open and probe. The default is the synth sequencer.

Some of the interface's buttons are not placed by the shared widget helper the
other screens use, so there is no table to read them out of: their rectangles
live inside the screen update's own code, which has no coordinate compares in
it either. Rather than measure them off a rendered screen - which is how seven
wrong coordinates got into this project - this taps a grid of points on the
real thing and reports which ones changed anything, where, and what page the
screen became.

The screen is reopened through the map before every tap, so one control that
changes something sticky does not poison the probes after it. SKIP lists the
points that would defeat that anyway: the sub-mode button steps a six-position
cycle the reopen does not undo.
"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro
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

# Which map box opens the page under test, as its centre. The rectangles are
# the map's own table, read out of the ROM.
PAGES = {
    "seq":     (30, 33),      # SYN1 SEQ
    "pattern": (42, 9),       # the untitled box at the top
    "mixer":  (221, 33),
    "fx":     (222, 63),
    "drums":  (34, 157),      # DRUMS SEQ
    "kbd":    (66, 53),       # SYN1 KBD
}
OPEN = PAGES["seq"]

# The NO button of the CLEAR ALL DATA prompt.
DIALOG_NO = (175, 117)

# The band above the grid, which is where the page's own buttons are.
# The band above the grid, which is where the sequencer's own buttons are.
# Override from the command line to sweep a whole page.
PROBE = [(x, y) for y in (8, 15) for x in range(4, 256, 3)]

# The sub-mode button, whose effect the reopen cannot undo.
SKIP = [(x, y) for y in (8, 15) for x in range(40, 60, 4)]

SETTLE = 150
GRID_Y = 33      # the bar is everything above the grid


def reopen(out, t):
    """Back to the map and into the sequencer again.

    This does not put notes on the grid first, though a control that clears
    the pattern would be easier to spot if it did: a tap on the grid toggles
    the cell, so re-entering the same notes before every probe alternates them
    on and off and makes every second baseline differ. Distinguishing a clear
    from anything else needs a run that enters notes once and never taps the
    grid again.
    """
    # NO first, in case the last tap opened the CLEAR ALL DATA prompt: it is
    # modal and swallows everything else, the map key included, and B does not
    # dismiss it. With no dialog up that tap lands on the grid and toggles a
    # cell, which is why the scoring below reports the bar and the rest of the
    # screen separately - grid noise cannot then hide a button.
    out.append("%d touch %d %d\n%d release\n" % (t, DIALOG_NO[0],
                                                 DIALOG_NO[1], t + 30))
    out.append("%d key L\n%d up L\n" % (t + 100, t + 120))
    out.append("%d touch %d %d\n%d release\n" % (t + 220, OPEN[0], OPEN[1],
                                                 t + 260))
    return t + 440


def points():
    return [p for p in PROBE if p not in SKIP]


def script():
    out = [BOOT]
    t = reopen(out, 2800)
    out.append("%d snap\n" % t)          # the baseline
    t += 60
    for x, y in points():
        t = reopen(out, t)
        out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 30))
        out.append("%d snap\n" % (t + SETTLE))
        t += SETTLE + 60
    return "".join(out), t + 60


def page_index():
    out = []
    for f in sorted(glob.glob("data/nitrofs/mainbg/*.NCGR")):
        try:
            g = nitro.Ncgr(open(f, "rb").read())
        except Exception:
            continue
        if len(g.data) >= 2048:
            out.append((os.path.basename(f)[:-5], g.data))
    return out


def identify(vram, index, off=0x4000):
    best = None
    for name, data in index:
        n = min(len(data), len(vram) - off)
        if n > 0 and vram[off:off + n] == data[:n]:
            if best is None or len(data) > best[1]:
                best = (name, len(data))
    return best[0] if best else "?"


def main():
    global OPEN, PROBE, GRID_Y
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out/probe"
    if len(sys.argv) > 2:
        OPEN = PAGES[sys.argv[2]]
    if len(sys.argv) > 5:
        y0, y1, ystep = (int(v) for v in sys.argv[3:6])
        PROBE = [(x, y) for y in range(y0, y1 + 1, ystep)
                 for x in range(4, 256, 6)]
        GRID_Y = 0
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

    index = page_index()
    snaps = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    if len(snaps) < 2:
        print("no snapshots")
        return
    base = snap.load(snaps[0])
    basepage = identify(base.flat["ABG"], index)
    print("baseline: %s" % basepage)
    for i, (x, y) in enumerate(points()):
        if 1 + i >= len(snaps):
            break
        s = snap.load(snaps[1 + i])
        bar = mid = 0
        for k in range(256 * 192):
            if s.bot[k * 4:k * 4 + 3] != base.bot[k * 4:k * 4 + 3]:
                if k // 256 < GRID_Y:
                    bar += 1
                else:
                    mid += 1
        page = identify(s.flat["ABG"], index)
        if not bar and not mid and page == basepage:
            continue
        print("  tap (%3d,%2d) -> bar %5d  rest %6d   page %s"
              % (x, y, bar, mid, page))


if __name__ == "__main__":
    main()
