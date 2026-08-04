#!/usr/bin/env python3
"""songtext - a DS-10 song or instrument as editable text, and back again.

Subcommands:
  dump <song> [out.txt]         a .ds10song -> text
  load <song.txt> [out]         text -> a .ds10song, MD5 header and all
  check <song>...               dump, load, and compare with the original

Sections in brackets - `[song]`, `[pattern N]`, `[raw]` - then lines of a keyword
and its tokens. `#` starts a comment at the start of a line or after whitespace,
so `C#2` is a note; blank lines and indentation are ignored; a `|` in a step row
is a bar mark the parser drops. Anything at its default is left out - see
DEFAULTS below - so what a file shows is what the song actually sets.

A track is up to four rows, `note`, `gate`, `vel` and `pan`, the same four pages
the sequencer's sub-mode button steps through, and they share one column width so
a step is in the same column everywhere. In the note row `-` carries the previous
note on, which implies a `tie` on the step before it - that is how a held note is
written, and why a run of `x` on a drum track (gate `retrig`) is three hits while
`x - -` would be one held sample.

Exactness. The dumper writes the named fields, loads its own output back, and
appends a `raw <offset> <hex>` line for every byte that did not survive. So a
field nobody has named yet still round-trips, and `check` proving a song is
byte-identical proves the whole payload, not the parts I could name. In `clean`
mode (dump --clean) one thing is dropped: the gate, velocity and pan of a step
that has no note. Those three bytes are dead - the sequencer only reads them for
a step whose note byte is not 0xFF - and they hold whatever the editor last left
there, 129 distinct combinations across the 109 songs in this repo. The song
plays the same; its MD5 changes.

DEFAULTS, measured over data/saves and the ROM's demos (109 songs, 1744
patterns, 165888 steps) - a field is written only when it differs:

  swing 50            straight
  steps 16            1738 of 1744 patterns
  velocity 127        96% of sounding steps
  pan 63              70% of sounding steps
  gate tie / retrig   what songs contain, not what the interface starts at: 79%
                      of sounding synth steps are ties, so a synth line with no
                      gate row is legato and a staccato step is the one that
                      says so. `-` takes the default too, so `C3 - -` is one
                      note held across three steps
  patch knobs         the ROM's own INIT patch, ds10synth00.dat (mean 10 of 31
                      knobs differ, so a patch line is short)
  lanes               75% of the 6912 lane slots have no automation at all
  interp smt / hld    the automation path: smt on a synth track (3158 of its 3488
                      records), hld on a drum one, which has no lanes at all
  lane shown=         the editor record's own copy of a lane's target, written
                      only when it differs from the id the lane array carries -
                      443 of 7176 slots, and playback reads the array
  modulation          82% of the 5 mod slots per patch are source `none`
  names               most of the six name fields in a pattern are empty

Note numbers are MIDI with 60 = C4, which is also the note a drum buffer is
rendered at, so a drum hit at unity speed is written `x`.
"""
import hashlib
import os
import struct
import sys

# ---------------------------------------------------------------- layout

PATTERNS, PAT_OFF, PAT_SIZE = 16, 0xEC, 676
CHAIN, TRACKS, STEPS, LANES = 100, 6, 16, 2
NO_PATTERN, NO_NOTE = 0xFFFF, 0xFF
KNOBS, MIX_KNOBS, DRUMS = 31, 25, 4

P_STEPS, P_KNOBS, P_STEP, P_LANE = 0x000, 0x004, 0x042, 0x1C2
P_MIX, P_PARAMS = 0x242, 0x25B
DRUM_KNOBS, DRUM_EXTRA, DRUM_EXTRA_N = 0x2B2C, 0x2BA8, 9
NAME_NOW, NAME_PAT, NAME_SIZE = 0x2BCC, 0x2BFC, 8   # 6 chars, then two pad

# The PLUS-only tail, which is the 31_SONG page's two per-bar lanes. `MUTE` is a
# six-bit track mask per bar, one array per deck, packed five to a word - the
# accessors are 0x0206BE08/0x0206BE64 for deck A and 0x0206BEAC/0x0206BF04 for
# deck B. `TRS` is a signed transpose per bar, 0x0206BF4C/0x0206BF5C. Both are
# indexed by bar 0..99; a mask array holds 105 slots and the transpose array is
# cut off by the end of the payload at 52, so its last 48 bars never reach a file.
MUTE_A, MUTE_B, MUTE_SLOTS = 0x2EFC, 0x2F50, 105
TRS_WORD, TRS, TRS_N = 0x2FA4, 0x2FA8, 52

PAYLOADS = (11212, 11260, 12028, 12252)

# ---------------------------------------------------------------- knobs

# name, signed, enum. Order is knobs 0..30, i.e. patch bytes 0x20..0x3E, and the
# signed column is knob_min < 0 from the synth's own knob descriptors.
WAVES = ["tri", "saw", "square", "noise"]
FILTERS = ["lp", "bp", "hp"]
ONOFF = ["off", "on"]
SOURCES = ["none", "sine", "saw", "square", "sh", "eg", "vco2"]

KNOB = [
    ("octave", 1, None), ("pitcheg", 1, None), ("porta", 0, None),
    ("pitchint", 1, None), ("vco1int", 1, None), ("vco2int", 1, None),
    ("wave1", 0, WAVES), ("wave2", 0, WAVES), ("vco2pitch", 1, None),
    ("balance", 0, None), ("sync", 0, ONOFF), ("cutoff", 0, None),
    ("peak", 0, None), ("filter", 0, FILTERS), ("cutoffeg", 1, None),
    ("cutoffint", 1, None), ("vcaeg", 0, ONOFF), ("vcaint", 1, None),
    ("drive", 0, None), ("level", 0, None), ("attack", 0, None),
    ("decay", 0, None), ("sustain", 0, None), ("release", 0, None),
    ("mgfreq", 0, None), ("mgsync", 0, ONOFF), ("pitchsrc", 0, SOURCES),
    ("vco1src", 0, SOURCES), ("vco2src", 0, SOURCES),
    ("cutoffsrc", 0, SOURCES), ("vcasrc", 0, SOURCES),
]
KNOB_BY_NAME = {n: i for i, (n, _, _) in enumerate(KNOB)}

# A modulation slot: destination, its intensity knob, its source knob. Written as
# one `mod` token group only when the source is not `none`.
MODS = [("pitch", 3, 26), ("vco1p", 4, 27), ("vco2p", 5, 28),
        ("cutoff", 15, 29), ("vca", 17, 30)]
MOD_INT = {k for _, k, _ in MODS}
MOD_SRC = {k for _, _, k in MODS}
MOD_BY_INT = {k: s for _, k, s in MODS}

# The ROM's INIT patch, ds10synth00.dat bytes 0x20..0x3E - the game's own idea of
# a blank patch, and this format's default.
INIT_PATCH = bytes([0, 0, 0, 0, 0, 0, 1, 1, 3, 0, 0, 127, 0, 0, 0, 0, 0, 0, 0,
                    105, 0, 66, 127, 0, 19, 0, 0, 0, 0, 0, 0])

# ---------------------------------------------------------------- mixer

# Mixer knobs, from the dispatch at 0x020736C0: 0 route, 1..8 and 15..16 the
# effect, 9..14 pan, 17..22 volume, 23..24 nothing.
ROUTES = ["syn1", "syn2", "syn1+syn2", "drums", "all", "off"]
FX_TYPES = ["delay", "chorus", "flanger"]
FX = {2: ("time", 0), 3: ("ratio", 1), 4: ("rate", 0), 5: ("depth", 1),
      6: ("sync", 0), 7: ("fb", 1), 8: ("wet", 0), 15: ("k15", 0),
      16: ("k16", 0)}
MIX_PAN, MIX_VOL = 9, 17

# The INIT template's mix, ds10init.dat pattern 0 at +0x242.
INIT_MIX = bytes([0, 0, 62, 0, 0, 0, 1, 0, 0, 65, 62, 61, 63, 65, 73, 0, 0,
                  127, 127, 127, 127, 127, 127, 0, 0])

# ---------------------------------------------------------------- steps

# The six gates the interface offers, plus the 0xFF mode it has no button for.
# The percentages are the interface's own; the sub-tick counts a straight step of
# 12 turns them into are 0, 3, 6, 9, 11 and 13 - so 100% stops a sub-tick short
# of the step and a tie runs a sub-tick past it, which is what lets an identical
# next step hold the note instead of restarting it.
GATE_NAME = {0xFF: "hold", 6: "retrig", 5: "tie", 4: "100%",
             3: "75%", 2: "50%", 1: "25%", 0: "off"}
GATE_VAL = {v: k for k, v in GATE_NAME.items()}
# What a `.` in a gate row means, and what a note cell with no gate cell compiles
# to: `tie` on a synth track and `retrig` on a drum one. Neither is the interface's
# neutral setting - both are what songs actually contain. 79% of the sounding synth
# steps here are ties, and making that the default drops 7525 of the corpus's 17546
# explicit gate cells and 1760 of its 3477 gate rows.
GATE_DEFAULT = (5, 6)        # tie on a synth track, retrig on a drum track
DS10_TIE = 5                 # what a `-` in the note row implies on the step
                             # before it, and the only gate that holds a note
VEL_DEFAULT, PAN_DEFAULT = 127, 63
REST_FILLER = (3, 127, 64)   # what ds10init.dat leaves on a step with no note

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
DRUM_ROOT = 60               # unity playback speed for a drum buffer

TRACK_NAME = ["syn1", "syn2", "drum1", "drum2", "drum3", "drum4"]
TRACK_BY_NAME = {n: i for i, n in enumerate(TRACK_NAME)}


def note_name(v):
    return "%s%d" % (NOTES[v % 12], v // 12 - 1)


def note_value(s):
    for i, n in enumerate(NOTES):
        if s.upper().startswith(n) and s[len(n):].lstrip("-").isdigit():
            if len(n) == 1 and len(s) > 1 and s[1] == "#":
                continue
            return (int(s[len(n):]) + 1) * 12 + i
    raise ValueError("not a note: %r" % s)


def knob_text(i, v):
    name, signed, enum = KNOB[i]
    if enum and 0 <= v < len(enum):
        return "%s=%s" % (name, enum[v])
    if signed and v > 127:
        v -= 256
    return "%s=%d" % (name, v)


def knob_value(i, s):
    name, signed, enum = KNOB[i]
    if enum and s in enum:
        return enum.index(s)
    return int(s) & 0xFF


# ---------------------------------------------------------------- text out

def quote(raw):
    """A name field as a quoted string, with the game's padding kept."""
    out = ""
    for c in raw:
        if c == 0x22 or c == 0x5C:
            out += "\\" + chr(c)
        elif 0x20 <= c < 0x7F:
            out += chr(c)
        elif c == 0:
            out += "\\0"
        elif c == 8:
            out += "\\b"        # the pad byte most factory names use
        else:
            out += "\\x%02x" % c
    return '"%s"' % out


def unquote(s):
    assert s[0] == '"' and s[-1] == '"', s
    out, i, s = bytearray(), 1, s[:-1]
    while i < len(s):
        c = s[i]
        if c == "\\":
            n = s[i + 1]
            if n == "0":
                out.append(0)
            elif n == "b":
                out.append(8)
            elif n == "x":
                out.append(int(s[i + 2:i + 4], 16))
                i += 2
            else:
                out.append(ord(n))
            i += 2
        else:
            out.append(ord(c))
            i += 1
    return bytes(out)


def strip_name(raw):
    """Only trailing NULs come off. The game pads a name field with 0x08 as
    often as with NUL - `XPAD\\b\\b` and `HIHAT\\0` are both real - and one
    factory patch is `CPU\\0 1`, an embedded NUL then a space. So a name is six
    raw bytes, not a C string (recon.md section 4), and `\\b` in a quoted name is
    the game's blank glyph, not padding this format invented."""
    return raw.rstrip(b"\0")


def patch_text(knobs, default=INIT_PATCH):
    """A patch as `cutoff=25 peak=127 filter=lp`, defaults left out, with the
    modulation routing as its own group and only when something is routed."""
    out = []
    for i in range(KNOBS):
        if i in MOD_SRC:
            continue
        if i in MOD_INT and knobs[MOD_BY_INT[i]] != 0:
            continue            # said by the mod group instead
        if knobs[i] != default[i]:
            out.append(knob_text(i, knobs[i]))
    mods = []
    for dest, kint, ksrc in MODS:
        src, amount = knobs[ksrc], knobs[kint]
        if src == 0:
            continue            # not patched; any leftover intensity is above
        tok = "%s=%s" % (dest, SOURCES[src] if src < len(SOURCES) else src)
        if amount != 0:
            tok += ":%+d" % (amount - 256 if amount > 127 else amount)
        mods.append(tok)
    return " ".join(out), " ".join(mods)


def parse_patch(tokens, knobs):
    for t in tokens:
        name, _, val = t.partition("=")
        knobs[KNOB_BY_NAME[name]] = knob_value(KNOB_BY_NAME[name], val)


def parse_mods(tokens, knobs):
    byname = {d: (ki, ks) for d, ki, ks in MODS}
    for t in tokens:
        dest, _, rest = t.partition("=")
        src, _, amount = rest.partition(":")
        kint, ksrc = byname[dest]
        knobs[ksrc] = SOURCES.index(src) if src in SOURCES else int(src)
        knobs[kint] = int(amount) & 0xFF if amount else 0


# A drum's nine bytes are a route and the eight effect parameters, pushed in as
# mixer knobs 0..8 - the same ids and the same names the `fx` line uses. Written
# only where a byte is not zero, and the type as its word.
def drumfx_text(x):
    out = []
    if x[0]:
        out.append("route=%s" % (ROUTES[x[0]] if x[0] < len(ROUTES) else x[0]))
    for k in sorted(FX):
        if k > 8 or not x[k]:
            continue
        name, signed = FX[k]
        v = x[k] - 256 if signed and x[k] > 127 else x[k]
        out.append("%s=%d" % (name, v))
    if not out:
        return ""
    kind = FX_TYPES[x[1]] if x[1] < len(FX_TYPES) else str(x[1])
    return "%s %s" % (kind, " ".join(out)) if out else kind


def parse_drumfx(tokens, x):
    """`<type> route=… time=… …` back into the nine bytes."""
    if not tokens:
        return
    x[1] = (FX_TYPES.index(tokens[0]) if tokens[0] in FX_TYPES
            else int(tokens[0]) & 0xFF)
    byname = {n: k for k, (n, _) in FX.items() if k <= 8}
    for t in tokens[1:]:
        name, _, val = t.partition("=")
        if name == "route":
            x[0] = (ROUTES.index(val) if val in ROUTES else int(val)) & 0xFF
        elif name in byname:
            x[byname[name]] = int(val) & 0xFF


def mix_text(mix, default=INIT_MIX):
    """The mixer's 25 knobs as `mix` and `fx` lines, defaults left out."""
    out, head = [], []
    if mix[0] != default[0]:
        head.append("route=%s"
                    % (ROUTES[mix[0]] if mix[0] < len(ROUTES) else mix[0]))
    for name, base in (("vol", MIX_VOL), ("pan", MIX_PAN)):
        if mix[base:base + 6] != default[base:base + 6]:
            head.append("%s=%s" % (name, ",".join(str(v) for v in
                                                  mix[base:base + 6])))
    if head:
        out.append("mix " + " ".join(head))
    fx = []
    for k in sorted(FX):
        name, signed = FX[k]
        v = mix[k]
        if v == default[k]:
            continue
        if signed and v > 127:
            v -= 256
        fx.append("%s=%d" % (name, v))
    if fx or mix[1] != default[1]:
        kind = FX_TYPES[mix[1]] if mix[1] < len(FX_TYPES) else str(mix[1])
        out.append("fx %s %s" % (kind, " ".join(fx)))
    return out


def parse_mix(tokens, mix):
    for t in tokens:
        name, _, val = t.partition("=")
        if name == "route":
            mix[0] = ROUTES.index(val) if val in ROUTES else int(val)
        elif name in ("vol", "pan"):
            base = MIX_VOL if name == "vol" else MIX_PAN
            for i, v in enumerate(val.split(",")):
                mix[base + i] = int(v) & 0xFF


def parse_fx(tokens, mix):
    mix[1] = FX_TYPES.index(tokens[0]) if tokens[0] in FX_TYPES \
        else int(tokens[0])
    byname = {n: k for k, (n, _) in FX.items()}
    for t in tokens[1:]:
        name, _, val = t.partition("=")
        mix[byname[name]] = int(val) & 0xFF


GRID_KINDS = ("note", "gate", "vel", "pan")


def grid_text(pat, track):
    """One track's 16 steps as up to four rows - the same four the sequencer's
    own sub-mode button steps through. A rest is `.`, a step whose four bytes
    repeat the one before it is `-`, a drum hit at the root note is `x`, and a
    `.` in the gate, vel or pan row means that step is at the default.

    Keeping gate, velocity and pan off the note cell is what lets every row use
    one column width, so a step sits in the same column in every row of the
    pattern and the note starts line up."""
    drum = track >= 2
    dflt = GATE_DEFAULT[1 if drum else 0]
    rows = {k: [] for k in GRID_KINDS}
    for s in range(STEPS):
        o = P_STEP + track * 64 + s * 4
        gate, note, vel, pan = pat[o], pat[o + 1], pat[o + 2], pat[o + 3]
        if note == NO_NOTE:
            for k in GRID_KINDS:
                rows[k].append(".")
            continue
        held = s and note == pat[o - 3] and pat[o - 4] == DS10_TIE
        if held:
            # The note carries on, so the step before it has to be a tie and
            # saying so twice would be noise: `-` is the tie.
            rows["note"].append("-")
            rows["gate"][s - 1] = "."
        else:
            rows["note"].append("x" if drum and note == DRUM_ROOT
                                else note_name(note))
        rows["gate"].append("." if gate == dflt
                            else GATE_NAME.get(gate, str(gate)))
        rows["vel"].append("." if vel == VEL_DEFAULT else str(vel))
        rows["pan"].append("." if pan == PAN_DEFAULT else str(pan))
    # A run's last step keeps its own gate: it is what ends the note.
    return rows


def grid_rows(rows):
    """Which of the four rows carry anything worth printing."""
    if all(c == "." for c in rows["note"]):
        return []
    return [k for k in GRID_KINDS
            if k == "note" or any(c not in (".", "-") for c in rows[k])]


def parse_grid(kind, tokens, pat, track):
    """One `note`, `gate`, `vel` or `pan` row. The note row has to be read
    first - it is what decides which steps exist - and the other three then
    override a byte of a step the note row already placed."""
    drum = track >= 2
    cells = [t for t in tokens if t != "|"][:STEPS]
    for s, t in enumerate(cells):
        o = P_STEP + track * 64 + s * 4
        if kind == "note":
            if t == "." or (t == "-" and s == 0):
                pat[o], pat[o + 1], pat[o + 2], pat[o + 3] = \
                    REST_FILLER[0], NO_NOTE, REST_FILLER[1], REST_FILLER[2]
            elif t == "-":
                # Carry the note on, and tie the step before it so it does.
                pat[o - 4] = DS10_TIE
                pat[o] = GATE_DEFAULT[1 if drum else 0]
                pat[o + 1] = pat[o - 3]
                pat[o + 2], pat[o + 3] = VEL_DEFAULT, PAN_DEFAULT
            else:
                pat[o] = GATE_DEFAULT[1 if drum else 0]
                pat[o + 1] = DRUM_ROOT if t == "x" else note_value(t)
                pat[o + 2], pat[o + 3] = VEL_DEFAULT, PAN_DEFAULT
            continue
        if t in (".", "-"):
            continue
        if kind == "gate":
            pat[o] = GATE_VAL[t] if t in GATE_VAL else int(t) & 0xFF
        elif kind == "vel":
            pat[o + 2] = int(t) & 0xFF
        else:
            pat[o + 3] = int(t) & 0xFF


# The 12-byte per-track record at pattern +0x25C, and the nine bytes of it that
# the game reads. Its accessor (0x0206B0C0) takes a field 0..9 and reads record
# byte `field`, so there is one for byte 8 as well - but nothing calls it with 8,
# and 10 and 11 have no accessor at all; all three are zero in every record here.
#
#   0  piano-roll scroll                          0..84
#   1  keyboard octave, the six dots over the keys 0..5, clamped at 0x0204D3E8
#   2  lane 1 knob id, a copy of the lane array's; playback reads the array
#   3  lane 2 knob id
#   4  KAOSS pad scale, index into the 31 names at 0x02084E54
#   5  KAOSS pad key, 0 = C .. 11 = B, the names at 0x02084B70
#   6  automation path: 1 = HLD, 2 = SMT. This is the one thing here the
#      sequencer reads - 0x0206DD4C puts it in the automation object's `enable`
#   7  KAOSS pad slot, the 1/2/3 buttons
#   9  the pattern page's LOCK: 1 = engaged, 0xFF = neither LOCK nor COPY
#
# docs/songtext.md has how each of these was measured. Each is written only when
# it differs from the default below, which is the value a fresh pattern carries.
# Bytes 2, 3 and 6 are not here: they are automation, not viewport, so they live
# with the lanes - `<track> auto interp=` for byte 6 and `shown=` on a lane line
# for that lane's copy of its target.
VIEW_FIELDS = (("scroll", 0, 0), ("oct", 1, 0), ("scale", 4, 0), ("key", 5, 0),
               ("slot", 7, 0), ("lock", 9, 0))

# Record byte 6, the automation path, and what a track carries when nothing says
# otherwise: `smt` on a synth track and `hld` on a drum one, which have no lanes at
# all. 3158 of the 3488 synth records here are smt and every drum record is hld.
AUTO_BYTE = 6
AUTO_DEFAULT = (2, 1)

# The pad's scales, the 31 names of 16 bytes at 0x02084E54, and its keys, the
# twelve of 8 bytes at 0x02084B70. Spelled as the screen spells them, because
# lowercasing would collide: MBLUES and mBLUES are the major and minor blues
# scales, and so are MPENTA/mPENTA and M3RD/m3RD.
SCALE = ("CHROMA", "IONIAN", "DORIAN", "PHRYGI", "LYDIAN", "MIXLYD", "AEOLIA",
         "LOCRIA", "MBLUES", "mBLUES", "DIM", "COMDIM", "MPENTA", "mPENTA",
         "RAGA1", "RAGA2", "RAGA3", "SPANISH", "GYPSY", "ARABIA", "EGYPT",
         "HAWAII", "PELOG", "JAPAN", "RYUKYU", "WHOLE", "m3RD", "M3RD", "4TH",
         "5TH", "OCTAVE")
KEY = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# The two automation paths the pad's own buttons are labelled with, plus the 0
# that only ds10demo00 carries.
INTERP = {0: "off", 1: "hld", 2: "smt"}
INTERP_VAL = {v: k for k, v in INTERP.items()}


def mute_get(payload, base, bar):
    """One bar's six-bit mask out of the five packed into each word."""
    word = struct.unpack_from("<I", payload, base + (bar // 5) * 4)[0]
    return (word >> (bar % 5) * 6) & 0x3F


def mute_set(payload, base, bar, value):
    o = base + (bar // 5) * 4
    word = struct.unpack_from("<I", payload, o)[0]
    shift = (bar % 5) * 6
    word = (word & ~(0x3F << shift)) | ((value & 0x3F) << shift)
    struct.pack_into("<I", payload, o, word)


def mute_text(payload, base):
    """`mute a` / `mute b` - only the bars that mute something."""
    if base + MUTE_SLOTS // 5 * 4 > len(payload):
        return None
    out = ["%d=%d" % (b, mute_get(payload, base, b))
           for b in range(MUTE_SLOTS) if mute_get(payload, base, b)]
    return " ".join(out) if out else None


def trs_text(payload):
    """`trs` - only the bars that transpose."""
    if TRS + TRS_N > len(payload):
        return None
    out = ["%d=%d" % (b, v - 256 if v > 127 else v)
           for b, v in enumerate(payload[TRS:TRS + TRS_N]) if v]
    return " ".join(out) if out else None


def parse_bar_pairs(tokens):
    for t in tokens:
        bar, _, value = t.partition("=")
        yield int(bar), int(value)


def view_text(pat, track):
    """`<track> view …` - one track's editor record as named fields, or None when
    every one of them is at its default."""
    base = P_PARAMS + 1 + track * 12
    out = []
    for name, byte, dflt in VIEW_FIELDS:
        v = pat[base + byte]
        if v == dflt:
            continue
        if name == "scale":
            out.append("scale=%s" % (SCALE[v] if v < len(SCALE) else v))
        elif name == "key":
            out.append("key=%s" % (KEY[v] if v < len(KEY) else v))
        elif name == "lock":
            out.append("lock=%d" % (v - 256 if v > 127 else v))
        else:
            out.append("%s=%d" % (name, v))
    if not out:
        return None
    return "%s view %s" % (TRACK_NAME[track], " ".join(out))


def parse_view(tokens, pat, track):
    base = P_PARAMS + 1 + track * 12
    byte = {name: b for name, b, _ in VIEW_FIELDS}
    for name, b, dflt in VIEW_FIELDS:
        pat[base + b] = dflt
    for t in tokens:
        name, _, val = t.partition("=")
        if name not in byte:
            continue
        if name == "scale":
            v = SCALE.index(val) if val in SCALE else int(val)
        elif name == "key":
            v = KEY.index(val) if val in KEY else int(val)
        else:
            v = int(val)
        pat[base + byte[name]] = v & 0xFF


def auto_text(pat, track):
    """`<track> auto interp=…` - the record's byte 6, the path the automation
    processor takes for this track. Written only when it is not the default."""
    v = pat[P_PARAMS + 1 + track * 12 + AUTO_BYTE]
    if v == AUTO_DEFAULT[1 if track >= LANES else 0]:
        return None
    return "%s auto interp=%s" % (TRACK_NAME[track], INTERP.get(v, v))


def parse_auto(tokens, pat, track):
    for t in tokens:
        name, _, val = t.partition("=")
        if name != "interp":
            continue
        v = INTERP_VAL[val] if val in INTERP_VAL else int(val)
        pat[P_PARAMS + 1 + track * 12 + AUTO_BYTE] = v & 0xFF


def lane_text(pat, track, lane):
    """One automation lane: its knob, then only the steps that have a value.
    -1 means "no automation here", which is most of them.

    The knob id is stored on every step, but the machine is only ever set up from
    step 0 (0x0206DEE4, and songplay does the same), so a lane cannot retarget
    mid-pattern. 31 of the 6912 lane slots in the saves do hold a second id on
    their later steps - what the editor left when the target was changed - and
    those come back as `raw` lines rather than as syntax nobody needs.

    `shown=` is the editor record's own copy of the target, byte 2 or 3, which the
    KAOSS page displays and playback ignores. It matches the lane's id in 6533 of
    the 7176 lane slots here, so it is written only when it does not."""
    base = P_LANE + track * 64
    rbase = P_PARAMS + 1 + track * 12 + 2 + lane
    raw = [pat[base + s * 4 + lane] for s in range(STEPS)]
    knob = [pat[base + s * 4 + 2 + lane] for s in range(STEPS)]
    shown = pat[rbase]
    if (all(r == NO_NOTE for r in raw) and knob[0] == (11 + lane)
            and shown == knob[0]):
        return None
    name = KNOB[knob[0]][0] if knob[0] < KNOBS else str(knob[0])
    out = ["%s lane%d knob=%s" % (TRACK_NAME[track], lane + 1, name)]
    if shown != knob[0]:
        out.append("shown=%s" % (KNOB[shown][0] if shown < KNOBS else shown))
    for s in range(STEPS):
        if raw[s] != NO_NOTE:
            out.append("%d=%d" % (s, raw[s]))
    return " ".join(out)


def parse_lane(tokens, pat, track, lane):
    base = P_LANE + track * 64
    rbase = P_PARAMS + 1 + track * 12 + 2 + lane
    knob = 11 + lane
    shown = None
    for t in tokens:
        name, _, val = t.partition("=")
        if name in ("knob", "shown"):
            v = KNOB_BY_NAME.get(val, None)
            v = v if v is not None else int(val)
            if name == "knob":
                knob = v
            else:
                shown = v
    for s in range(STEPS):
        pat[base + s * 4 + 2 + lane] = knob
    # The record's copy follows the lane unless the line says otherwise.
    pat[rbase] = knob if shown is None else shown
    for t in tokens:
        if "=" in t and t.split("=")[0].isdigit():
            s, _, v = t.partition("=")
            pat[base + int(s) * 4 + lane] = int(v) & 0xFF


# ---------------------------------------------------------------- dump

def strip_comment(line):
    """A `#` starts a comment only at the start of a line or after whitespace,
    so `C#2` is a note and not the end of the line."""
    for i, c in enumerate(line):
        if c == "#" and (i == 0 or line[i - 1] in " \t"):
            return line[:i]
    return line


def blank(payload_len):
    """The skeleton a text file is read into: everything this format calls a
    default. Whatever a song does differently is then either a named field or a
    `raw` line."""
    p = bytearray(payload_len)
    struct.pack_into("<H", p, 0x08, 12)
    struct.pack_into("<H", p, 0x0A, 50)
    for i in range(CHAIN):
        struct.pack_into("<H", p, 0x24 + i * 2, NO_PATTERN)
    for i in range(PATTERNS):
        o = PAT_OFF + i * PAT_SIZE
        if o + PAT_SIZE > payload_len:
            break
        struct.pack_into("<I", p, o + P_STEPS, STEPS)
        for t in range(2):
            p[o + P_KNOBS + t * KNOBS:o + P_KNOBS + (t + 1) * KNOBS] = \
                INIT_PATCH
        for t in range(TRACKS):
            for s in range(STEPS):
                q = o + P_STEP + t * 64 + s * 4
                p[q], p[q + 1] = REST_FILLER[0], NO_NOTE
                p[q + 2], p[q + 3] = REST_FILLER[1], REST_FILLER[2]
        for t in range(LANES):
            for s in range(STEPS):
                q = o + P_LANE + t * 64 + s * 4
                p[q], p[q + 1] = NO_NOTE, NO_NOTE
                p[q + 2], p[q + 3] = 11, 12
        for t in range(TRACKS):
            base = o + P_PARAMS + 1 + t * 12
            p[base + AUTO_BYTE] = AUTO_DEFAULT[1 if t >= LANES else 0]
            if t < LANES:
                # A lane's own id is the default for the record's copy of it.
                p[base + 2], p[base + 3] = 11, 12
        p[o + P_MIX:o + P_MIX + MIX_KNOBS] = INIT_MIX
    if payload_len >= DRUM_KNOBS + DRUMS * KNOBS:
        for d in range(DRUMS):
            p[DRUM_KNOBS + d * KNOBS:DRUM_KNOBS + (d + 1) * KNOBS] = INIT_PATCH
    return p


def spans(a, b, skip, gap=8):
    """Byte runs where two payloads differ, ignoring offsets in `skip`. Runs
    closer together than `gap` are merged - rewriting a few bytes that already
    agree costs nothing and keeps this to one line per region instead of
    fifteen."""
    out, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i] and i not in skip:
            j = i
            run = 0
            while j < n and run <= gap:
                if a[j] != b[j] and j not in skip:
                    run = 0
                else:
                    run += 1
                j += 1
            j -= run
            out.append((i, bytes(a[i:j])))
            i = j
        else:
            i += 1
    if len(a) != len(b):
        out.append((n, bytes(a[n:])))
    return out


def dead_bytes(payload):
    """The gate, velocity and pan of every step with no note. The sequencer
    never reads them and the editor leaves whatever it likes there."""
    out = set()
    for i in range(PATTERNS):
        o = PAT_OFF + i * PAT_SIZE
        if o + PAT_SIZE > len(payload):
            break
        for t in range(TRACKS):
            for s in range(STEPS):
                q = o + P_STEP + t * 64 + s * 4
                if payload[q + 1] == NO_NOTE:
                    out |= {q, q + 2, q + 3}
    return out


def dumps(payload, clean=False, source=None):
    """A song payload (no MD5 header) as text."""
    out = []
    if source:
        out.append("# %s" % source)
    out.append("[song]")
    out.append("payload %d" % len(payload))
    ver, rev, tempo = struct.unpack_from("<HHI", payload, 0)
    f08, swing = struct.unpack_from("<HH", payload, 8)
    out.append("version %d" % ver)
    if rev:
        out.append("revision %d" % rev)
    out.append("tempo %d" % tempo)
    if swing != 50:
        out.append("swing %d" % swing)
    if f08 != 12:
        out.append("f08 %d" % f08)
    name = strip_name(payload[0x14:0x24].rstrip(b"\0"))
    if name:
        out.append("name %s" % quote(name))
    chain = struct.unpack_from("<100H", payload, 0x24)
    bars = next((i for i, v in enumerate(chain) if v == NO_PATTERN), CHAIN)
    if bars:
        out.append("chain " + " ".join(str(c) for c in chain[:bars]))
    for tag, base in (("a", MUTE_A), ("b", MUTE_B)):
        line = mute_text(payload, base)
        if line:
            out.append("mute %s %s" % (tag, line))
    line = trs_text(payload)
    if line:
        out.append("trs %s" % line)

    if len(payload) >= DRUM_KNOBS + DRUMS * KNOBS:
        for d in range(DRUMS):
            o = DRUM_KNOBS + d * KNOBS
            knobs = payload[o:o + KNOBS]
            body, mods = patch_text(knobs)
            if body:
                out.append("drum%d patch %s" % (d + 1, body))
            if mods:
                out.append("drum%d mod %s" % (d + 1, mods))
            if len(payload) >= DRUM_EXTRA + DRUMS * DRUM_EXTRA_N:
                x = payload[DRUM_EXTRA + d * DRUM_EXTRA_N:
                            DRUM_EXTRA + (d + 1) * DRUM_EXTRA_N]
                line = drumfx_text(x)
                if line:
                    out.append("drum%d fx %s" % (d + 1, line))

    for i in range(PATTERNS):
        o = PAT_OFF + i * PAT_SIZE
        if o + PAT_SIZE > len(payload):
            break
        pat = payload[o:o + PAT_SIZE]
        body = []
        steps = struct.unpack_from("<I", pat, P_STEPS)[0]
        if steps != STEPS:
            body.append("steps %d" % steps)
        if len(payload) >= NAME_PAT + (i + 1) * TRACKS * NAME_SIZE:
            for t in range(TRACKS):
                q = NAME_PAT + i * TRACKS * NAME_SIZE + t * NAME_SIZE
                nm = strip_name(payload[q:q + 6])
                if nm:
                    body.append("%s name %s" % (TRACK_NAME[t], quote(nm)))
        for t in range(2):
            knobs = pat[P_KNOBS + t * KNOBS:P_KNOBS + (t + 1) * KNOBS]
            pbody, mods = patch_text(knobs)
            if pbody:
                body.append("%s patch %s" % (TRACK_NAME[t], pbody))
            if mods:
                body.append("%s mod %s" % (TRACK_NAME[t], mods))
        grids = [(t, grid_text(pat, t)) for t in range(TRACKS)]
        width = max([len(c) for _, rows in grids for k in grid_rows(rows)
                     for c in rows[k]] + [1])
        for t, rows in grids:
            for n, kind in enumerate(grid_rows(rows)):
                body.append("%-5s %-4s %s"
                            % (TRACK_NAME[t] if n == 0 else "", kind,
                               fmt_grid(rows[kind], width)))
        for t in range(TRACKS):
            line = auto_text(pat, t)
            if line:
                body.append(line)
            if t >= LANES:
                continue
            for l in range(LANES):
                line = lane_text(pat, t, l)
                if line:
                    body.append(line)
        mix = pat[P_MIX:P_MIX + MIX_KNOBS]
        if bytes(mix) != INIT_MIX:
            body += mix_text(mix)
        for t in range(TRACKS):
            rec = view_text(pat, t)
            if rec:
                body.append(rec)
        if body:
            out.append("")
            out.append("[pattern %d]" % i)
            out += body

    text = "\n".join(out) + "\n"
    rebuilt = loads(text, _raw=False)
    skip = dead_bytes(payload) if clean else set()
    extra = spans(payload, rebuilt, skip)
    if extra:
        out.append("")
        out.append("[raw]")
        out.append("# bytes this format does not name, payload-relative")
        for off, blob in extra:
            for k in range(0, len(blob), 24):
                out.append("raw %04x %s"
                           % (off + k, blob[k:k + 24].hex(" ")))
        text = "\n".join(out) + "\n"
    return text


def fmt_grid(cells, w):
    """Bar marks every four steps, one column width for the whole pattern."""
    out = []
    for s, c in enumerate(cells):
        if s and s % 4 == 0:
            out.append("|")
        out.append(c.ljust(w) if w > 1 else c)
    return " ".join(out).rstrip()


# ---------------------------------------------------------------- load

def loads(text, _raw=True):
    """Text back to a payload."""
    lines = []
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if line:
            lines.append(line)

    length = 12252
    for line in lines:
        if line.startswith("payload "):
            length = int(line.split()[1])
    p = blank(length)
    pat, track = None, None
    for line in lines:
        if line.startswith("["):
            head = line.strip("[]").split()
            pat = int(head[1]) if head[0] == "pattern" else None
            track = None
            continue
        tok = line.split()
        key, rest = tok[0], tok[1:]
        if key == "payload":
            continue
        if key == "version":
            struct.pack_into("<H", p, 0, int(rest[0]))
        elif key == "revision":
            struct.pack_into("<H", p, 2, int(rest[0]))
        elif key == "tempo":
            struct.pack_into("<I", p, 4, int(rest[0]))
        elif key == "f08":
            struct.pack_into("<H", p, 8, int(rest[0]))
        elif key == "swing":
            struct.pack_into("<H", p, 0x0A, int(rest[0]))
        elif key == "name":
            nm = unquote(line.split(None, 1)[1])
            p[0x14:0x24] = nm.ljust(16, b"\0")[:16]
        elif key == "chain":
            for i, v in enumerate(rest):
                struct.pack_into("<H", p, 0x24 + i * 2, int(v))
        elif key == "mute":
            base = MUTE_A if rest[0] == "a" else MUTE_B
            for bar, value in parse_bar_pairs(rest[1:]):
                mute_set(p, base, bar, value)
        elif key == "trs":
            for bar, value in parse_bar_pairs(rest):
                p[TRS + bar] = value & 0xFF
        elif key == "raw":
            if _raw:
                off = int(rest[0], 16)
                blob = bytes.fromhex("".join(rest[1:]))
                p[off:off + len(blob)] = blob
        elif (pat is None and key.startswith("drum") and rest
              and rest[0] in ("patch", "mod", "fx")):
            d = int(key[4:]) - 1
            o = DRUM_KNOBS + d * KNOBS
            if rest[0] == "patch":
                knobs = bytearray(p[o:o + KNOBS])
                parse_patch(rest[1:], knobs)
                p[o:o + KNOBS] = knobs
            elif rest[0] == "mod":
                knobs = bytearray(p[o:o + KNOBS])
                parse_mods(rest[1:], knobs)
                p[o:o + KNOBS] = knobs
            else:
                at = DRUM_EXTRA + d * DRUM_EXTRA_N
                x = bytearray(p[at:at + DRUM_EXTRA_N])
                parse_drumfx(rest[1:], x)
                p[at:at + DRUM_EXTRA_N] = x
        elif pat is not None:
            track = apply_pattern_line(p, pat, key, rest, line, track)
    return p


def apply_pattern_line(p, i, key, rest, line, track):
    """`track` is the one the last `note` row named, so the `gate`, `vel` and
    `pan` rows under it can leave the label column empty."""
    o = PAT_OFF + i * PAT_SIZE
    if key in GRID_KINDS:
        if track is not None:
            pat = bytearray(p[o:o + PAT_SIZE])
            parse_grid(key, rest, pat, track)
            p[o:o + PAT_SIZE] = pat
        return track
    if key == "steps":
        struct.pack_into("<I", p, o + P_STEPS, int(rest[0]))
        return track
    if key == "mix":
        mix = bytearray(p[o + P_MIX:o + P_MIX + MIX_KNOBS])
        parse_mix(rest, mix)
        p[o + P_MIX:o + P_MIX + MIX_KNOBS] = mix
        return track
    if key == "fx":
        mix = bytearray(p[o + P_MIX:o + P_MIX + MIX_KNOBS])
        parse_fx(rest, mix)
        p[o + P_MIX:o + P_MIX + MIX_KNOBS] = mix
        return track
    if key not in TRACK_BY_NAME:
        return track
    t = TRACK_BY_NAME[key]
    what = rest[0] if rest else ""
    if what == "name":
        nm = unquote(line.split(None, 2)[2])
        q = NAME_PAT + i * TRACKS * NAME_SIZE + t * NAME_SIZE
        if q + 6 <= len(p):
            p[q:q + 6] = nm.ljust(6, b"\0")[:6]
        if i == 0 and NAME_NOW + t * NAME_SIZE + 6 <= len(p):
            q = NAME_NOW + t * NAME_SIZE
            p[q:q + 6] = nm.ljust(6, b"\0")[:6]
    elif what == "patch":
        knobs = bytearray(p[o + P_KNOBS + t * KNOBS:
                            o + P_KNOBS + (t + 1) * KNOBS])
        parse_patch(rest[1:], knobs)
        p[o + P_KNOBS + t * KNOBS:o + P_KNOBS + (t + 1) * KNOBS] = knobs
    elif what == "mod":
        knobs = bytearray(p[o + P_KNOBS + t * KNOBS:
                            o + P_KNOBS + (t + 1) * KNOBS])
        parse_mods(rest[1:], knobs)
        p[o + P_KNOBS + t * KNOBS:o + P_KNOBS + (t + 1) * KNOBS] = knobs
    elif what == "view":
        pat = bytearray(p[o:o + PAT_SIZE])
        parse_view(rest[1:], pat, t)
        p[o:o + PAT_SIZE] = pat
    elif what == "auto":
        pat = bytearray(p[o:o + PAT_SIZE])
        parse_auto(rest[1:], pat, t)
        p[o:o + PAT_SIZE] = pat
    elif what.startswith("lane"):
        pat = bytearray(p[o:o + PAT_SIZE])
        parse_lane(rest[1:], pat, t, int(what[4:]) - 1)
        p[o:o + PAT_SIZE] = pat
    elif what in GRID_KINDS:
        pat = bytearray(p[o:o + PAT_SIZE])
        parse_grid(what, rest[1:], pat, t)
        p[o:o + PAT_SIZE] = pat
    return t


# ------------------------------------------------- the global instrument bank

# A bank record is what /data/demo/ds10synthNN.dat holds: 16-byte MD5, a 6-byte
# name, ten bytes that are zero in all 24 factory patches, then knobs 0..30.
INSTR_SIZE, INSTR_NAME, INSTR_KNOBS = 63, 0x10, 0x20


def instr_text(slot, rec):
    """One bank slot, in the same shape a pattern has: a section and its lines."""
    out = ["[instrument %d]" % slot]
    knobs = rec[INSTR_KNOBS:INSTR_KNOBS + KNOBS]
    body, mods = patch_text(knobs)
    out.append("name %s" % quote(strip_name(rec[INSTR_NAME:INSTR_NAME + 6])))
    if body:
        out.append("patch %s" % body)
    if mods:
        out.append("mod %s" % mods)
    pad = rec[INSTR_NAME + 6:INSTR_KNOBS]
    if any(pad):
        out.append("pad %s" % pad.hex(" "))
    return out


def instr_blank():
    rec = bytearray(INSTR_SIZE)
    rec[INSTR_KNOBS:INSTR_KNOBS + KNOBS] = INIT_PATCH
    return rec


def instr_seal(rec):
    """Recompute the record's MD5 header, as the game's own writer does."""
    rec[:16] = hashlib.md5(bytes(rec[16:])).digest()
    return bytes(rec)


def bank_loads(text):
    """Text to {slot: 63-byte record}. Slots that are not mentioned stay
    empty - the bank is sparse, and 13 of EMPTY10's 24 slots are."""
    bank, rec = {}, None
    for raw in text.splitlines():
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            head = line.strip("[]").split()
            rec = None
            if head[0] == "instrument":
                rec = bank.setdefault(int(head[1]), instr_blank())
            continue
        if rec is None:
            continue
        tok = line.split()
        if tok[0] == "name":
            nm = unquote(line.split(None, 1)[1])
            rec[INSTR_NAME:INSTR_NAME + 6] = nm.ljust(6, b"\0")[:6]
        elif tok[0] == "pad":
            blob = bytes.fromhex("".join(tok[1:]))
            rec[INSTR_NAME + 6:INSTR_NAME + 6 + len(blob)] = blob
        elif tok[0] in ("patch", "mod"):
            knobs = bytearray(rec[INSTR_KNOBS:INSTR_KNOBS + KNOBS])
            (parse_patch if tok[0] == "patch" else parse_mods)(tok[1:], knobs)
            rec[INSTR_KNOBS:INSTR_KNOBS + KNOBS] = knobs
    return {s: instr_seal(r) for s, r in bank.items()}


# ---------------------------------------------------------------- files

def read_song(path):
    b = open(path, "rb").read()
    if len(b) - 16 not in PAYLOADS:
        raise SystemExit("%s: %d bytes is not a song file" % (path, len(b)))
    if hashlib.md5(b[16:]).digest() != b[:16]:
        raise SystemExit("%s: MD5 header does not match" % path)
    return b[16:]


def write_song(path, payload):
    with open(path, "wb") as f:
        f.write(hashlib.md5(payload).digest() + bytes(payload))


def cmd_dump(args):
    clean = "--clean" in args
    args = [a for a in args if a != "--clean"]
    if not args:
        print("usage: songtext.py dump [--clean] <song> [out.ds10song.txt]")
        return 1
    payload = read_song(args[0])
    text = dumps(payload, clean=clean, source=os.path.basename(args[0]))
    if len(args) > 1:
        open(args[1], "w").write(text)
        print("%s: %d lines" % (args[1], text.count("\n")))
    else:
        sys.stdout.write(text)
    return 0


def cmd_load(args):
    if not args:
        print("usage: songtext.py load <song.ds10song.txt> [out.ds10song]")
        return 1
    payload = loads(open(args[0]).read())
    out = args[1] if len(args) > 1 else os.path.splitext(args[0])[0]
    write_song(out, payload)
    print("%s: %d bytes" % (out, len(payload) + 16))
    return 0


def cmd_check(args):
    bad = 0
    for path in args:
        payload = read_song(path)
        for clean in (False, True):
            back = loads(dumps(payload, clean=clean))
            if clean:
                skip = dead_bytes(payload)
                diff = [i for i in range(len(payload))
                        if payload[i] != back[i] and i not in skip]
                tag = "clean"
            else:
                diff = [i for i in range(len(payload)) if payload[i] != back[i]]
                tag = "exact"
            if diff or len(back) != len(payload):
                bad += 1
                print("FAIL %-24s %s: %d bytes differ, first at %#x"
                      % (os.path.basename(path), tag, len(diff),
                         diff[0] if diff else -1))
    print("%d of %d songs round-tripped" % (len(args) - bad, len(args)))
    return 1 if bad else 0


def main():
    cmds = {"dump": cmd_dump, "load": cmd_load, "check": cmd_check}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    return cmds[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main())
