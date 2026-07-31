#!/usr/bin/env python3
"""pages - work out which page each captured frame is showing, and save its
sprite layer.

The backgrounds are already reproduced from the asset files. The sprites are
not: they are placed by widget code that is still being read, so until that is
done the OAM is taken from the captures instead. Each entry here is a page
name and the 128 OAM entries the game had set while that page was up.

    python3 tools/pages.py out/sweep data/pages.bin

A page is identified by looking for each /data/mainbg tile image in the frame's
VRAM, so the naming is the game's own, not a guess.

Format (little-endian):

    "PAGE" u32 version u32 count
    per page:  u8 namelen, name, u8 engine, u8[1024] OAM
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro
import snap

VERSION = 1


def page_index(assetdir):
    """Map each page's first tiles to its name, for identification."""
    out = []
    for f in sorted(glob.glob(os.path.join(assetdir, "*.NCGR"))):
        name = os.path.basename(f)[:-5]
        try:
            g = nitro.Ncgr(open(f, "rb").read())
        except Exception:
            continue
        if len(g.data) < 2048:
            continue
        out.append((name, g.data))
    return out


def identify(s, index):
    """(bottom, top) page names for this frame, or None where unknown."""
    found = [None, None]
    # Engine A draws the bottom screen and reads BG-A; engine B the top.
    for slot, (reg, off) in enumerate((("ABG", 0x4000), ("BBG", 0x4000))):
        vram = s.flat[reg]
        best = None
        for name, data in index:
            n = min(len(data), len(vram) - off)
            if n <= 0:
                continue
            if vram[off:off + n] == data[:n]:
                # Prefer the longest match: several pages share a prefix.
                if best is None or len(data) > best[1]:
                    best = (name, len(data))
        if best:
            found[slot] = best[0]
    return found


def main():
    srcdir = sys.argv[1] if len(sys.argv) > 1 else "out/sweep"
    out = sys.argv[2] if len(sys.argv) > 2 else "data/pages.bin"
    index = page_index("data/nitrofs/mainbg")
    seen = {}
    for p in sorted(glob.glob(os.path.join(srcdir, "*.snap"))):
        s = snap.load(p)
        bot, top = identify(s, index)
        # The engine that is showing this page owns the matching half of OAM.
        if bot and bot not in seen:
            seen[bot] = (0, s.oam[:1024])
        if top and top not in seen:
            seen[top] = (1, s.oam[1024:])
    with open(out, "wb") as f:
        f.write(b"PAGE")
        f.write(struct.pack("<II", VERSION, len(seen)))
        for name in sorted(seen):
            engine, oam = seen[name]
            f.write(struct.pack("<B", len(name)))
            f.write(name.encode("latin1"))
            f.write(struct.pack("<B", engine))
            f.write(oam)
    print("%s: %d pages" % (out, len(seen)))
    for name in sorted(seen):
        engine, oam = seen[name]
        live = sum(1 for n in range(128)
                   if (struct.unpack_from("<H", oam, n * 8)[0] >> 8) & 3 != 2)
        print("  %-22s engine %s  %3d sprites" % (name, "AB"[engine], live))


main()
