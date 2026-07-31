#!/usr/bin/env python3
"""nitro - the Nintendo NITRO asset containers DS-10 stores its interface in.

All of them are the same shape: an 16-byte header naming the file, then a run
of blocks, each an 8-byte header and a payload. The magics are stored
byte-reversed, so an NCGR file begins "RGCN".

    python3 tools/nitro.py info  data/nitrofs/mainbg/*.NCGR
    python3 tools/nitro.py png   data/nitrofs/mainbg/61_SYN_EDIT   # + .NCLR
    python3 tools/nitro.py vram  out/sweep/f01234.snap             # what is
                                                                   # loaded

`png` renders a background: the NSCR names tiles out of the NCGR, coloured by
the NCLR. That is the same composition the 2D hardware does, so it is a check
on the parse before any of it reaches the renderer.
"""
import glob
import os
import struct
import sys


def blocks(blob):
    """Yield (magic, payload) for each block, after the file header."""
    if len(blob) < 16:
        raise ValueError("too short for a NITRO header")
    magic = blob[:4][::-1].decode("latin1")
    size, hdrlen, nblocks = struct.unpack_from("<IHH", blob, 8)
    o = hdrlen
    out = []
    for _ in range(nblocks):
        if o + 8 > len(blob):
            break
        bmagic = blob[o:o + 4][::-1].decode("latin1")
        blen = struct.unpack_from("<I", blob, o + 4)[0]
        if blen < 8:
            break
        out.append((bmagic, blob[o + 8:o + blen]))
        o += blen
    return magic, out


class Ncgr:
    """Character (tile) graphics. Also NCBR, which is the same block with the
    linear flag set: the pixels are one bitmap rather than a run of tiles."""

    def __init__(self, blob):
        magic, bl = blocks(blob)
        d = dict(bl)
        if "CHAR" not in d:
            raise ValueError("%s: no CHAR block" % magic)
        c = d["CHAR"]
        (self.height, self.width, self.fmt, self.mapping, self.linear,
         self.datalen, self.dataoff) = struct.unpack_from("<HHIIIII", c, 0)
        self.bpp = 8 if self.fmt == 4 else 4
        self.data = c[self.dataoff:self.dataoff + self.datalen]
        # A tiled file with 0xFFFF dimensions has no shape of its own; the
        # tilemap or the cell bank decides.
        if self.width == 0xFFFF:
            self.width = self.height = None

    def tiles(self):
        """Number of 8x8 tiles the payload holds."""
        return len(self.data) // (64 if self.bpp == 8 else 32)

    def tile_pixels(self, n):
        """Palette indices of tile n, row-major, 64 of them."""
        if self.bpp == 8:
            return list(self.data[n * 64:n * 64 + 64])
        out = []
        for b in self.data[n * 32:n * 32 + 32]:
            out.append(b & 0x0F)
            out.append(b >> 4)
        return out


class Nclr:
    """Palette."""

    def __init__(self, blob):
        magic, bl = blocks(blob)
        d = dict(bl)
        if "PLTT" not in d:
            raise ValueError("%s: no PLTT block" % magic)
        p = d["PLTT"]
        self.fmt, self.extended, self.datalen, dataoff = \
            struct.unpack_from("<IIII", p, 0)
        raw = p[8 + dataoff:8 + dataoff + self.datalen]
        self.colors = list(struct.unpack("<%dH" % (len(raw) // 2), raw))

    def rgb(self, i):
        c = self.colors[i] if i < len(self.colors) else 0
        r, g, b = c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)


class Nscr:
    """Tilemap: 16 bits per cell, naming a tile, its flips and its palette."""

    def __init__(self, blob):
        magic, bl = blocks(blob)
        d = dict(bl)
        if "SCRN" not in d:
            raise ValueError("%s: no SCRN block" % magic)
        s = d["SCRN"]
        self.width, self.height, self.mode, self.datalen = \
            struct.unpack_from("<HHII", s, 0)
        raw = s[12:12 + self.datalen]
        self.cells = list(struct.unpack("<%dH" % (len(raw) // 2), raw))

    def cell(self, x, y):
        v = self.cells[y * (self.width // 8) + x]
        return v & 0x03FF, bool(v & 0x0400), bool(v & 0x0800), v >> 12


def render_bg(ncgr, nscr, nclr):
    """Compose a tilemap into an RGB image, the way the hardware would."""
    from PIL import Image
    w, h = nscr.width, nscr.height
    img = Image.new("RGB", (w, h))
    px = img.load()
    for ty in range(h // 8):
        for tx in range(w // 8):
            tile, xflip, yflip, pal = nscr.cell(tx, ty)
            pix = ncgr.tile_pixels(tile)
            for y in range(8):
                for x in range(8):
                    sx = 7 - x if xflip else x
                    sy = 7 - y if yflip else y
                    i = pix[sy * 8 + sx]
                    if ncgr.bpp == 4:
                        i = pal * 16 + i
                    px[tx * 8 + x, ty * 8 + y] = nclr.rgb(i)
    return img


def cmd_info(paths):
    for p in paths:
        blob = open(p, "rb").read()
        try:
            magic, bl = blocks(blob)
        except ValueError as e:
            print("%s: %s" % (p, e))
            continue
        desc = " ".join("%s:%d" % (m, len(d)) for m, d in bl)
        extra = ""
        try:
            if magic in ("NCGR", "NCBR"):
                g = Ncgr(blob)
                extra = "  %dbpp %s tiles=%d linear=%d map=%d" % (
                    g.bpp,
                    "%sx%s" % (g.width, g.height) if g.width else "-",
                    g.tiles(), g.linear, g.mapping)
            elif magic == "NCLR":
                c = Nclr(blob)
                extra = "  %d colours fmt=%d ext=%d" % (
                    len(c.colors), c.fmt, c.extended)
            elif magic == "NSCR":
                s = Nscr(blob)
                extra = "  %dx%d mode=%d" % (s.width, s.height, s.mode)
        except (ValueError, struct.error) as e:
            extra = "  (%s)" % e
        print("%-52s %s  %s%s" % (os.path.basename(p), magic, desc, extra))


def cmd_png(stems):
    for stem in stems:
        ncgr = Ncgr(open(stem + ".NCGR", "rb").read())
        nscr = Nscr(open(stem + ".NSCR", "rb").read())
        pal = stem + ".NCLR"
        if not os.path.exists(pal):
            pal = "data/nitrofs/mainbg/PALETTE.NCLR"
        nclr = Nclr(open(pal, "rb").read())
        out = os.path.join("out", os.path.basename(stem) + ".png")
        os.makedirs("out", exist_ok=True)
        render_bg(ncgr, nscr, nclr).save(out)
        print("%s -> %s" % (stem, out))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd, args = sys.argv[1], sys.argv[2:]
    paths = []
    for a in args:
        paths += sorted(glob.glob(a)) if "*" in a else [a]
    {"info": cmd_info, "png": cmd_png}[cmd](paths)


if __name__ == "__main__":
    main()
