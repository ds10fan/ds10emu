#!/usr/bin/env python3
"""pagecheck - render each page with the reimplementation and compare it
against what melonDS produced for the same page.

    python3 tools/pagecheck.py [snapshotdir]

For every distinct pair of pages in the capture, this runs out/ds10view and
diffs both screens against the captured framebuffers. A page whose sprite
layer is known should come out at zero; a page without one differs wherever a
sprite would have been, and a page that shows live values differs wherever
those values are.
"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro
import snap

W, H = 256, 192


def page_index(assetdir):
    out = []
    for f in sorted(glob.glob(os.path.join(assetdir, "*.NCGR"))):
        try:
            g = nitro.Ncgr(open(f, "rb").read())
        except Exception:
            continue
        if len(g.data) >= 2048:
            out.append((os.path.basename(f)[:-5], g.data))
    return out


def identify(s, index):
    """(bottom, top) page names, by finding each page's tiles in VRAM."""
    found = [None, None]
    for slot, (reg, off) in enumerate((("ABG", 0x4000), ("BBG", 0x4000))):
        vram = s.flat[reg]
        best = None
        for name, data in index:
            n = min(len(data), len(vram) - off)
            if n > 0 and vram[off:off + n] == data[:n]:
                if best is None or len(data) > best[1]:
                    best = (name, len(data))
        if best:
            found[slot] = best[0]
    return found


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    return parts[3]


def diff(mine, ref):
    """Pixels differing, comparing RGB against the capture's BGRA."""
    n = 0
    for i in range(W * H):
        if (mine[i * 3] != ref[i * 4 + 2] or mine[i * 3 + 1] != ref[i * 4 + 1]
                or mine[i * 3 + 2] != ref[i * 4]):
            n += 1
    return n


# The pattern the captured frame was showing. tools/pages.py takes the step
# markers out of the display list because they are state, not chrome, so
# reproducing the frame means drawing them from here - which is the check that
# the grid's origin, its cell size and its row-to-note mapping are all right.
#
# Six markers at steps 0,2,3,4,6,7 on rows 9,9,5,2,0,0, and the top row is A4.
SEQ_STATE = {
    "60_SYN_SEQ_PIANO": "0/0:60,2:60,3:64,4:67,6:69,7:69",
}


def main():
    srcdir = sys.argv[1] if len(sys.argv) > 1 else "out/sweep"
    index = page_index("data/nitrofs/mainbg")
    seen, rows, bad = set(), [], 0
    for p in sorted(glob.glob(os.path.join(srcdir, "*.snap"))):
        s = snap.load(p)
        bot, top = identify(s, index)
        if not bot or not top or (bot, top) in seen:
            continue
        seen.add((bot, top))
        cmd = ["./out/ds10view", bot, top, "out/pagecheck"]
        if bot in SEQ_STATE:
            cmd.append(SEQ_STATE[bot])
        r = subprocess.run(cmd, capture_output=True)
        if r.returncode:
            rows.append(("%s / %s" % (bot, top), "-",
                         r.stderr.decode().strip().split("\n")[-1]))
            bad += 1
            continue
        for which, name, ref in (("bot", bot, s.bot), ("top", top, s.top)):
            mine = read_ppm("out/pagecheck_%s.ppm" % which)
            d = diff(mine, ref)
            rows.append((name, which, "%d" % d))
            if d:
                bad += 1
    for name, which, note in rows:
        print("  %-24s %-4s %s" % (name, which, note))
    print("%s: %d screens, %d not pixel-exact" %
          ("PASS" if not bad else "PARTIAL", len(rows), bad))
    return 0


sys.exit(main())
