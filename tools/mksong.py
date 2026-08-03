#!/usr/bin/env python3
"""mksong - write a DS-10 song file from scratch, as a listening test.

Takes an INIT song as the container and fills in one pattern: synth 1 playing a
scale on a bare sawtooth with the filter open, everything else silent and the
effect off. If that comes out as a recognisable scale at the right pitches, the
note-to-pitch path and the whole chain downstream of it are right.

    python3 tools/mksong.py out/scale.ds10song [root] [bars]

The result is a real song file - MD5 header and all - so the original would load
it too.
"""
import hashlib
import struct
import sys

INIT = "out/nitrofs/data/demo/ds10init.dat"
PAT = 0xEC
PSIZE = 676
STEPS = 16
NO_NOTE = 0xFF

# Knob order is the 31 in recon.md section 4. A bare saw: one oscillator, filter
# wide open and not modulated, envelope straight out of the VCA.
KNOBS = {
    0: 0,     # octave
    6: 1,     # vco1 waveform: saw
    7: 1,     # vco2 waveform (silent anyway)
    9: 0,     # balance: vco1 only
    11: 127,  # cutoff wide open
    12: 0,    # peak
    13: 0,    # vcf type: low pass
    16: 1,    # vca follows the envelope
    19: 110,  # level
    20: 0,    # attack
    21: 90,   # decay
    22: 110,  # sustain
    23: 30,   # release
}

# Mixer knobs: route 5 is OFF, so nothing goes through the effect. Synth 1 at
# full volume dead centre, everything else down.
MIX = {0: 5, 9: 64, 10: 64, 11: 64, 12: 64, 13: 64, 14: 64, 17: 127}

MAJOR = [0, 2, 4, 5, 7, 9, 11, 12]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out/scale.ds10song"
    root = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    bars = int(sys.argv[3]) if len(sys.argv) > 3 else 4

    p = bytearray(open(INIT, "rb").read()[16:])

    struct.pack_into("<I", p, 4, 120)        # tempo
    struct.pack_into("<H", p, 0x0A, 50)      # swing: straight
    p[0x14:0x24] = b"SCALE\0" + b"\0" * 10

    # One bar of pattern 0, repeated.
    for i in range(100):
        struct.pack_into("<H", p, 0x24 + 2 * i, 0 if i < bars else 0xFFFF)

    for i in range(16):
        b = PAT + i * PSIZE
        struct.pack_into("<I", p, b, STEPS)
        for t in range(2):
            for k, v in ((k, v) for k, v in KNOBS.items()):
                p[b + 4 + t * 31 + k] = v & 0xFF
        # Every step of every track a rest, then fill synth 1 in.
        for t in range(6):
            for s in range(STEPS):
                p[b + 0x42 + t * 64 + s * 4:b + 0x42 + t * 64 + s * 4 + 4] = \
                    bytes([0, NO_NOTE, 127, 64])
        # Lanes off: no recorded value, no target knob.
        for t in range(2):
            for s in range(STEPS):
                o = b + 0x1C2 + t * 64 + s * 4
                p[o:o + 4] = bytes([0xFF, 0xFF, 0xFF, 0xFF])
        for k in range(25):
            p[b + 0x242 + k] = MIX.get(k, 0) & 0xFF

    # Pattern 0 only: the scale up and back down, one note per step, full gate.
    b = PAT
    notes = [root + n for n in MAJOR] + [root + n for n in reversed(MAJOR)]
    for s, n in enumerate(notes[:STEPS]):
        o = b + 0x42 + 0 * 64 + s * 4
        p[o:o + 4] = bytes([4, n & 0xFF, 127, 64])

    blob = hashlib.md5(bytes(p)).digest() + bytes(p)
    open(out, "wb").write(blob)
    print("%s: %d bytes, root %d, %d bars, notes %s" %
          (out, len(blob), root, bars, notes[:STEPS]))


main()
