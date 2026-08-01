#!/usr/bin/env python3
"""probe_edge - find a button's exact edges by tapping one point per boot.

    python3 tools/probe_edge.py 8 36 64      # y, then the x range to test

`tools/probe_buttons.py` locates a control to within its step, but it cannot
resolve an edge: the sequencer's sub-mode button steps a cycle its reopen
cannot undo, so consecutive taps are not independent. This runs the emulator
once per candidate point, from reset, so every tap starts from the same state
and a hit is unambiguous - the page becomes the next sub-mode or it does not.

Slow by design: a boot per point. Use it to pin an edge, not to search.
"""
import glob
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_buttons as pb
import snap

BOOT_END = 2800


def run(outdir, x, y):
    os.makedirs(outdir, exist_ok=True)
    out = [pb.BOOT]
    t = pb.reopen(out, BOOT_END)
    out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 30))
    out.append("%d snap\n" % (t + pb.SETTLE))
    frames = t + pb.SETTLE + 40
    path = os.path.join(outdir, "edge.txt")
    with open(path, "w") as f:
        f.write("".join(out))
    cmd = ["./out/ndsref", pb.ROM, "-f", str(frames), "-o", outdir,
           "-s", path, "-e", "100000", "-b", "100000"]
    if os.path.exists(pb.SAVE):
        cmd += ["-w", pb.SAVE]
    subprocess.run(cmd, check=True, capture_output=True)
    snaps = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    if not snaps:
        return "?"
    return pb.identify(snap.load(snaps[-1]).flat["ABG"], pb.page_index())


def main():
    y = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 36
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    tmp = "out/edge"
    for x in range(lo, hi + 1):
        shutil.rmtree(tmp, ignore_errors=True)
        page = run(tmp, x, y)
        print("  (%3d,%2d) -> %s%s" % (x, y, page,
                                       "   HIT" if page != "60_SYN_SEQ_PIANO"
                                       else ""))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
