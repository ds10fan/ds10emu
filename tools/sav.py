#!/usr/bin/env python3
"""sav - read and write the song slots of a DS-10 PLUS save file.

Subcommands:
  list <SAV>...                     what each slot holds
  extract <outdir> <SAV>...         write every written slot out as a song file
  pack <SAV> <slot> <song> <out>    put a song file back into a slot
  unpack <SAV> <dir> [--clean]      the whole save as editable text
  repack <dir> <out.SAV>            that text back into a save file

A save is 512 KiB and everything in it is MD5-checked the same way the rest of
the game is - the hash comes first, and it is what tells a written record from
untouched flash:

    0x00000   16  "DS10PSAVEDATA000"
    0x00010   12  00 ff ff ff, then eight zero bytes
    0x0001C   16  MD5 of the 28 bytes above. Constant across every save here,
                  so writing a slot leaves the header alone
    0x00100       18 song slots, stride 0x3000: 16-byte MD5 + 12252 payload
    0x38000       24 instrument slots, stride 0x100: the same 63-byte record
                  /data/demo/ds10synth*.dat holds, the rest of each slot zero.
                  A global bank, shared by every song and referenced by none:
                  nothing in a song points into it
    elsewhere     0xff

An unwritten song slot is 256 zero bytes then 0xff to the end of its stride; an
unwritten instrument slot is 256 zeros. `repack` builds a whole save from that,
so text is enough to make one - no original needed.

The scanner at 0x0205D088 walks the song slots at `save + 0x200 + slot*0x3000`;
on disk the run starts at 0x100 instead, with the same stride. This tool only
ever touches those 18 slots: nothing outside them refers to a slot, so a write
needs no other bookkeeping.

A slot is a fixed 12252-byte payload and its hash covers all of it, so `pack`
takes PLUS records only. A DS-10 or pre-release song file is shorter (the format
grew by appending), and padding one out would mean inventing the 224 bytes it
does not have.
"""
import glob
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import songtext                                                 # noqa: E402

SIZE = 0x80000
HEADER = b"DS10PSAVEDATA000" + bytes([0, 0xFF, 0xFF, 0xFF]) + bytes(8)
BASE = 0x100
STRIDE = 0x3000
PAYLOAD = 0x2FDC
RECORD = 16 + PAYLOAD
SLOTS = 18
BLANK_SLOT = 256          # an unwritten slot: this many zeros, then 0xff

PATCH_BASE = 0x38000
PATCH_STRIDE = 0x100
PATCH_SIZE = songtext.INSTR_SIZE
PATCHES = 24


def load(path):
    with open(path, "rb") as f:
        return f.read()


def hashed(rec):
    """True if this record's MD5 header covers what follows it."""
    return len(rec) > 16 and hashlib.md5(rec[16:]).digest() == rec[:16]


def slot_at(blob, i):
    o = BASE + i * STRIDE
    return blob[o:o + RECORD]


def slots(blob):
    """Yield (index, record) for every slot whose MD5 header checks out."""
    for i in range(SLOTS):
        rec = slot_at(blob, i)
        if len(rec) < RECORD:
            return
        if hashed(rec):
            yield i, rec


def patches(blob):
    """Yield (index, record) for every written patch-bank slot."""
    for i in range(PATCHES):
        o = PATCH_BASE + i * PATCH_STRIDE
        rec = blob[o:o + PATCH_SIZE]
        if hashed(rec):
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


def line(i, rec):
    name, tempo, swing, bars, used, live = describe(rec)
    return ("  slot %2d  %-7s %3d BPM  swing %3d%%  %3d bars"
            " (%2d distinct)  %2d patterns written"
            % (i, repr(name), tempo, swing, bars, used, live))


# ---------------------------------------------------------------- list

def cmd_list(args):
    for path in args:
        blob = load(path)
        written = dict(slots(blob))
        print("%s: %d of %d slots written, %d patches"
              % (os.path.basename(path), len(written), SLOTS,
                 len(list(patches(blob)))))
        for i in range(SLOTS):
            print(line(i, written[i]) if i in written
                  else "  slot %2d  -" % i)
    return 0


# ---------------------------------------------------------------- extract

def cmd_extract(args):
    if len(args) < 2:
        print("usage: sav.py extract <outdir> <SAV>...")
        return 1
    outdir, paths = args[0], args[1:]
    os.makedirs(outdir, exist_ok=True)
    total = 0
    for path in paths:
        blob = load(path)
        tag = os.path.splitext(os.path.basename(path))[0].lower()
        print("%s:" % os.path.basename(path))
        for i, rec in slots(blob):
            out = os.path.join(outdir, "%s_%02d.dat" % (tag, i))
            with open(out, "wb") as f:
                f.write(rec)
            total += 1
            print(line(i, rec))
    print("%d slots written to %s" % (total, outdir))
    return 0


# ---------------------------------------------------------------- pack

def cmd_pack(args):
    if len(args) != 4:
        print("usage: sav.py pack <SAV> <slot> <song> <out>")
        return 1
    path, which, song, out = args
    i = int(which, 0)
    if not 0 <= i < SLOTS:
        print("slot %d is outside 0..%d" % (i, SLOTS - 1))
        return 1
    blob = load(path)
    if len(blob) < BASE + SLOTS * STRIDE:
        print("%s: %d bytes, too short to hold %d slots"
              % (path, len(blob), SLOTS))
        return 1
    rec = load(song)
    if len(rec) != RECORD:
        print("%s: %d bytes, a slot needs exactly %d (16-byte MD5 + %d payload)"
              % (song, len(rec), RECORD, PAYLOAD))
        return 1
    if not hashed(rec):
        print("%s: MD5 header does not match the payload" % song)
        return 1
    was = slot_at(blob, i)
    o = BASE + i * STRIDE
    with open(out, "wb") as f:
        f.write(blob[:o] + rec + blob[o + RECORD:])
    print("%s slot %d: %s -> %s"
          % (out, i,
             repr(describe(was)[0]) if hashed(was) else "empty",
             repr(describe(rec)[0])))
    return 0


# ---------------------------------------------------------------- unpack

def cmd_unpack(args):
    clean = "--clean" in args
    args = [a for a in args if a != "--clean"]
    if len(args) != 2:
        print("usage: sav.py unpack <SAV> <dir> [--clean]")
        return 1
    path, outdir = args
    blob = load(path)
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for i, rec in slots(blob):
        text = songtext.dumps(rec[16:], clean=clean,
                              source="%s slot %d" % (os.path.basename(path), i))
        with open(os.path.join(outdir, "slot%02d.txt" % i), "w") as f:
            f.write(text)
        n += 1
    bank = list(patches(blob))
    lines = ["# %s instrument bank: %d of %d slots written." % (
        os.path.basename(path), len(bank), PATCHES),
        "# Global - shared by every song, and referenced by none of them."]
    for i, rec in bank:
        lines += songtext.instr_text(i, rec)
    with open(os.path.join(outdir, "bank.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d songs, %d instruments%s"
          % (outdir, n, len(bank), " (clean)" if clean else ""))
    return 0


# ---------------------------------------------------------------- repack

def cmd_repack(args):
    if len(args) != 2:
        print("usage: sav.py repack <dir> <out.SAV>")
        return 1
    src, out = args
    blob = bytearray(b"\xff" * SIZE)
    blob[:len(HEADER)] = HEADER
    blob[len(HEADER):len(HEADER) + 16] = hashlib.md5(HEADER).digest()
    for i in range(SLOTS):
        o = BASE + i * STRIDE
        txt = os.path.join(src, "slot%02d.txt" % i)
        if os.path.exists(txt):
            payload = songtext.loads(open(txt).read())
            if len(payload) != PAYLOAD:
                print("%s: payload is %d bytes, a slot needs %d"
                      % (txt, len(payload), PAYLOAD))
                return 1
            blob[o:o + RECORD] = hashlib.md5(payload).digest() + bytes(payload)
        else:
            blob[o:o + BLANK_SLOT] = bytes(BLANK_SLOT)
    bank = {}
    txt = os.path.join(src, "bank.txt")
    if os.path.exists(txt):
        bank = songtext.bank_loads(open(txt).read())
    for i in range(PATCHES):
        o = PATCH_BASE + i * PATCH_STRIDE
        blob[o:o + PATCH_STRIDE] = bytes(PATCH_STRIDE)
        if i in bank:
            blob[o:o + PATCH_SIZE] = bank[i]
    with open(out, "wb") as f:
        f.write(blob)
    songs = len(glob.glob(os.path.join(src, "slot??.txt")))
    print("%s: %d bytes, %d songs, %d instruments"
          % (out, len(blob), songs, len(bank)))
    return 0


# ---------------------------------------------------------------- main

def main():
    cmds = {"list": cmd_list, "extract": cmd_extract, "pack": cmd_pack,
            "unpack": cmd_unpack, "repack": cmd_repack}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    return cmds[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
