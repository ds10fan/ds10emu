# songtext — a DS-10 song as text

`tools/songtext.py` turns a song file into text and back; `tools/sav.py unpack`
does it for a whole save at once, writing `slot00.txt` … `slot17.txt` plus
`bank.txt`, and `sav.py repack` builds a save file out of that directory.

The point is that the interesting parts of a song — patches, notes, the mix —
are named and readable, and that anything sitting at a default is simply absent.
A pattern that nobody touched, an automation lane with nothing recorded in it, a
modulation slot with no source: none of them appear at all.

## Shape

Sections in brackets, then lines of a keyword and its tokens. `#` starts a
comment when it is at the start of a line or follows whitespace — so `C#2` is a
note, not a truncated line. Blank lines and indentation are ignored; a `|` inside
a step grid is a bar mark the parser drops. `[song]` holds the header and the
four drum instruments, `[pattern N]` one pattern, `[raw]` whatever the format
could not name.

```
[song]
payload 12252              # 11212, 11260, 12028 or 12252 - the four that exist
version 2                  # 1 = DS-10, 2 = PLUS, 0 or 0xFFFF = pre-release
revision 1
tempo 127
name "FLUTER"
chain 12 13 12 14 15 15 15 15        # bar -> pattern; absent means no song
drum1 patch octave=-2 cutoff=23 peak=83 decay=19 release=21
drum1 extra 0 0 63 0 62 60 0 0 0     # the nine per-drum bytes nobody has named
drum2 mod vca=eg:+63

[pattern 0]
syn1 name "XLEAD\b"
syn1 patch porta=14 wave1=square cutoff=57 peak=51 cutoffeg=14 mgfreq=77
syn1 mod pitch=sine:+4
syn1  note D4   F2   -    .    | .    G#2  -    -    | .    .    .    A3
      gate tie  .    tie  .    | .    .    .    tie  | .    .    .    .
drum1 note C2   .    .    .    | C2   .    .    .    | C2   x    x    x
      gate 100% .    .    .    | 100% .    .    .    | 100% .    .    .
      pan  .    .    .    .    | 64   .    .    .    | 64   .    .    .
syn2 lane1 knob=cutoff 0=69 1=62 2=51 3=45 4=48 5=50 6=37
mix route=syn1+syn2 vol=105,105,104,105,66,105 pan=65,62,61,63,74,56
fx delay time=14 ratio=14 rate=23 depth=62 sync=0 fb=38 wet=52
scroll 46 31 36 36 57 36             # each track's piano-roll position
steps 12                             # only when it is not 16
```

A track is up to four rows — `note`, `gate`, `vel`, `pan` — which are the four
pages the sequencer's own sub-mode button steps through. Only `note` is always
there; the other three appear when some step is not at its default. Every row of
every track in a pattern shares one column width, so a step sits in the same
column throughout and the note starts line up.

A `note` cell is a MIDI note name with 60 = C4, or `x` on a drum track for the
root note 60 — unity playback speed for a drum buffer — or `.` for no note, or
`-` to carry the previous note on. In the other three rows `.` means that step is
at the default. Gates are `off`, `25%`, `50%`, `75%`, `100%`, `tie`, `retrig` and
`hold`, which are the interface's own six plus the 0xFF mode it has no button
for; recon.md §13 has the sub-tick counts.

### A held note is `-`, and the tie is implied

```
syn1  note D5   D6   C3   -    -
      gate .    .    .    .    .
```

is D5 and D6 at the default, then one C3 lasting three steps. `-` carries the
note on, and the step before a `-` has to be a `tie` for that to happen — so the
format writes the tie as the `-` and leaves the gate row alone. The last step of
a run keeps its own gate, because that is what ends the note: here it is the
default 25%, so C3 is released three sub-ticks into its last step.

This is also why a repeat is not always a `-`. `x x x` on a drum track is three
separate hits, because their gate is `retrig`, and only `tie` holds a note. The
two cases look different in the text because they sound different.

The 0xFF gate code recon.md used to call a tie does not occur once in the 109
songs here. The tie a song actually contains is this: the same note on
consecutive steps under gate `tie`, whose count of 13 sub-ticks outlives the
12-sub-tick step, so `0x0206E9D8` finds the note still inside its gate and emits
nothing instead of restarting it. It happens 6209 times, and 325 notes are held
across all 16 steps.

A patch is `name=value` tokens over knobs 0..30, with `filter=lp|bp|hp`,
`wave1=tri|saw|square|noise` and `sync=off|on` spelled out. The five modulation
slots are their own `mod` group — `dest=source:±intensity`, source one of
`sine saw square sh eg vco2` — and a slot with no source is not written at all.
That is the "not patched, not mentioned" rule.

The global instrument bank is not part of a song. It lives in the save, is
shared by all 18 songs, and nothing in a song refers to it, so `bank.txt` is its
own file: one `instrument <slot> "NAME" <knobs>` line per written slot, and the
slots nobody has used are absent.

## Defaults

A field is written only when it differs from these. The percentages are measured
over `data/saves` and the ROM's demos — 109 songs, 1744 patterns, 165888 steps:

| field | default | why |
|---|---|---|
| `swing` | 50 | straight |
| `steps` | 16 | 1738 of 1744 patterns |
| velocity | 127 | 96% of sounding steps |
| pan | 63 | 70% of sounding steps |
| gate | `25%` synth, `retrig` drum | the interface's neutral setting. Note this is not the majority: 79% of synth steps are `tie`, so their gate rows are explicit. `tie` as the synth default instead would cut the corpus from 13710 explicit gate cells in 3029 rows to 6393 in 1294 |
| patch knob | the ROM's INIT patch, `ds10synth00.dat` | a patch differs in 10 of 31 knobs on average |
| mix | the INIT template's mix | |
| lane | absent | 75% of the 6912 lane slots have nothing recorded |
| mod slot | absent | 82% of the 5 slots per patch have no source |
| name | absent | most of the six name fields in a pattern are empty |

A name is six raw bytes, not a C string: the game pads with `0x08` as often as
with NUL, and one factory patch is `CPU\0 1`. So `\b` inside a quoted name is
the game's own blank glyph and is kept — dropping it changes the file.

## Exactness

The dumper writes the named fields, parses its own output back, and appends a
`raw <offset> <hex>` line for every byte that did not survive the trip. Two
consequences: a field nobody has named yet still round-trips, and a song that
`songtext.py check` calls byte-identical is byte-identical in full, not just in
the parts I could name.

Verified: all **109** songs in `data/saves` and the ROM round-trip byte for byte,
and all **six** saves in `songs/` unpack to text and repack to a byte-identical
512 KiB save — from the text alone, with no original needed.

`--clean` drops exactly one thing: the gate, velocity and pan of a step with no
note. The sequencer reads those three bytes only when the note byte is not
0xFF, and they hold whatever the editor last left there — 129 distinct
combinations across the 109 songs. Dropping them takes a typical song's text
from 491 lines to 270 and its `raw` lines from 275 to 54. Measured on
`DS10PLUS.SAV`: a clean unpack/repack changes 54090 of 524288 bytes, every one
of them either a recomputed MD5 or one of those dead bytes, all 18 slots still
pass their hash check, and 90720 live fields compare equal. The song plays
identically; its hash does not match the original.

## What is still not named

`raw` lines in a clean dump are almost entirely the 12-byte per-track record at
pattern +0x25C. Byte 0 of it is the piano-roll scroll (`scroll`), bytes 2 and 3
are a copy of the two lanes' knob ids that agrees with the lane array in 94% of
cases and is not what playback reads, and bytes 1, 4, 5, 6 and 9 are unnamed.
The PLUS-only tail blocks at payload +0x2EFC and +0x2FA4 are unnamed too; the
first is nonzero in most songs, the second only ever in its last two bytes.
