#!/usr/bin/env python3
"""pages - build each screen's display list out of a captured frame.

The interface draws by expanding NCER cells into OAM, so the OAM can be read
back the other way: `tools/cells.py` finds which cell drew each run, and this
turns that into a list of "cell, where, how" per page. That is what the
reimplementation draws from - a list it can redraw when something moves, not a
frozen frame of OAM.

    python3 tools/pages.py out/sweep out/sweep2 data/pages.bin

A page is identified by looking for its tile image in the frame's VRAM, so the
naming is the game's own. Items that cannot be attributed to a cell are kept
as raw OAM entries, so a page is never worse off than the frozen frame was.

Format, little-endian:

    "PAGE" u32 version=3 u32 npages
    per page:
        u8 namelen, name, u8 engine, u16 nrecords
        per record, in the order the game drew them:
            u8 kind
            kind 0: u8 banklen, bank, u16 cell, u8 prio, u8 flags,
                    u8 affine, u8 pad, u16 base, i32 x, i32 y, i32 mtx[4]
            kind 1: u8[6] one OAM entry that no cell explained
    flags: bit 0 the cell was rotated, bit 1 double-size

Order matters: sprites of the same priority are resolved by OAM index, so a
record that came first has to stay first.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cells as cellmod
import nitro
import snap

VERSION = 4

# The sheets src/app/ui.c actually loads into OBJ VRAM. A cell taken from any
# other bank still names tiles, but they resolve through whichever sheet is at
# that base - so prefer these when more than one bank explains a run.
LOADED = {"CHARACTER_c": 3, "CHARACTER_S": 2, "DS10FONT": 1}

# The step grid, from src/app/ui.c - the same numbers the
# constructor gives.
SEQ_X0, SEQ_Y0, SEQ_CELL, SEQ_CURSOR_Y = 45, 35, 13, 20


def page_index(assetdir):
    out = []
    for f in sorted(glob.glob(os.path.join(assetdir, "*.NCGR"))):
        try:
            g = nitro.Ncgr(open(f, "rb").read())
        except Exception:
            continue
        if len(g.data) >= 2048:
            out.append((os.path.basename(f)[:-5], g.data))
    return out


def identify(s, index):
    """(bottom, top) page names, by finding each page's tiles in VRAM."""
    found = [None, None]
    for slot, (reg, off) in enumerate((("ABG", 0x4000), ("BBG", 0x4000))):
        vram = s.flat[reg]
        best = None
        for name, data in index:
            n = min(len(data), len(vram) - off)
            if n > 0 and vram[off:off + n] == data[:n]:
                if best is None or len(data) > best[1]:
                    best = (name, len(data))
        if best:
            found[slot] = best[0]
    return found


def onscreen(a0, a1):
    if ((a0 >> 8) & 3) == 2:
        return False
    y = a0 & 0xFF
    x = a1 & 0x1FF
    if x & 0x100:
        x -= 512
    return y < 192 and -64 < x < 256


def build(half, banks):
    """A page's display list, plus whatever could not be attributed."""
    oam = cellmod.oam_entries(half)
    rec = []
    slot = 0
    while slot < 128:
        a0, a1, _ = oam[slot]
        if not onscreen(a0, a1):
            slot += 1
            continue
        best = None
        bestkey = None
        for base in cellmod.TILE_BASES:
            for name, cl in banks:
                for idx, cell in enumerate(cl):
                    m = cellmod.match(cell, oam, slot, base)
                    if not m:
                        continue
                    # Longest run wins, and between equals prefer a sheet that
                    # is actually in OBJ VRAM. A cell from a bank that is not
                    # loaded resolves its tiles through whatever is at that
                    # base instead, which draws the wrong thing - that is what
                    # put a 71_KAOSS_c cell on the pan page.
                    key = (len(cell), LOADED.get(name, 0))
                    if bestkey is None or key > bestkey:
                        bestkey = key
                        best = (name, idx, m, base, len(cell))
        if best is None:
            rec.append((1, half[slot * 8:slot * 8 + 6]))
            slot += 1
            continue
        name, idx, (_, _, prio, rot), base, n = best
        cell = dict(banks)[name][idx]
        mtx = [0x1000, 0, 0, 0x1000]
        affine = 0
        dbl = 0
        if rot:
            affine, mtx = cellmod.affine_of(half, slot)
            dbl = 1 if ((a0 >> 8) & 3) == 3 else 0
        pos = cellmod.solve_pos(cell, mtx if rot else None, affine, dbl, base,
                                oam, slot)
        rec.append((0, (name, idx, prio, (1 if rot else 0) | (dbl << 1),
                        affine, base, pos[0], pos[1], mtx)))
        slot += n
    return rec


# Records that are state, not chrome.
#
# A captured frame shows whatever pattern happened to be loaded, and keeping
# those records would freeze someone else's notes into the page. The
# reimplementation draws them from its own pattern instead - so they come out
# here, and `make test-page` still has to reproduce the frame, which is what
# says the two agree.
#
# For the synth sequencer that is the step markers (cell 7 of the page's own
# sheet) and the cursor above the grid (CHARACTER_c cell 4, the only sprite in
# that band).
def is_state(page, rec):
    if rec[0] != 0:
        return False
    name, idx, prio, flags, affine, base, x, y, mtx = rec[1]
    x >>= 12
    y >>= 12
    # Identify a marker by where it is, not by which cell explains it. The
    # same run is described equally well by cell 7 of the piano roll's sheet
    # and by cell 8 of CHARACTER_c, and which one the matcher picks depends on
    # its tie-break - so a name-based test here silently stops filtering when
    # that changes, and the captured pattern gets baked back into the page.
    if page.startswith("60_SYN_SEQ_") or page == "51_ALL_DRUMS":
        if x >= SEQ_X0 and y >= SEQ_Y0 and \
           (x - SEQ_X0) % SEQ_CELL == 0 and (y - SEQ_Y0) % SEQ_CELL == 0:
            return True
        if y == SEQ_CURSOR_Y and x >= SEQ_X0 and (x - SEQ_X0 - 1) % SEQ_CELL == 0:
            return True
    return False


def main():
    """pages.py <snapdir> [snapdir ...] <out.bin>"""
    args = sys.argv[1:] or ["out/sweep", "data/pages.bin"]
    srcdirs, out = args[:-1], args[-1]
    index = page_index("data/nitrofs/mainbg")
    banks = []
    for f in sorted(glob.glob("data/nitrofs/mainobj/*.NCER")):
        cl = cellmod.load_bank(f)
        if cl:
            banks.append((os.path.basename(f)[:-5], cl))

    seen = {}
    snaps = []
    for d in srcdirs:
        snaps += sorted(glob.glob(os.path.join(d, "*.snap")))
    for p in snaps:
        s = snap.load(p)
        bot, top = identify(s, index)
        for name, engine, half in ((bot, 0, s.oam[:1024]),
                                   (top, 1, s.oam[1024:])):
            if name and name not in seen:
                rec = [r for r in build(half, banks) if not is_state(name, r)]
                seen[name] = (engine, rec)

    with open(out, "wb") as f:
        f.write(b"PAGE")
        f.write(struct.pack("<II", VERSION, len(seen)))
        for name in sorted(seen):
            engine, rec = seen[name]
            f.write(struct.pack("<B", len(name)))
            f.write(name.encode("latin1"))
            f.write(struct.pack("<BH", engine, len(rec)))
            for kind, body in rec:
                f.write(struct.pack("<B", kind))
                if kind:
                    f.write(body)
                    continue
                bank, cell, prio, flags, affine, base, x, y, mtx = body
                f.write(struct.pack("<B", len(bank)))
                f.write(bank.encode("latin1"))
                f.write(struct.pack("<HBBBBH", cell, prio, flags, affine, 0,
                                    base))
                f.write(struct.pack("<2i", x, y))
                f.write(struct.pack("<4i", *mtx))

    print("%s: %d pages" % (out, len(seen)))
    for name in sorted(seen):
        engine, rec = seen[name]
        ncell = sum(1 for k, _ in rec if k == 0)
        print("  %-22s engine %s  %3d cells, %2d raw"
              % (name, "AB"[engine], ncell, len(rec) - ncell))



if __name__ == "__main__":
    main()
