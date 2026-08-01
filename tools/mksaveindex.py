#!/usr/bin/env python3
"""mksaveindex - list the songs tools/savex.py pulled out of the saves.

    python3 tools/mksaveindex.py data/saves web/saves.js

The page needs their names, not just their filenames, and the names live
inside the payload. This writes them out as a small script the page loads.
"""
import glob
import os
import struct
import sys


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "data/saves"
    out = sys.argv[2] if len(sys.argv) > 2 else "web/saves.js"
    rows = []
    for f in sorted(glob.glob(os.path.join(src, "*.dat"))):
        b = open(f, "rb").read()[16:]
        name = "".join(chr(c) if 32 <= c < 127 else "" for c in b[0x14:0x1A])
        tempo = struct.unpack_from("<I", b, 4)[0]
        bars = 0
        for i in range(100):
            if struct.unpack_from("<H", b, 0x24 + i * 2)[0] == 0xFFFF:
                break
            bars += 1
        stem = os.path.basename(f)[:-4]
        rows.append((stem, name.strip() or stem, tempo, bars))
    with open(out, "w") as o:
        o.write("/* The songs in songs/*.SAV, extracted at build time by\n"
                " * tools/savex.py. Generated - do not edit. */\n")
        o.write('"use strict";\nconst SAVED_SONGS = [\n')
        for stem, name, tempo, bars in rows:
            o.write('  ["%s", "%s  %d bpm  %d bars"],\n'
                    % (stem, name, tempo, bars))
        o.write("];\n")
    print("%s: %d songs" % (out, len(rows)))


if __name__ == "__main__":
    main()
