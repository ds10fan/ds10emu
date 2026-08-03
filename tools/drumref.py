#!/usr/bin/env python3
"""drumref - the hardware's own drum buffers for a song of your choosing.

    python3 tools/drumref.py data/saves/ds10plus_00.ds10song [outdir]

The game renders the four one-second drum buffers when it loads a song, so this
packs the song you name into slot 0 of a save, boots the real thing under
melonDS with it, snapshots main RAM and reads the buffers back out. That makes a
reference for any drum patch and any of its nine effect bytes - which is what
`songplay`'s own render has to match, and what nothing before this could produce:
the oracle builds the buffer from an inference rather than from the game's code.

Needs `make ndsref` and the ROM. One run is about a minute of emulation.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drumbuf                                                 # noqa: E402
import sav                                                     # noqa: E402
import snap                                                    # noqa: E402

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")
BASE_SAVE = "songs/DS10PLUS.ds10save"
FRAME = int(os.environ.get("DRUMREF_FRAME", "2500"))

# Enough to get past the title and the "continue" prompt, which is when the save
# is read and the buffers are rendered. The taps are snapui.py's.
BOOT = """1180 touch 128 55
1195 release
2260 touch 128 58
2300 release
2360 touch 78 186
2400 release
"""


def render(song, outdir="out/drumref", frame=FRAME):
    """Boot with `song` in slot 0 and return its four rendered buffers."""
    os.makedirs(outdir, exist_ok=True)
    tag = os.path.splitext(os.path.basename(song))[0]
    save = os.path.join(outdir, tag + ".ds10save")
    if sav.cmd_pack([BASE_SAVE, "0", song, save]) != 0:
        raise SystemExit("could not pack %s into a save" % song)

    script = os.path.join(outdir, tag + ".script.txt")
    with open(script, "w") as f:
        f.write(BOOT + "%d snap\n" % frame)
    for f in os.listdir(outdir):
        if f.startswith(tag + ".f") and f.endswith(".snap"):
            os.remove(os.path.join(outdir, f))
    subprocess.run(["./out/ndsref", ROM, "-f", str(frame + 40), "-o", outdir,
                    "-s", script, "-e", "1000000", "-b", "1000000", "-m",
                    "-w", save], check=True,
                   stdout=subprocess.DEVNULL)
    snaps = sorted(f for f in os.listdir(outdir) if f.endswith(".snap"))
    if not snaps:
        raise SystemExit("no snapshot at frame %d" % frame)
    path = os.path.join(outdir, snaps[-1])
    ram = snap.load(path).ram
    base = drumbuf.find_record(ram)
    if base is None:
        raise SystemExit("%s: no live song record - is the song loaded?" % path)
    return drumbuf.buffers(ram, base), base, path


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    song = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "out/drumref"
    bufs, base, path = render(song, outdir)
    tag = os.path.splitext(os.path.basename(song))[0]
    print("%s: record at %#010x, from %s" % (tag, 0x02000000 + base, path))
    for d, b in enumerate(bufs):
        out = os.path.join(outdir, "%s.drum%d.wav" % (tag, d))
        drumbuf.wav(out, b)
        last = max((i for i, v in enumerate(b) if v not in (0, -1)), default=-1)
        print("  drum %d  peak %6d at %5d  ends %5d  %s"
              % (d, max(abs(v) for v in b),
                 max(range(len(b)), key=lambda i: abs(b[i])), last, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
