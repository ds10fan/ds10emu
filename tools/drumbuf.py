#!/usr/bin/env python3
"""drumbuf - the four rendered drum buffers, out of a RAM snapshot.

    python3 tools/snapui.py drums                     # boot and snapshot
    python3 tools/drumbuf.py out/snapui/drums/*.snap [song.ds10song]

A live song record in RAM is 0x43040 bytes and it is
`[4 x 0x10000 drum buffer][the song, with the live 0xB4 header]` - which is what
the 0x43040 stride at 0x020347EC is, and why the drum knobs sit at record
+0x42AF4: that is 0x40000 past the buffers plus the song's own 0x2AF4, the file's
0x2B2C less the 0x38 the live header is shorter. Each of the four drum part
objects (0x2E0 apart) holds a pointer to its buffer, and this finds the record by
those four pointers.

Given the song that was loaded, it also writes our own render beside each one -
`drumN.hw.wav` from the dump, `drumN.ours.wav` as songplay makes it, and
`drumN.ours-mixed.wav` through our mixer with that drum's nine effect bytes - and
says how far apart they are. Today: not close. Same pitch and character - the
zero-crossing rates agree to a percent - but the envelopes and levels do not, so
the "one second of a fresh voice at note 60" inference in songplay.c is wrong
about how the render is driven. See TODO.md section 5.
"""
import array
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snap                                                    # noqa: E402

BUFLEN = 0x10000            # bytes; 0x8000 samples, one second at 32768 Hz
RECORD = 0x43040
RATE = 32768
DRUMS = 4


def find_record(ram):
    """The record base, from the four buffer pointers the drum parts hold."""
    words = set()
    for off in range(0, len(ram) - 4, 4):
        v = struct.unpack_from("<I", ram, off)[0]
        if 0x02000000 <= v < 0x02400000:
            words.add(v)
    for v in sorted(words):
        if all(v + i * BUFLEN in words for i in range(1, DRUMS)):
            base = v - 0x02000000
            if base + RECORD <= len(ram):
                return base
    return None


def buffers(ram, base):
    out = []
    for d in range(DRUMS):
        a = array.array("h")
        a.frombytes(ram[base + d * BUFLEN:base + (d + 1) * BUFLEN])
        out.append(list(a))
    return out


def wav(path, samples):
    body = struct.pack("<%dh" % len(samples), *samples)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(body)) + body)


def zcr(v):
    return sum(1 for a, b in zip(v, v[1:]) if (a < 0) != (b < 0))


def corr(a, b):
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    sa = math.sqrt(sum(x * x for x in a)) or 1
    sb = math.sqrt(sum(x * x for x in b)) or 1
    return sum(x * y for x, y in zip(a, b)) / (sa * sb)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    ram = snap.load(sys.argv[1]).ram
    if not ram:
        print("%s has no main RAM - snapshot with ndsref -m" % sys.argv[1])
        return 1
    base = find_record(ram)
    if base is None:
        print("no live song record found")
        return 1
    print("record at %#010x, buffers at +0, +0x10000, +0x20000, +0x30000"
          % (0x02000000 + base))
    bufs = buffers(ram, base)
    outdir = os.path.join("out", "drumbuf")
    os.makedirs(outdir, exist_ok=True)
    for d, b in enumerate(bufs):
        path = os.path.join(outdir, "drum%d.hw.wav" % d)
        wav(path, b)
        last = max((i for i, v in enumerate(b) if v not in (0, -1)), default=-1)
        print("  drum %d  peak %6d at %5d  ends %5d  %s"
              % (d, max(abs(v) for v in b),
                 max(range(len(b)), key=lambda i: abs(b[i])), last, path))
    print("  the nine bytes each: %s"
          % [list(ram[base + 0x42B70 + d * 9:base + 0x42B70 + (d + 1) * 9])
             for d in range(DRUMS)])

    if len(sys.argv) > 2:
        import oracle
        import songtext
        p = songtext.read_song(sys.argv[2])
        print("\nagainst our own render of %s:" % os.path.basename(sys.argv[2]))
        for d in range(DRUMS):
            _, _, _, _, dry, wet = oracle.capture_drum_fx(p, d, frames=512)
            ours = [s for f in dry for s in f]
            mixed = [s for f, _ in wet for s in f]
            wav(os.path.join(outdir, "drum%d.ours.wav" % d), ours)
            wav(os.path.join(outdir, "drum%d.ours-mixed.wav" % d), mixed)
            print("  drum %d  peak %6d at %5d  zcr ours %5d theirs %5d"
                  "  corr %+.4f"
                  % (d, max(abs(v) for v in ours),
                     max(range(len(ours)), key=lambda i: abs(ours[i])),
                     zcr(ours[:4096]), zcr(bufs[d][:4096]),
                     corr(bufs[d], ours)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
