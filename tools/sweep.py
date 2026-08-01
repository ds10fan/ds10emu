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

# The map's boxes, from the table its constructor walks (0x02087018 and
# 0x02086f08) - the same one src/app/ui.c carries, as x, y, w, h.
#
# These replace coordinates that had been measured off a rendered screen.
# Several of those missed their box by a pixel or two - the FX box starts at
# y 55 and was being tapped at 54 - and three were nowhere near one, which is
# why the corpus had no FX, drum or pattern pages in it at all.
MAP_BOX = [
    ("syn1_seq",    7,  25, 46, 16),
    ("syn1_kbd",   39,  45, 54, 16),
    ("syn1_kaoss", 39,  65, 54, 16),
    ("syn1_edit", 117,  25, 46, 16),
    ("syn1_patch", 131, 54, 53, 16),
    ("syn2_seq",    7,  87, 46, 16),
    ("syn2_kbd",   39, 107, 54, 16),
    ("syn2_kaoss", 39, 127, 54, 16),
    ("syn2_edit", 117,  87, 46, 16),
    ("syn2_patch", 131, 116, 53, 16),
    ("mixer",     203,  25, 36, 16),
    ("fx",        207,  55, 30, 16),
    ("pattern",     4,   2, 76, 14),
    ("drums_seq",   7, 149, 54, 16),
]

# The sequencer's sub-mode button, which steps
#
#     PIANO -> GATE -> VOL -> PAN -> KAOSS -> KAOSS -> PIANO
#
# and is the only way into those five variants: the map opens the sequencer
# and nothing else. Found with tools/probe_buttons.py, which taps a page and
# watches, because these buttons are not in any table.
SUBMODE = (46, 8)
SUBMODE_STEPS = 5

# The DRUMS button opens a submenu over the map, so these need two taps.
DRUMS = (206, 142, 46, 16)
SUBMENU = [
    ("drum_edit",  85,  64, 88, 16),
    ("drum_patch", 85,  96, 88, 16),
    ("drum_fx",    85, 128, 88, 16),
]


PAGES = [(n, x + w // 2, y + h // 2) for n, x, y, w, h in MAP_BOX]

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
    # The drum pages need the DRUMS button first: it opens a submenu over the
    # map, and the page is on the second tap.
    dx, dy = DRUMS[0] + DRUMS[2] // 2, DRUMS[1] + DRUMS[3] // 2
    for name, x, y, w, h in SUBMENU:
        out.append("%d key L\n%d up L\n" % (t, t + 20))
        out.append("%d touch %d %d\n" % (t + 90, dx, dy))
        out.append("%d release\n" % (t + 130))
        out.append("%d snap\n" % (t + 200))           # the submenu itself
        out.append("%d touch %d %d\n" % (t + 220, x + w // 2, y + h // 2))
        out.append("%d release\n" % (t + 260))
        out.append("%d snap\n" % (t + 380))
        t += STEP + 120
    # The sequencer's other five sub-modes, each one tap further on.
    out.append("%d key L\n%d up L\n" % (t, t + 20))
    out.append("%d touch 30 33\n%d release\n" % (t + 120, t + 160))
    t += 340
    for _ in range(SUBMODE_STEPS):
        out.append("%d touch %d %d\n%d release\n"
                   % (t, SUBMODE[0], SUBMODE[1], t + 30))
        out.append("%d snap\n" % (t + 200))
        t += 260
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
    if "-m" in sys.argv:
        cmd.append("-m")
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)


main()
