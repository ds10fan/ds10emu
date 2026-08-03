#!/usr/bin/env python3
"""uibyte - find out what a byte of a song does by changing it and looking.

    python3 tools/uibyte.py 0x25C+1 3 5            # that byte, as 3 and as 5
    python3 tools/uibyte.py --pages seq,kaoss 0x25C+6 1 2

Writes two saves that differ in one byte of the loaded song, boots each under
melonDS to the same pages, and reports which page's screen changed and by how
much. A byte nothing draws is a byte no page reacts to; a byte that moves one
page's pixels is that page's state.

The offset is `pattern+N` for a byte inside pattern 0 (which is where the
per-track editor records live, at +0x25C + track*12), or a plain payload offset.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sav                                                     # noqa: E402
import snap                                                    # noqa: E402
import snapui                                                  # noqa: E402
import songtext as st                                          # noqa: E402

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
BASE_SAVE = "songs/DS10PLUS.ds10save"
SONG = "data/saves/ds10plus_00.ds10song"
OUT = "out/uibyte"


def variant(offset, value, tag):
    """A song with one byte changed, packed into slot 0 of a save."""
    p = bytearray(st.read_song(SONG))
    p[offset] = value & 0xFF
    song = os.path.join(OUT, "%s.ds10song" % tag)
    save = os.path.join(OUT, "%s.ds10save" % tag)
    st.write_song(song, bytes(p))
    if sav.cmd_pack([BASE_SAVE, "0", song, save]) != 0:
        raise SystemExit("could not pack %s" % song)
    return save


TAPS = {"set": (85, 8), "s": (140, 8), "pad": (128, 110)}


def shot(save, page, tag, after=None):
    """Boot to `page` with that save and return its two framebuffers."""
    outdir = os.path.join(OUT, "%s.%s%s" % (tag, page, after or ""))
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith(".snap"):
            os.remove(os.path.join(outdir, f))
    x, y = snapui.PAGES[page]
    t = 2800
    script = [snapui.BOOT, "%d key L\n%d up L\n" % (t, t + 20)]
    t += 140
    script.append("%d touch %d %d\n%d release\n" % (t, x, y, t + 20))
    t += 180
    if after:
        x2, y2 = TAPS[after]
        script.append("%d touch %d %d\n%d release\n" % (t, x2, y2, t + 20))
        t += 160
    script.append("%d snap\n" % t)
    path = os.path.join(outdir, "script.txt")
    with open(path, "w") as f:
        f.write("".join(script))
    subprocess.run(["./out/ndsref", ROM, "-f", str(t + 60), "-o", outdir,
                    "-s", path, "-e", "1000000", "-b", "1000000", "-w", save],
                   check=True, stdout=subprocess.DEVNULL)
    snaps = sorted(f for f in os.listdir(outdir) if f.endswith(".snap"))
    if not snaps:
        raise SystemExit("no snapshot for %s/%s" % (tag, page))
    return snap.load(os.path.join(outdir, snaps[-1])).screens()


def differ(a, b):
    return sum(1 for x, y in zip(a, b) if x != y)


def main():
    args = sys.argv[1:]
    pages = ["seq", "synedit", "kaoss", "patch", "pattern"]
    after = None
    if args and args[0] == "--pages":
        pages = args[1].split(",")
        args = args[2:]
    if args and args[0] == "--then":
        after = args[1]
        args = args[2:]
    if len(args) < 3:
        print(__doc__)
        return 1
    where, lo, hi = args[0], int(args[1], 0), int(args[2], 0)
    if where.startswith("pattern+"):
        offset = st.PAT_OFF + int(where.split("+")[1], 0)
    else:
        offset = int(where, 0)
    os.makedirs(OUT, exist_ok=True)
    a = variant(offset, lo, "a")
    b = variant(offset, hi, "b")
    print("payload %#x: %d vs %d" % (offset, lo, hi))
    for page in pages:
        ta, ba = shot(a, page, "a", after)
        tb, bb = shot(b, page, "b", after)
        print("  %-8s top %6d pixels differ, bottom %6d"
              % (page, differ(ta, tb) // 4, differ(ba, bb) // 4))
    return 0


if __name__ == "__main__":
    sys.exit(main())
