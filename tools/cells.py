#!/usr/bin/env python3
"""cells - work out which NCER cell drew each run of sprites on a screen.

The interface draws by expanding a cell into OAM entries and appending them,
so a cell's entries are contiguous and in order. That makes the expansion
invertible: walk the OAM, and at each slot try every cell in the banks the
screen has loaded. A cell matches when its entries agree with the OAM on
everything the expander does not change - tile, palette, shape, size, flips,
mode - and when the positions differ from the cell's own by one constant
offset.

    python3 tools/cells.py out/sweep/f03900.snap

What comes out is a screen's display list: cell, where it was put, and which
priority it was stamped with. Feeding that back through the expander
reproduces the sprite layer, which is the point - a page that is drawn from a
display list can be redrawn when something moves.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nitro
import snap

# Two draw calls, two banks. 0x02022c38 adds CHARACTER_c's base of 0x200 to
# every tile; 0x02022d38 is the same code with no base, for the sheet at
# tile 0 - which is DS10FONT.
TILE_BASES = (0x200, 0x000)


def load_bank(path):
    """Every cell of an NCER, as a list of (attr0, attr1, attr2) lists."""
    blob = open(path, "rb").read()
    d = dict(nitro.blocks(blob)[1])
    if "CEBK" not in d:
        return []
    c = d["CEBK"]
    n, ctype = struct.unpack_from("<HH", c, 0)
    coff = struct.unpack_from("<I", c, 4)[0]
    stride = 16 if ctype == 1 else 8
    objbase = coff + n * stride
    out = []
    for i in range(n):
        e = coff + i * stride
        nobj = struct.unpack_from("<H", c, e)[0]
        off = struct.unpack_from("<I", c, e + 4)[0]
        objs = [struct.unpack_from("<3H", c, objbase + off + k * 6)
                for k in range(nobj)]
        out.append(objs)
    return out


def oam_entries(half):
    out = []
    for slot in range(128):
        a0, a1, a2 = struct.unpack_from("<3H", half, slot * 8)
        out.append((a0, a1, a2))
    return out


def sign9(v):
    return v - 512 if v & 0x100 else v


def sign8(v):
    return v - 256 if v & 0x80 else v


def match(cell, oam, slot, tile_base):
    """Does `cell` explain the OAM run at `slot`? Returns (dx, dy, prio, rot).

    What the expander is allowed to change: attr0 bits 8-9 (the rot/scale
    flags), attr1 bits 9-13 (the affine index) and the position. Everything
    else - shape, size, colour depth, mode, mosaic, tile, palette - it copies,
    and the priority it stamps uniformly. A rotated run's positions have been
    through the matrix, so no constant offset is required of it.
    """
    n = len(cell)
    if not n or slot + n > 128:
        return None
    dx = dy = prio = None
    rot = False
    for k, (c0, c1, c2) in enumerate(cell):
        o0, o1, o2 = oam[slot + k]
        if (c0 & 0xFC00) != (o0 & 0xFC00):
            return None
        if ((o0 >> 8) & 3) in (1, 3):
            rot = True
            if (c1 & 0xC000) != (o1 & 0xC000):
                return None
        else:
            if (c1 & 0xFE00) != (o1 & 0xFE00):
                return None
        if ((c2 & 0x3FF) + tile_base) & 0x3FF != (o2 & 0x3FF):
            return None
        if (c2 & 0xF000) != (o2 & 0xF000):
            return None
        p = (o2 >> 10) & 3
        if prio is None:
            prio = p
        elif prio != p:
            return None
        ddx = sign9(o1 & 0x1FF) - sign9(c1 & 0x1FF)
        ddy = sign8(o0 & 0xFF) - sign8(c0 & 0xFF)
        if dx is None:
            dx, dy = ddx, ddy
        elif (dx, dy) != (ddx, ddy) and not rot:
            return None
    return dx, dy, prio, rot


def display_list(oam, banks):
    """Greedy left-to-right cover of the OAM by cells, longest first."""
    out = []
    slot = 0
    while slot < 128:
        a0 = oam[slot][0]
        if ((a0 >> 8) & 3) == 2:  # disabled
            slot += 1
            continue
        best = None
        for base in TILE_BASES:
            for name, cells in banks:
                for idx, cell in enumerate(cells):
                    m = match(cell, oam, slot, base)
                    if m and (best is None or len(cell) > best[3]):
                        best = (name, idx, m, len(cell))
        if best is None:
            out.append(("?", -1, slot, None))
            slot += 1
            continue
        name, idx, (dx, dy, prio, rot), n = best
        out.append((name, idx, slot, (dx, dy, prio, n, rot)))
        slot += n
    return out


OBJ_W = [8, 16, 32, 64, 16, 32, 32, 64, 8, 8, 16, 32, 4096, 0, 4096, 0]
OBJ_H = [8, 16, 32, 64, 8, 8, 16, 32, 16, 32, 32, 64, 8, 16, 32, 64]


def dim_index(attr):
    return ((attr & 0xC000) >> 14) * 4 + ((attr >> 30) & 3)


def expand(cell, mtx, pos, affine, dbl, base):
    """Our port of 0x02069564, checked against the C in src/app/cell.c by
    make test-page: if this disagrees, the regenerated page does not match."""
    out = []
    for (c0, c1, c2) in cell:
        attr = c0 | (c1 << 16)
        x = (attr & 0x01FF0000) >> 16
        if x > 0xFF:
            x -= 0x200
        x *= 0x1000
        y = attr & 0xFF
        if y > 0x7F:
            y -= 0x100
        y *= 0x1000
        if mtx:
            rot = attr & 0x300
            if rot not in (0x100, 0x300):
                rot = attr & 0x30000300
            if rot == 0x300:
                x += OBJ_W[dim_index(attr)] * 0x800
                y += OBJ_H[dim_index(attr)] * 0x800
            rot = 0x300 if dbl else 0x100
            px = x * mtx[0] + mtx[2] * y + 0x1000
            py = x * mtx[1] + mtx[3] * y + 0x1000
            attr = (attr & 0xC1FFFCFF) | rot | (affine << 25)
            hw = OBJ_W[dim_index(attr)] >> 1
            hh = OBJ_H[dim_index(attr)] >> 1
            nx = (px >> 12) + mtx[2] * hh + mtx[0] * hw - hw * 0x1000
            y = (py >> 12) + mtx[3] * hh + mtx[1] * hw - hh * 0x1000
            if rot == 0x300:
                nx -= hw * 0x1000
                y -= hh * 0x1000
            x = nx
        if pos:
            x += pos[0]
            y += pos[1]
        attr = (attr & 0xFE00FF00) | (((y + 0x800) >> 12) & 0xFF) \
            | ((((x + 0x800) >> 12) & 0x1FF) << 16)
        tile = ((c2 & 0x3FF) + base) & 0x3FF
        out.append((attr & 0xFFFF, (attr >> 16) & 0xFFFF,
                    (c2 & ~0x3FF) | tile))
    return out


def affine_of(half, slot):
    """The 2x2 matrix in the affine slot this entry names, as 20.12."""
    a1 = struct.unpack_from("<H", half, slot * 8 + 2)[0]
    idx = (a1 >> 9) & 0x1F
    m = []
    for k in range(4):
        v = struct.unpack_from("<h", half, idx * 0x20 + 6 + k * 8)[0]
        m.append(v * 16)   # 8.8 back up to 20.12
    return idx, m


def solve_pos(cell, mtx, affine, dbl, base, oam, slot):
    """The translation that puts this cell where the OAM has it."""
    got = expand(cell, mtx, None, affine, dbl, base)
    o0, o1, _ = oam[slot]
    gx = o1 & 0x1FF
    if gx > 0xFF:
        gx -= 0x200
    gy = o0 & 0xFF
    if gy > 0x7F:
        gy -= 0x100
    cx = got[0][1] & 0x1FF
    if cx > 0xFF:
        cx -= 0x200
    cy = got[0][0] & 0xFF
    if cy > 0x7F:
        cy -= 0x100
    return ((gx - cx) * 0x1000, (gy - cy) * 0x1000)


def main():
    path = sys.argv[1]
    s = snap.load(path)
    banks = []
    for f in sorted(glob.glob("data/nitrofs/mainobj/*.NCER")):
        cells = load_bank(f)
        if cells:
            banks.append((os.path.basename(f)[:-5], cells))
    for engine, half in ((0, s.oam[:1024]), (1, s.oam[1024:])):
        oam = oam_entries(half)
        dl = display_list(oam, banks)
        named = [d for d in dl if d[1] >= 0]
        covered = sum(d[3][3] for d in named)
        live = sum(1 for e in oam if ((e[0] >> 8) & 3) != 2)
        print("engine %s: %d live sprites, %d explained by %d cells"
              % ("AB"[engine], live, covered, len(named)))
        for name, idx, slot, info in dl[:24]:
            if idx < 0:
                print("    slot %3d  unexplained" % slot)
            else:
                dx, dy, prio, n, rot = info
                print("    slot %3d  %s cell %-3d at (%4d,%4d) prio %d  x%d%s"
                      % (slot, name, idx, dx, dy, prio, n,
                         "  rotated" if rot else ""))


if __name__ == "__main__":
    main()
