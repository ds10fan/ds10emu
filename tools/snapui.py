#!/usr/bin/env python3
"""snapui - capture a page with the machine's main RAM.

    python3 tools/snapui.py drums [outdir]

Boots, opens one page through the navigation map, and snapshots with
`ndsref -m`. tools/drumbuf.py reads the drum buffers out of such a snapshot.

The result is what tools/emu.py's `seed` writes into the Unicorn image, so a
screen's own function can be called against the machine as melonDS had it.
"""
import glob
import os
import subprocess
import sys

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
SAVE = "songs/DS10PLUS.ds10save"

BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""

# The centre of the map box that opens each page.
PAGES = {
    "seq":     (30, 33),      # SYN1 SEQ
    "synedit": (140, 33),     # SYN1 EDIT
    "patch":   (157, 62),     # SYN1 PATCH
    "kaoss":   (66, 73),      # SYN1 KAOSS
    "kbd":     (66, 53),      # SYN1 KBD
    "mixer":   (221, 33),
    "fx":      (222, 63),
    "pattern": (42, 9),
    "drums":   (34, 157),
    "drumsel": (229, 150),
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in PAGES:
        print(__doc__)
        print("pages: %s" % " ".join(sorted(PAGES)))
        return 1
    page = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "out/snapui/" + page
    x, y = PAGES[page]

    t = 2800
    out = [BOOT, "%d key L\n%d up L\n" % (t, t + 20)]
    t += 140
    out.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 20))
    t += 180
    out.append("%d snap\n" % t)

    os.makedirs(outdir, exist_ok=True)
    for f in glob.glob(os.path.join(outdir, "*.snap")):
        os.remove(f)
    path = os.path.join(outdir, "script.txt")
    with open(path, "w") as f:
        f.write("".join(out))
    cmd = ["./out/ndsref", ROM, "-f", str(t + 60), "-o", outdir, "-s", path,
           "-e", "1000000", "-b", "1000000", "-m"]
    if os.path.exists(SAVE):
        cmd += ["-w", SAVE]
    subprocess.run(cmd, check=True)
    got = sorted(glob.glob(os.path.join(outdir, "*.snap")))
    print("%s: %s" % (page, got[-1] if got else "no snapshot"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
