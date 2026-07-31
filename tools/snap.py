#!/usr/bin/env python3
"""snap - read the .snap files ndsref writes.

A snapshot is the whole 2D state of one frame plus the two framebuffers
melonDS composited from it. This module parses them; run it as a script to
write PNGs.

    python3 tools/snap.py png out/ref/*.snap      # -> alongside, .png
    python3 tools/snap.py info out/ref/f00000.snap
"""
import glob
import os
import struct
import sys

MAGIC = b"NDS2"
W, H = 256, 192
FB = W * H * 4

VRAM = [("A", 128), ("B", 128), ("C", 128), ("D", 128), ("E", 64),
        ("F", 16), ("G", 16), ("H", 32), ("I", 16)]

FLAT = [("ABG", 512), ("BBG", 128), ("AOBJ", 256), ("BOBJ", 128),
        ("ABGExtPal", 32), ("BBGExtPal", 32),
        ("AOBJExtPal", 8), ("BOBJExtPal", 8)]

ENGINE = [
    ("dispcnt", "<I", 1), ("bgcnt", "<H", 4), ("bgx", "<H", 4),
    ("bgy", "<H", 4), ("bgxref", "<i", 2), ("bgyref", "<i", 2),
    ("rota", "<h", 2), ("rotb", "<h", 2), ("rotc", "<h", 2), ("rotd", "<h", 2),
    ("win0", "<B", 4), ("win1", "<B", 4), ("wincnt", "<B", 4),
    ("bgmosaic", "<B", 2), ("objmosaic", "<B", 2), ("blendcnt", "<H", 1),
    ("eva", "<B", 1), ("evb", "<B", 1), ("evy", "<B", 1), ("_pad", "<B", 1),
]


class Snap:
    def __init__(self, blob):
        if blob[:4] != MAGIC:
            raise ValueError("not a snapshot")
        o = 4
        self.version, self.frame = struct.unpack_from("<II", blob, o)
        o += 8
        (self.powcnt, self.brightA, self.brightB,
         _) = struct.unpack_from("<4H", blob, o)
        o += 8
        self.vramcnt = list(blob[o:o + 9])
        self.vramstat = blob[o + 9]
        o += 12
        self.eng = []
        for _ in range(2):
            e, o = self._engine(blob, o)
            self.eng.append(e)
        self.palette = blob[o:o + 2048]
        o += 2048
        self.oam = blob[o:o + 2048]
        o += 2048
        self.vram = {}
        for name, kb in VRAM:
            self.vram[name] = blob[o:o + kb * 1024]
            o += kb * 1024
        # The nine banks composited down per the VRAMCNT mapping, which is what
        # the renderer actually reads.
        self.flat = {}
        for name, kb in FLAT:
            self.flat[name] = blob[o:o + kb * 1024]
            o += kb * 1024
        # The 3D engine's output plane, which the compositor treats as BG0 when
        # DISPCNT bit 3 is set. Alpha 0 means the pixel is transparent.
        self.plane3d = blob[o:o + FB]
        o += FB
        self.top = blob[o:o + FB]
        o += FB
        self.bot = blob[o:o + FB]

    @staticmethod
    def _engine(blob, o):
        e = {}
        for name, fmt, n in ENGINE:
            sz = struct.calcsize(fmt)
            v = [struct.unpack_from(fmt, blob, o + i * sz)[0] for i in range(n)]
            e[name] = v[0] if n == 1 else v
            o += sz * n
        return e, o

    def screens(self):
        """(top, bottom) framebuffers, honouring the display swap in POWCNT1."""
        return (self.top, self.bot)


def load(path):
    with open(path, "rb") as f:
        return Snap(f.read())


def to_png(raw, path):
    from PIL import Image
    # melonDS composites into 0xFFRRGGBB stored little-endian, so the bytes
    # come out B,G,R,A.
    img = Image.frombytes("RGBA", (W, H), raw)
    b, g, r, a = img.split()
    Image.merge("RGB", (r, g, b)).save(path)


def cmd_png(paths):
    for p in paths:
        s = load(p)
        stem = os.path.splitext(p)[0]
        to_png(s.top, stem + "_top.png")
        to_png(s.bot, stem + "_bot.png")
        print("%s -> %s_{top,bot}.png" % (p, os.path.basename(stem)))


def cmd_info(paths):
    for p in paths:
        s = load(p)
        print("%s  frame %d  powcnt %04x  bright %04x/%04x" %
              (p, s.frame, s.powcnt, s.brightA, s.brightB))
        print("  vramcnt %s" % " ".join("%02x" % v for v in s.vramcnt))
        for i, e in enumerate(s.eng):
            print("  engine %s: dispcnt %08x  bgcnt %s  blend %04x "
                  "eva/evb/evy %d/%d/%d" %
                  ("AB"[i], e["dispcnt"],
                   " ".join("%04x" % v for v in e["bgcnt"]),
                   e["blendcnt"], e["eva"], e["evb"], e["evy"]))
            print("    scroll %s" %
                  " ".join("(%d,%d)" % (x, y)
                           for x, y in zip(e["bgx"], e["bgy"])))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd, args = sys.argv[1], sys.argv[2:]
    paths = []
    for a in args:
        paths += sorted(glob.glob(a)) if "*" in a else [a]
    {"png": cmd_png, "info": cmd_info}[cmd](paths)


if __name__ == "__main__":
    main()
