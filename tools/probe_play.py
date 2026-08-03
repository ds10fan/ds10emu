#!/usr/bin/env python3
"""probe_play - find out what the sequencer bar's two unidentified buttons do.

    python3 tools/probe_play.py [outdir]

tools/probe_buttons.py taps a point, waits, and diffs one frame against a
baseline. That finds a button that changes what is drawn and misses one that
changes what is *happening*: if the transport starts, a single later frame is
different from the baseline only by however far the play cursor happened to
move, and if it moved a whole bar it is not different at all.

So this takes a run of frames after each tap and asks whether the screen is
*moving* - consecutive frames differing - which is what a running sequencer
looks like and a still one does not. The candidates are the two controls
between CLR and the map button, at x 166..195 ("S") and x 198..220 (a play
triangle), plus the same pair on 71_KAOSS.
"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
SAVE = "songs/DS10PLUS.ds10save"

BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""

SEQ = (30, 33)          # the map's SYN1 SEQ box
NOTES = [(58, 315), (97, 315), (136, 315), (175, 315)]

# What to tap, and where. The play triangle and the S beside it.
CANDIDATES = [("play", 209, 9), ("S", 180, 9)]

WATCH = 5               # frames to compare after each tap
GAP = 30                # between them


def tap(out, t, x, y):
    out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 20))
    return t + 40


def script():
    out = [BOOT]
    t = 2800
    out.append("%d key L\n%d up L\n" % (t, t + 20))
    t += 140
    t = tap(out, t, *SEQ)
    t += 60
    # A few notes, so a moving cursor has something to move over.
    for x, y in NOTES:
        t = tap(out, t, x // 2, y // 2)
    t += 60
    marks = []
    for name, x, y in CANDIDATES:
        t = tap(out, t, x, y)
        t += 60
        marks.append((name, "after", t))
        for _ in range(WATCH):
            out.append("%d snap\n" % t)
            t += GAP
        t = tap(out, t, x, y)      # and off again
        t += 60
        marks.append((name, "off", t))
        for _ in range(WATCH):
            out.append("%d snap\n" % t)
            t += GAP
    return "".join(out), t + 60, marks


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out/probeplay"
    text, frames, marks = script()
    os.makedirs(outdir, exist_ok=True)
    for f in glob.glob(os.path.join(outdir, "*.snap")):
        os.remove(f)
    path = os.path.join(outdir, "probe.txt")
    with open(path, "w") as f:
        f.write(text)
    cmd = ["./out/ndsref", ROM, "-f", str(frames), "-o", outdir, "-s", path,
           "-e", "1000000", "-b", "1000000"]
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    subprocess.run(cmd, check=True)

    snaps = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    if len(snaps) < len(marks) * WATCH:
        print("only %d snapshots" % len(snaps))
        return
    i = 0
    for name, when, _ in marks:
        group = [snap.load(s) for s in snaps[i:i + WATCH]]
        i += WATCH
        moved = 0
        for a, b in zip(group, group[1:]):
            moved += sum(1 for k in range(256 * 192 * 4) if a.bot[k] != b.bot[k])
        print("  %-5s %-5s: %6d bytes changed across %d frames"
              % (name, when, moved, WATCH))


if __name__ == "__main__":
    main()
