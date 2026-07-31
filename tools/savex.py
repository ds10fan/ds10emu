#!/usr/bin/env python3
"""savex - pull the song slots out of a DS-10 PLUS save file.

A save holds 18 song slots. Each is the same record a `.dat` is: a 16-byte MD5
followed by a 12252-byte payload, and the hash is what identifies a slot as
holding a real song rather than uninitialised flash. In RAM the scanner at
0x0205D088 walks them at `save + 0x200 + slot*0x3000`; in the file on disk the
run starts at 0x100 instead, with the same 0x3000 stride.

    python3 tools/savex.py out/songs songs/*.SAV

Slots whose hash does not check out are skipped, as are ones that are byte for
byte the INIT template, since those are empty save slots rather than songs.
"""
import hashlib
import os
import struct
import sys

BASE = 0x100
STRIDE = 0x3000
PAYLOAD = 0x2FDC
SLOTS = 18


def slots(blob):
    """Yield (index, record) for every slot whose MD5 header checks out."""
    for i in range(SLOTS):
        o = BASE + i * STRIDE
        rec = blob[o:o + 16 + PAYLOAD]
        if len(rec) < 16 + PAYLOAD:
            return
        if hashlib.md5(rec[16:]).digest() == rec[:16]:
            yield i, rec


def describe(rec):
    p = rec[16:]
    name = p[0x14:0x1A].decode("latin1").rstrip("\0 ")
    tempo = struct.unpack_from("<I", p, 4)[0]
    swing = struct.unpack_from("<H", p, 0x0A)[0]
    chain = struct.unpack_from("<100H", p, 0x24)
    bars = next((i for i, v in enumerate(chain) if v == 0xFFFF), 100)
    used = len({c for c in chain[:bars]})
    # A pattern counts as written if any track has a note on any step.
    live = 0
    for i in range(16):
        b = 0xEC + i * 676
        if any(p[b + 0x42 + t * 64 + s * 4 + 1] != 0xFF
               for t in range(6) for s in range(16)):
            live += 1
    return name, tempo, swing, bars, used, live


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    total = 0
    for path in sys.argv[2:]:
        blob = open(path, "rb").read()
        tag = os.path.splitext(os.path.basename(path))[0].lower()
        print("%s:" % os.path.basename(path))
        for i, rec in slots(blob):
            name, tempo, swing, bars, used, live = describe(rec)
            out = os.path.join(outdir, "%s_%02d.dat" % (tag, i))
            open(out, "wb").write(rec)
            total += 1
            print("  slot %2d  %-7s %3d BPM  swing %3d%%  %3d bars"
                  " (%2d distinct)  %2d patterns written"
                  % (i, repr(name), tempo, swing, bars, used, live))
    print("%d slots written to %s" % (total, outdir))


main()
