#!/usr/bin/env python3
"""mktables - turn data/tables.bin into readable C.

    python3 tools/mktables.py data/tables.bin src/core/tables_data.c

The engine's tables are ROM-derived and were read from a file at start-up,
which means every build needs a filesystem. The audio worklet has none, so
they are compiled in instead.

Each one comes out as its own named array of its own type, with the ROM
address it was taken from, rather than as one opaque blob - the point of
decompiling them is that they can be read. They are what the engine indexes
directly: one fixed set of ROM constants, named, rather than a struct of
pointers threaded through every call.

Two files come out, and the header is where the documentation goes, because
that is what a reader of the engine has open.
"""
import struct
import sys

# Order and width must match TABLES in tools/oracle.py and the enum in
# src/core/tables.c. The type is the one the view is declared with, the
# address is where the table lives in the ROM's ARM9 image, and the text says
# what an entry actually is - a table of bare numbers is not decompiled.
TABLES = [
    ("env_rate", "uint32_t", 4, "t.env_rate", "0x0207F1E0", """\
Envelope rate, indexed by an ATTACK, DECAY or RELEASE knob 0..127.

The value is what the envelope adds to its 24-bit accumulator each frame, so
it falls as the knob rises: entry 0 is 0xFFFFFF, a whole stage in one frame,
and the last entries take minutes. Sustain uses the same table for its
approach rate."""),
    ("lfo_bpm", "uint32_t", 4, "t.lfo_bpm", "0x0207FDC4", """\
LFO phase increment for a tempo-synced rate, indexed by the note division.

Sixteen divisions. The voice is told the BPM by the sequencer's opcode 4
broadcast and scales this by it; a voice that never hears that broadcast runs
every synced LFO at 120."""),
    ("lfo_free", "uint32_t", 4, "t.lfo_free", "0x0207FFC4", """\
LFO phase increment for a free-running rate, indexed by MG FREQ 0..127.

Phase is 32-bit and wraps, so the increment is the frequency: entry n gives
n's rate directly with no BPM involved. The effect's LFO shares this table."""),
    ("porta", "uint32_t", 4, "t.porta", "0x0207F344", """\
Portamento coefficient, indexed by the PORTA knob 0..127.

Each frame the current pitch moves this fraction of the way to the target, so
entry 0 arrives immediately and the top entries glide for seconds."""),
    ("peak", "uint32_t", 4, "t.peak", "0x020805C4", """\
Filter resonance, indexed by the PEAK knob 0..127.

Feeds the filter's feedback term. The top of the range self-oscillates, which
is why the last entries climb steeply."""),
    ("pitch", "int32_t", 4, "t.pitch", "0x0207E950", """\
Oscillator frequency ratio, indexed by semitone.

Successive entries are in the ratio 1.05946, which is 2^(1/12), so this is an
equal-tempered scale in 20.12 fixed point. It holds for the first 119 steps
and then saturates."""),
    ("depth", "int32_t", 4, "t.depth", "0x0207EB38", """\
Bipolar modulation depth, indexed by a modulation-intensity knob 0..127.

Bipolar: the middle of the range is no modulation and the two halves are
opposite signs, which is what lets one knob invert an envelope or an LFO."""),
    ("mod_a", "int16_t", 2, "t.mod_a", "0x0207ED38", """\
First of a pair of modulation coefficients, indexed by knob value 0..127.

mod_a and mod_b are used together by the knob handler when it routes a
modulation source to a destination."""),
    ("mod_b", "int16_t", 2, "t.mod_b", "0x0207EE38", """\
The other half of that pair. See mod_a."""),
    ("coef", "int32_t", 4, "t.coef", "0x020807C4", """\
Filter cutoff coefficient, indexed by the CUTOFF knob.

Also 2^(1/12) per step while the values are small, then it saturates: entry
150 is 2147391259, just under 0x7FFFFFFF. That saturation is why the count is
151 rather than 128 - the published figure, which this confirms."""),
    ("fx_lfo_phase", "int32_t", 4, "fx.lfo_phase_off", "0x0207F044", """\
Starting LFO phase offset, one per effect type.

Three entries, for the three effects: delay, flanger and chorus."""),
    ("fx_flanger_fb", "int16_t", 2, "fx.flanger_fb", "0x0207EF44", """\
Flanger feedback, indexed by the FEEDBACK knob 0..127."""),
    ("fx_depth", "int16_t", 2, "fx.depth", "0x0207F0C4", """\
Effect modulation depth, indexed by the DEPTH knob 0..127.

How far the effect's LFO sweeps its delay line."""),
    ("fx_delay_div", "uint32_t", 4, "fx.delay_div", "0x0207F144", """\
Tempo-synced delay times, indexed by note division.

Twelve divisions. The delay length is the BPM scaled by one of these."""),
    ("fx_lfo_rate", "uint32_t", 4, "fx.lfo_rate", "0x0207FFC4", """\
The effect LFO's free-running rate, indexed by its knob 0..127.

The same address as the synth's lfo_free: one table, two users."""),
    ("drum_pitch", "uint32_t", 4, "drum_pitch", "0x0207ED38", """\
Drum playback rate, indexed by note - root + 64.

A drum is not synthesised per note: its patch is rendered once into a
one-second buffer at note 60 and the voice resamples that buffer at this
rate. Entry 64 is exactly 1024, unity, so the root note plays back
untouched."""),
    ("sincos", "int16_t", 2, "sincos", "0x0207A3D8", """\
The NitroSDK sine and cosine table, interleaved, 0x1000 of each.

Entries are 4.12, so 4096 is one. A widget's angle is 16-bit units of a turn
and this has 0x1000 entries, so the index is angle >> 4: sine at [idx*2] and
cosine at [idx*2+1]. Every rot/scale sprite's affine matrix is built from a
pair out of here - 0x02022ba4 reads them, 0x02011fc4 lays them out as
{cos, sin, -sin, cos} and 0x02011fd0 applies the scale.

It is exactly round(sin(2*pi*i/0x1000) * 4096) with no rounding quirks, and
cos is sin a quarter turn along, both checked against the ROM. It is stored
whole anyway, so the indexing here reads the way the ARM does."""),
]

# What follows the tables in the file, all of it 32-bit.
TRAILING = [
    ("voice_init", "int32_t", "voice_init", """\
The state a voice's constructor leaves behind, field by field.

None of it is zero, and starting from a memset does not reproduce the
original - which is why this is captured rather than assumed. Reloading it is
also what makes a pattern change deterministic, because the knob handler's
side effects depend on what was there before."""),
    ("mixer_init", "int32_t", "mixer_init", "The same for the mixer."),
    ("fx_init", "int32_t", "fx_init", "And for the effect."),
    ("knob_min", "int32_t", "knob_min", """\
Each synth knob's minimum, from the knob descriptors."""),
    ("knob_range", "int32_t", "knob_range", """\
And its range. An automation lane uses the pair as offset and scale, so a
recorded 0..127 lane sweeps exactly that knob's range."""),
]

# The synth's 31 knobs, in order, so the two per-knob tables can say which
# entry belongs to which control.
# Every array gets this, so the engine's use of one is greppable.
PREFIX = "ds10_tbl_"

KNOBS = [
    "OCTAVE", "PITCH EG INT", "PORTA", "PITCH IN INT", "VCO1 P.IN INT",
    "VCO2 P.IN INT", "VCO1 WAVE", "VCO2 WAVE", "VCO2 PITCH", "BALANCE",
    "VCO SYNC", "CUTOFF", "PEAK", "FILTER TYPE", "VCF EG INT",
    "CUTOFF IN INT", "VCA EG", "VCA IN INT", "DRIVE", "LEVEL", "ATTACK",
    "DECAY", "SUSTAIN", "RELEASE", "MG FREQ", "MG BPM", "PITCH IN",
    "VCO1 P.IN", "VCO2 P.IN", "CUTOFF IN", "VCA IN",
]


def read(blob):
    """(name -> list of values), following the file's own layout."""
    if blob[:4] != b"TBL1":
        raise SystemExit("not a table image")
    n = struct.unpack_from("<I", blob, 4)[0]
    if n != len(TABLES):
        raise SystemExit("file has %d tables, this knows %d" % (n, len(TABLES)))
    out = {}
    off = 8
    for name, ctype, wide, _view, _addr, _doc in TABLES:
        cnt = struct.unpack_from("<I", blob, off)[0]
        off += 4
        fmt = {("uint32_t", 4): "<%dI", ("int32_t", 4): "<%di",
               ("int16_t", 2): "<%dh"}[(ctype, wide)] % cnt
        out[name] = list(struct.unpack_from(fmt, blob, off))
        off += cnt * wide
    for name, _ctype, _view, _doc in TRAILING[:3]:
        if name == "mixer_init":
            nm, nf = struct.unpack_from("<II", blob, off)
            off += 8
            out["mixer_init"] = list(struct.unpack_from("<%di" % nm, blob, off))
            off += nm * 4
            out["fx_init"] = list(struct.unpack_from("<%di" % nf, blob, off))
            off += nf * 4
        elif name == "fx_init":
            pass                      # taken with mixer_init above
        else:
            cnt = struct.unpack_from("<I", blob, off)[0]
            off += 4
            out[name] = list(struct.unpack_from("<%di" % cnt, blob, off))
            off += cnt * 4
    out["knob_min"] = list(struct.unpack_from("<31i", blob, off))
    out["knob_range"] = list(struct.unpack_from("<31i", blob, off + 31 * 4))
    return out


def emit_doc(f, head, doc):
    f.write("/* %s\n *\n" % head)
    for line in doc.rstrip().split("\n"):
        f.write(" *%s\n" % ((" " + line).rstrip()))
    f.write(" */\n")


def emit(f, name, ctype, values, head, labels=None):
    f.write("/* %s */\n" % head)
    f.write("const %s %s[%d] = {\n" % (ctype, PREFIX + name, len(values)))
    if labels:
        w = max(len("%d" % v) for v in values)
        for i, v in enumerate(values):
            f.write("    %*d,   /* %2d %s */\n" % (w, v, i, labels[i]))
    else:
        per = 8 if ctype == "int16_t" else 6
        for i in range(0, len(values), per):
            f.write("    " + " ".join("%d," % v for v in values[i:i + per])
                    + "\n")
    f.write("};\n\n")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "data/tables.bin"
    out = sys.argv[2] if len(sys.argv) > 2 else "src/core/tables_data.c"
    hdr = out[:-2] + ".h"
    vals = read(open(src, "rb").read())
    every = ([(n, c, "%s, %d entries." % (a, len(vals[n])), d,
               KNOBS if n.startswith("knob_") else None)
              for n, c, _w, _v, a, d in TABLES]
             + [(n, c, "%d entries." % len(vals[n]), d,
                 KNOBS if n.startswith("knob_") else None)
                for n, c, _v, d in TRAILING])

    with open(hdr, "w") as f:
        f.write("/* The engine's tables, lifted out of the ROM.\n"
                " *\n"
                " * Generated by tools/mktables.py from %s - do not edit.\n"
                " *\n"
                " * These are one fixed set of constants, so the engine reads\n"
                " * them by name rather than being handed them. What varies\n"
                " * between runs - a voice's state, a pattern's knobs - is\n"
                " * still passed in.\n"
                " */\n" % src)
        f.write("#ifndef DS10_TABLES_DATA_H\n#define DS10_TABLES_DATA_H\n\n")
        f.write("#include <stdint.h>\n\n")
        for name, ctype, head, doc, _labels in every:
            emit_doc(f, head, doc)
            f.write("extern const %s %s[%d];\n\n"
                    % (ctype, PREFIX + name, len(vals[name])))
        f.write("#endif /* DS10_TABLES_DATA_H */\n")

    with open(out, "w") as f:
        f.write("/* The engine's tables, lifted out of the ROM.\n"
                " *\n"
                " * Generated by tools/mktables.py from %s - do not edit.\n"
                " * What each one means is in tables_data.h.\n"
                " */\n" % src)
        f.write('#include "tables_data.h"\n\n')
        for name, ctype, head, _doc, labels in every:
            emit(f, name, ctype, vals[name], head, labels)
    print("%s and %s: %d tables"
          % (out, hdr, len(TABLES) + len(TRAILING)))


if __name__ == "__main__":
    main()
