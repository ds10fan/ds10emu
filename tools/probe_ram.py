#!/usr/bin/env python3
"""probe_ram - what a button changes in memory, when it changes nothing on
screen.

    python3 tools/probe_ram.py 180 9 [outdir]

tools/probe_buttons.py finds a control by the pixels it changes and
tools/probe_play.py by the pixels it sets moving. A control that does neither
- the "S" on the sequencer's top bar is one - is invisible to both, but it
still has to write something down. This taps it and diffs main RAM.

Two snapshots are taken before the tap and one after. Anything that differs
between the first two is the emulator's own churn - counters, audio buffers,
the frame's scratch - and is masked out, so what is left is what the tap did.
Runs of changed bytes are printed with their old and new contents.
"""
import glob
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
SAVE = "songs/DS10PLUS.ds10save"
RAM_BASE = 0x02000000

BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""

SEQ = (30, 33)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    x, y = int(sys.argv[1]), int(sys.argv[2])
    taps = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    outdir = sys.argv[4] if len(sys.argv) > 4 else "out/proberam"

    out = [BOOT]
    t = 2800
    out.append("%d key L\n%d up L\n" % (t, t + 20))
    t += 140
    out.append("%d touch %d %d\n%d release\n" % (t, SEQ[0], SEQ[1], t + 20))
    t += 120
    out.append("%d snap\n" % t)          # A
    t += 60
    out.append("%d snap\n" % t)          # B - the churn baseline
    t += 60
    for _ in range(taps):
        out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 20))
        t += 120
        out.append("%d snap\n" % t)      # C, and one per tap after it
        t += 30

    os.makedirs(outdir, exist_ok=True)
    for f in glob.glob(os.path.join(outdir, "*.snap")):
        os.remove(f)
    path = os.path.join(outdir, "probe.txt")
    with open(path, "w") as f:
        f.write("".join(out))
    cmd = ["./out/ndsref", ROM, "-f", str(t + 60), "-o", outdir, "-s", path,
           "-e", "1000000", "-b", "1000000", "-m"]
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    subprocess.run(cmd, check=True)

    snaps = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    if len(snaps) < 3:
        print("only %d snapshots" % len(snaps))
        return 1
    mem = [snap.load(s).ram for s in snaps]
    if not mem[0]:
        print("no main RAM in the snapshots - was -m passed?")
        return 1
    a, b = mem[0], mem[1]
    churn = set()
    for i in range(len(a)):
        if a[i] != b[i]:
            churn.add(i)

    for n, c in enumerate(mem[2:], 1):
        runs, cur = [], None
        for i in range(len(b)):
            changed = b[i] != c[i] and i not in churn
            if changed and cur is None:
                cur = i
            elif not changed and cur is not None:
                if i - cur < 64:
                    runs.append((cur, i))
                cur = None
        print("tap %d on (%d,%d): %d runs changed" % (n, x, y, len(runs)))
        for lo, hi in runs[:80]:
            print("  %08x %-24s -> %s" % (RAM_BASE + lo, b[lo:hi].hex(),
                                          c[lo:hi].hex()))
        if len(runs) > 80:
            print("  ... %d more" % (len(runs) - 80))
        b = c
    return 0


if __name__ == "__main__":
    sys.exit(main())
