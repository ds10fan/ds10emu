# songtext — a DS-10 song as text

`tools/songtext.py` turns a song file into text and back; `tools/sav.py unpack`
does it for a whole save at once, writing `slot00.txt` … `slot17.txt` plus
`bank.txt`, and `sav.py repack` builds a save file out of that directory.

The point is that the interesting parts of a song — patches, notes, the mix —
are named and readable, and that anything sitting at a default is simply absent.
A pattern that nobody touched, an automation lane with nothing recorded in it, a
modulation slot with no source: none of them appear at all.

## Shape

Line oriented: a keyword, then tokens. `#` starts a comment when it is at the
start of a line or follows whitespace — so `C#2` is a note, not a truncated
line. Indentation and blank lines are ignored; a `|` inside a step grid is a bar
mark the parser drops. Everything before the first `pattern` line is the song;
after it, lines belong to that pattern.

```
payload 12252              # 11212, 11260, 12028 or 12252 - the four that exist
version 2                  # 1 = DS-10, 2 = PLUS, 0 or 0xFFFF = pre-release
revision 1
tempo 127
name "FLUTER"
chain 12 13 12 14 15 15 15 15        # bar -> pattern; absent means no song

drum1 patch octave=-2 cutoff=23 peak=83 decay=19 release=21
drum1 extra 0 0 63 0 62 60 0 0 0     # the nine per-drum bytes nobody has named
drum2 mod vca=eg:+63

pattern 0
  syn1 name "XLEAD"
  syn1 patch porta=14 wave1=square cutoff=57 peak=51 cutoffeg=14 mgfreq=77
  syn1 mod pitch=sine:+4
  syn1 .  .  G4 .  | .  G4 G4 .  | E4 E4 .  F4 | .  .  .  .
  drum1 x . x . | . . . . | . . x . | . . . .
  drum4 F2,p64 .  .  .  | .  .  .  .  | .  .  E2,p64 .  | .  .  .  .
  syn2 lane1 knob=cutoff 0=69 1=62 2=51 3=45 4=48 5=50 6=37
  mix route=syn1+syn2 vol=105,105,104,105,66,105 pan=65,62,61,63,74,56
  fx delay time=14 ratio=14 rate=23 depth=62 sync=0 fb=38 wet=52
  scroll 46 31 36 36 57 36           # each track's piano-roll position
  steps 12                           # only when it is not 16
```

A step cell is `<note>[:<gate>][,v<velocity>][,p<pan>]`, or `.` for no note, or
`x` on a drum track for the root note 60 — unity playback speed for a drum
buffer. Notes are MIDI names with 60 = C4. Gates are `tie`, `retrig`, `over`,
`full`, `3/4`, `2/4`, `1/4` (recon.md §13: 4 and 5 are a sub-tick short of and
over a full step, 6 retriggers, 0xFF ties, and anything else is
`gate * subticks / 4`).

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
| gate | `over` synth, `retrig` drum | 79% and 68% of their sounding steps |
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
