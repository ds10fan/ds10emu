#!/usr/bin/env python3
"""sweep - drive ndsref through the whole UI and snapshot every screen.

Boots, loads a session, then walks the navigation map tapping each page in
turn. What comes out is the corpus `make test-2d` runs against: the more of
the interface it covers, the more of the renderer is actually tested.

    python3 tools/sweep.py [outdir]

The map lives on the top screen, which is not touchable, so L is pressed
first - it swaps the two screens, which is how the map becomes reachable.
"""
import os
import subprocess
import sys

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
SAVE = "songs/DS10PLUS.SAV"

# Tap targets on the navigation map, once it is on the touch screen.
PAGES = [
    ("syn1_seq", 40, 38), ("syn1_kbd", 48, 54), ("syn1_kaoss", 48, 70),
    ("syn1_edit", 130, 38), ("syn1_patch", 133, 54),
    ("mixer", 228, 38), ("fx", 230, 54),
    ("syn2_seq", 40, 98), ("syn2_kbd", 48, 114), ("syn2_kaoss", 48, 130),
    ("syn2_edit", 130, 98), ("syn2_patch", 133, 114),
    ("menu", 228, 98), ("song", 228, 114), ("drums", 228, 130),
    ("drums_seq", 40, 157),
]

# Boot, SINGLE PLAYER, session S-01, confirm. Frame numbers are settling time,
# found by watching the framebuffer stop changing.
BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""

START = 2800
STEP = 320


def script():
    out = [BOOT]
    t = START
    for name, x, y in PAGES:
        out.append("%d key L\n%d up L\n" % (t, t + 20))
        out.append("%d snap\n" % (t + 90))            # the map itself
        out.append("%d touch %d %d\n" % (t + 120, x, y))
        out.append("%d release\n" % (t + 160))
        out.append("%d snap\n" % (t + 280))           # the page it opened
        t += STEP
    return "".join(out), t + 40


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out/sweep"
    text, frames = script()
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "sweep.txt")
    with open(path, "w") as f:
        f.write(text)
    cmd = ["./out/ndsref", ROM, "-f", str(frames), "-o", outdir,
           "-s", path, "-e", "200", "-b", "100"]
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


main()
