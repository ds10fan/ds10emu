# songtext — a DS-10 song as text

Two implementations: `src/extras/songtext.c` for anything that links the engine,
and `tools/songtext.py`, which is where the format is worked out. `make
test-songtext` is the round trip, in C alone; `make test-songtext-py` holds the two
to the same output byte for byte and is kept out of `make test` because it needs
python3.

```sh
make songtext && out/songtext dump [--clean] x.ds10song [x.ds10song.txt]  # load, check
python3 tools/songtext.py dump x.ds10song [x.ds10song.txt]             # the reference
python3 tools/sav.py unpack x.ds10save dir/   # slotNN.ds10song.txt + bank.ds10patch.txt
```

```c
#include "extras/songtext.h"
size_t n = ds10_songtext_write(payload, len, NULL, clean, buf, sizeof buf);
int    e = ds10_songtext_read(buf, payload, sizeof payload, &len, &errline);
/* or ds10_songtext_dump / _parse, which take a parsed ds10_song */
```

## Example

```
[song]
payload 12252
version 2
tempo 127
swing 54
name "FLUTER"
chain 0 1 1

[pattern 0]
steps 12
syn1  name "XLEAD"
syn1  patch cutoff=57 peak=51 filter=bp wave1=square
syn1  mod cutoff=sine:+7
syn1  note D5 D6 C3 -    | -    .  .  .  | .  .  .  .
      gate .  .  .  .    | 100% .  .  .  | .  .  .  .
      vel  .  .  92 .    | .    .  .  .  | .  .  .  .
drum1 note x  .  x  .    | x    .  x  .  | x  .  x  .
syn1  lane1 knob=cutoff 0=69 1=62
mix route=drums vol=105,105,104,105,66,105
view 46,3,11,12,1,0,2,0,0 31,3,11,12,1,0,2,0,0 36,0,0,0,0,0,1,0,0 36,0,0,0,0,0,1,0,0 57,0,0,0,0,0,1,0,0 36,0,0,0,0,0,1,0,0
```

## What it compiles to

| line | compiles to | value |
|---|---|---|
| `payload 12252` | the PLUS length, which picks the record size | 12252, after a 16-byte MD5 |
| `version 2` | `+0x00` u16 | 2 |
| `tempo 127` | `+0x04` u32 | 127 |
| `swing 54` | `+0x0A` u16 | 54, so a step pair splits 13/11 sub-ticks |
| `name "FLUTER"` | `+0x14`, 16 bytes, 6 shown | `46 4c 55 54 45 52 00 00` |
| `chain 0 1 1` | `+0x24`, u16 per bar | 0 1 1, then 0xffff ends the song |
| `steps 12` | pattern `+0x000` u32 | 12 |
| `syn1 patch …` | pattern `+0x004`, knobs 0..30 | knob 11 cutoff=57, 12 peak=51, 13 filter=1 (bp), 6 wave1=2 (square) |
| `syn1 mod cutoff=sine:+7` | knobs 29 and 15 | source 1 (sine), intensity 7 |
| `syn1 name "XLEAD"` | payload `+0x2BFC` + pattern*48 | `58 4c 45 41 44 00` |
| `syn1 lane1 knob=cutoff 0=69 1=62` | pattern `+0x1C2` | raw 69, 62, then -1 for the rest; knob 11 on every step |
| `mix route=drums vol=…` | pattern `+0x242`, 25 knobs | knob 0=3 (drums), 17..22=105,105,104,105,66,105 |
| `view 46,3,11,12,…` | pattern `+0x25C`, six 12-byte editor records | track 0: scroll 46, octave 3, lane knobs 11 and 12, pad scale IONIAN key C, `SMT`, pad slot 0, no `LOCK` |

The step rows of `syn1`, four bytes per step at pattern `+0x042 + track*64 + step*4`:

| step | `note` | `gate` | `vel` | gate byte | note byte | vel | pan |
|---|---|---|---|---|---|---|---|
| 0 | `D5` | `.` | `.` | 1 = 25% | 74 = D5 | 127 | 63 |
| 1 | `D6` | `.` | `.` | 1 = 25% | 86 = D6 | 127 | 63 |
| 2 | `C3` | `.` | `92` | **5 = tie** | 48 = C3 | 92 | 63 |
| 3 | `-` | `.` | `.` | **5 = tie** | 48 = C3 | 127 | 63 |
| 4 | `-` | `100%` | `.` | 4 = 100% | 48 = C3 | 127 | 63 |
| 5 | `.` | `.` | `.` | 3 | 255 = no note | 127 | 64 |

- Steps 2–4 are **one** C3 lasting three steps: each `-` ties the step before it,
  and `tie` is the only gate that holds a note.
- A run's last gate ends it — `100%` here, so C3 releases a sub-tick before step 4
  finishes.
- Step 5's three non-note bytes are filler the sequencer never reads.

## Sections

| section | holds |
|---|---|
| `[song]` | header fields, the four drum instruments |
| `[pattern N]` | one pattern, N = 0..15 |
| `[raw]` | `raw <offset> <hex>`, payload-relative — bytes no keyword names yet |

## `[song]` lines

| line | bytes |
|---|---|
| `payload N` | one of 11212, 11260, 12028, 12252 |
| `version N` | `+0x00`: 1 = DS-10, 2 = PLUS, 0 or 0xFFFF = pre-release |
| `revision N` | `+0x02` |
| `tempo N` | `+0x04` BPM |
| `f08 N` | `+0x08`, 12 from v1 on |
| `swing N` | `+0x0A` percent |
| `name "…"` | `+0x14`, 16 bytes |
| `chain A B C …` | `+0x24`, one u16 per bar |
| `mute a bar=mask …` | `+0x2EFC`, six bits of track per bar, five bars to a word |
| `mute b bar=mask …` | `+0x2F50`, the same for the second deck |
| `trs bar=semitones …` | `+0x2FA8`, one signed byte per bar |
| `drumN patch …` | `+0x2B2C + (N-1)*31`, knobs 0..30 |
| `drumN mod …` | the same knobs, modulation slots only |
| `drumN fx …` | `+0x2BA8 + (N-1)*9`: route, fx type, time, ratio, rate, depth, sync, feedback, dry/wet |

## `[pattern N]` lines

| line | bytes |
|---|---|
| `steps N` | `+0x000` |
| `<track> patch …` / `mod …` | `+0x004`, 31 knobs per synth track |
| `<track> name "…"` | payload `+0x2BFC + N*48 + track*8` |
| `<track> note/gate/vel/pan …` | `+0x042 + track*64 + step*4` |
| `<track> laneL knob=K S=V …` | `+0x1C2 + track*64 + step*4`; `S=V` per step that has a value |
| `mix …` / `fx …` | `+0x242`, 25 knobs |
| `view <t0> … <t5>` | `+0x25C + track*12`, nine bytes per track, comma-separated |

Tracks are `syn1`, `syn2`, `drum1`..`drum4`. Lanes are `lane1`, `lane2`, on synth
tracks only.

## Step rows

- Four rows per track, the four pages the sequencer's sub-mode button steps through.
- `note` is always there; `gate`, `vel` and `pan` only when a step is off default.
- One column width per pattern, so a step is in the same column in every row.
- `|` is a bar mark the parser drops.

| `note` cell | means |
|---|---|
| `C4`, `G#2` | MIDI name, 60 = C4 |
| `x` | drum track at note 60, unity playback speed |
| `.` | no note |
| `-` | carry the previous note on, and tie the step before it |

`.` in `gate`, `vel` or `pan` means that step is at the default. A repeat that is
not a tie keeps its note name: `x x x` under `retrig` is three hits, `x - -` would
be one held sample.

## `view` — the editor records

One line per pattern, one comma-separated group per track, nine bytes each. Mostly
editor state, and it repeats from pattern to pattern, which is why it is one line
rather than six.

| position | byte | what | seen |
|---|---|---|---|
| 0 | 0 | piano-roll scroll, counting upward | 0..84 |
| 1 | 1 | keyboard octave — the row of six dots over the keys | 0..5 |
| 2, 3 | 2, 3 | a copy of the two lanes' knob ids — agrees with the lane array in 94% of cases and is *not* what playback reads | |
| 4 | 4 | KAOSS pad scale, index into the table below | 0..29 |
| 5 | 5 | KAOSS pad key, 0 = C .. 11 = B | 0..11 |
| 6 | 6 | automation path: 1 = `HLD`, 2 = `SMT`, 0 = off | 0..2 |
| 7 | 7 | KAOSS pad slot the `1`/`2`/`3` buttons select | 0..2 |
| 8 | 9 | pattern page `LOCK`: 1 = engaged, 0xFF = neither it nor `COPY` | 0, 1, 0xFF |

Bytes 8, 10 and 11 of the record are not in the line. The accessor at `0x0206B0C0`
takes a field 0..9 and reads record byte *field*, so byte 8 has one; nothing calls
it with 8, and 10 and 11 have no accessor at all. All three are zero in all 10464
records here.

Position 6 is the one the sequencer reads: it lands in the automation object's
`enable` (`0x0206DD4C`, live `+0x8A`), and `ds10_seq_run` only runs a lane whose
enable selects which of its two paths runs: 1 latches a step's value, 2 ramps into
the next step's. Both are implemented, `songplay` installs the pattern's own value,
and `make test-seq` holds the ramp to the original's own processor over 900 cases.

Pad scales, `0x02084E54`, 16 bytes each:

| | | | | | |
|---|---|---|---|---|---|
| 0 CHROMA | 1 IONIAN | 2 DORIAN | 3 PHRYGI | 4 LYDIAN | 5 MIXLYD |
| 6 AEOLIA | 7 LOCRIA | 8 MBLUES | 9 mBLUES | 10 DIM | 11 COMDIM |
| 12 MPENTA | 13 mPENTA | 14 RAGA1 | 15 RAGA2 | 16 RAGA3 | 17 SPANISH |
| 18 GYPSY | 19 ARABIA | 20 EGYPT | 21 HAWAII | 22 PELOG | 23 JAPAN |
| 24 RYUKYU | 25 WHOLE | 26 m3RD | 27 M3RD | 28 4TH | 29 5TH |
| 30 OCTAVE | | | | | |

Keys are the twelve names at `0x02084B70`, 8 bytes each, `C` through `B`.

## Gates

Sub-tick counts are for a straight step of 12 (24 per step pair, split by swing).

| token | code | ticks | behaviour |
|---|---|---|---|
| `off` | 0 | 0 | released on the sub-tick it starts |
| `25%` `50%` `75%` | 1, 2, 3 | 3, 6, 9 | `code * subticks / 4` |
| `100%` | 4 | 11 | a tick short, so the next note articulates |
| `tie` | 5 | 13 | outlives the step; an identical next step is held, not restarted |
| `retrig` | 6 | — | restrikes, never auto-releases: the sample plays out |
| `hold` | 0xFF | — | sounds until an empty step releases it |

`retrig` is the drum tracks': 12691 drum steps and no synth step in the ROM or in
the 108 saved songs use it. `hold` is in no file at all and the interface has no
button for it - the one way it could reach a pattern is record mode, where
`0x0206E5AC` writes the live keyboard's gate into the step it is passing
(`rec[1] = live_gate`), so a note still held down has no gate to record yet. Both
are readable and writable here, and neither is exercised by any real data.

## Synth knobs

The 31 of a patch, i.e. bytes `0x20..0x3E` of a `ds10synth*.dat`, the 31 at
pattern `+0x004` per synth track, and each drum's 31 at `+0x2B2C`. Ranges are the
synth's own knob descriptors, so signedness comes from the range and not from the
data. `INIT` is the ROM's INIT patch, which is this format's default - those
values are the ones never written.

| knob | name | range | enc | INIT | |
|---|---|---|---|---|---|
| 0 | `octave` | -2..2 | s8 | 0 |  |
| 1 | `pitcheg` | -63..63 | s8 | 0 |  |
| 2 | `porta` | 0..127 | u8 | 0 | portamento |
| 3 | `pitchint` | -63..63 | s8 | 0 | intensity for `mod pitch=` |
| 4 | `vco1int` | -63..63 | s8 | 0 | intensity for `mod vco1p=` |
| 5 | `vco2int` | -63..63 | s8 | 0 | intensity for `mod vco2p=` |
| 6 | `wave1` | 0..3 | u8 | 1 | `tri` `saw` `square` `noise` |
| 7 | `wave2` | 0..3 | u8 | 1 | `tri` `saw` `square` `noise` |
| 8 | `vco2pitch` | -63..63 | s8 | 3 |  |
| 9 | `balance` | 0..127 | u8 | 0 | VCO1 to VCO2 |
| 10 | `sync` | 0..1 | u8 | 0 | `off` `on` |
| 11 | `cutoff` | 0..127 | u8 | 127 |  |
| 12 | `peak` | 0..127 | u8 | 0 | resonance |
| 13 | `filter` | 0..2 | u8 | 0 | `lp` `bp` `hp` |
| 14 | `cutoffeg` | -63..63 | s8 | 0 | VCF EG intensity |
| 15 | `cutoffint` | -63..63 | s8 | 0 | intensity for `mod cutoff=` |
| 16 | `vcaeg` | 0..1 | u8 | 0 | `off` `on` |
| 17 | `vcaint` | -63..63 | s8 | 0 | intensity for `mod vca=` |
| 18 | `drive` | 0..127 | u8 | 0 |  |
| 19 | `level` | 0..127 | u8 | 105 |  |
| 20 | `attack` | 0..127 | u8 | 0 |  |
| 21 | `decay` | 0..127 | u8 | 66 |  |
| 22 | `sustain` | 0..127 | u8 | 127 |  |
| 23 | `release` | 0..127 | u8 | 0 |  |
| 24 | `mgfreq` | 0..127 | u8 | 19 | LFO rate |
| 25 | `mgsync` | 0..1 | u8 | 0 | `off` `on`, LFO to BPM |
| 26 | `pitchsrc` | 0..6 | u8 | 0 | source for `mod pitch=` |
| 27 | `vco1src` | 0..6 | u8 | 0 | source for `mod vco1p=` |
| 28 | `vco2src` | 0..6 | u8 | 0 | source for `mod vco2p=` |
| 29 | `cutoffsrc` | 0..6 | u8 | 0 | source for `mod cutoff=` |
| 30 | `vcasrc` | 0..6 | u8 | 0 | source for `mod vca=` |

Knobs 26..30 take `none sine saw square sh eg vco2`. A routed slot and its
intensity are written as one `mod <dest>=<source>:<intensity>` token instead of
two `patch` tokens, and an unrouted one is not written at all. Any of the 31 can
be an automation lane's target: `syn1 lane1 knob=cutoff`.

## Mixer knobs

The 25 at pattern `+0x242`, from the dispatch at `0x020736C0`.

| knob | text | goes to |
|---|---|---|
| 0 | `mix route=` | `syn1` `syn2` `syn1+syn2` `drums` `all` `off` |
| 1 | `fx <type>` | `delay` `chorus` `flanger` |
| 2 | `fx time=` | delay time, or the chorus and flanger equivalent |
| 3 | `fx ratio=` | L/R ratio |
| 4 | `fx rate=` | LFO rate, shared by all three types |
| 5 | `fx depth=` | depth |
| 6 | `fx sync=` | tempo-sync divisor, delay only |
| 7 | `fx fb=` | feedback |
| 8 | `fx wet=` | dry/wet; forced to 0 when the route is `off` |
| 9..14 | `mix pan=` | pan for syn1, syn2, drum1..4 - `value*258`, linear |
| 15, 16 | `fx k15= k16=` | nonexistent parameters: the mixer forwards them to the effect, whose setter has nothing at 15 or 16, so they are stored and never act. 127 or 0 in every song, and kept only because they are in the file |
| 17..22 | `mix vol=` | volume, same channel order - `value^2 * 0x10404 >> 15`, quadratic |
| 23, 24 | - | the setter returns early above 22; zero in all 1744 patterns |
## Lanes never retarget

A lane's knob id is stored on all 16 steps, but the machine is only ever set up
from step 0 (`0x0206DEE4`), so the target cannot change inside a pattern and the
format writes it once, as `knob=`. 31 of the 6912 lane slots in `data/saves` do
hold a second id on their later steps - what the editor left behind when the
target was changed - and those come back as `raw` lines rather than as syntax for
something the engine cannot do.

## Defaults

Written only when a field differs. Percentages are over `data/saves` and the ROM's
demos: 109 songs, 1744 patterns, 165888 steps.

| field | default | measured |
|---|---|---|
| `swing` | 50 | straight |
| `steps` | 16 | 1738 of 1744 patterns |
| `vel` | 127 | 96% of sounding steps |
| `pan` | 63 | 70% of sounding steps |
| `gate` | `25%` synth, `retrig` drum | the interface's neutral setting, not the majority — 79% of synth steps are ties, so they say so. `tie` as the synth default would cut 13710 explicit gate cells in 3029 rows to 6393 in 1294 |
| patch knob | the ROM's INIT patch, `ds10synth00.dat` | a patch differs in 10 of 31 knobs on average |
| `mix` | the INIT template's mix | |
| lane | absent | 75% of the 6912 lane slots hold no automation |
| mod slot | absent | 82% of the 5 slots per patch have no source |
| name | absent | most of the six per-pattern name fields are empty |

A name is six raw bytes, not a C string: padding is `0x08` as often as NUL, and one
factory patch is `CPU\0 1`. `\b` is that `0x08`, and it is kept because dropping it
changes the file. It is not a space: the interface's own font has no glyph mapped
at `0x08`, so it advances nothing at all — `out/fonttest fonts.NFTR $'A\bA'`
renders exactly what `AA` does, while `A A` leaves a gap.

## The instrument bank

Save-level, not song-level: 24 slots at `0x38000`, stride `0x100`, shared by all 18
songs and referenced by none of them. `sav.py unpack` writes it to
`bank.ds10patch.txt` in the same shape a pattern has — a section and its lines —
with the slots nobody has used simply absent.

```
[instrument 3]
name "TANK\01"
patch vco2pitch=30 balance=51 sync=on cutoff=55 peak=71 drive=48
mod vco2p=saw:+16 cutoff=sine:+8
```

## Exactness

- The dumper parses its own output back and appends a `raw` line for every byte
  that did not survive, so unnamed fields round-trip too.
- All **109** songs in `data/saves` and the ROM round-trip byte for byte, in C
  (`make test-songtext`) and in Python.
- The C and the Python produce identical text for every song in both modes -
  218 dumps, `make test-songtext-py`. This is what caught the two bugs the round
  trip could not see, both quoted names holding a space.
- All **six** saves in `songs/` unpack to text and repack to a byte-identical
  512 KiB save, from the text alone.
- `--clean` drops one thing: the gate, velocity and pan of a step with no note —
  dead bytes, 129 distinct combinations across the corpus. A song's text goes from
  491 lines to 270. On `DS10PLUS.ds10save` that changes 54090 of 524288 bytes, all of
  them dead bytes or recomputed MD5s; all 18 slots still hash and 90720 live
  fields compare equal.

## `mute` and `trs` — the PLUS tail

The 224 bytes a PLUS payload has beyond a DS-10 one are the two per-bar lanes of
the `31_SONG` page, which has three modes — `PATTERN`, `MUTE` and `TRS`.

| line | at | shape |
|---|---|---|
| `mute a` | `+0x2EFC`, 84 bytes | 6 bits of track per bar, 5 bars to a word, 105 slots. `DECK-A` |
| `mute b` | `+0x2F50`, 84 bytes | the same, `DECK-B` |
| — | `+0x2FA4`, 4 bytes | five more 6-bit slots; only the page's "clear song" ever writes them, nothing reads them, zero in the whole corpus |
| `trs` | `+0x2FA8`, 52 bytes | one signed byte of semitones per bar |

- Bit 0 is `syn1` and bit 5 is `drum4`, the order of the twelve row labels at
  `0x0207E668` — `SYN1 SYN2 DRUM1..DRUM4`, once per deck. So `31=60` is the four
  drums muted in bar 31 and `20` is `drum1` and `drum3`.
- A `mute` bit set is a track muted in that bar: 78 of the 97 saved songs have both
  masks all-zero, and the page draws a marker where a bit is set. `mute b` is
  all-zero in every one of them — deck B is the DSi build's second instance, which
  §6 of recon.md puts out of reach.
- Both are indexed by bar 0..99 — the same 100 bars as `chain`. A mask array has
  room for 105, but `trs` runs out at the end of the payload after 52, so bars
  52..99 of a transpose cannot survive a save. The ROM's own accessors write all
  100 either way.
- The two bytes the corpus does have set, `trs 50` and `trs 51`, are not loaded:
  after a boot with such a song the live document has zero at both. They are the
  record buffer's leftovers, which is why they read as transposes for bars that
  most of those songs do not have.

## Not named yet

- pattern `+0x25C`: bytes 8, 10 and 11 of the per-track record, which no accessor
  call site touches and which are zero in all 10464 records here.
- nothing in a drum's own effect: `drumN fx` is named, `src/core/drumbuf.c` applies
  it, and `make test-drumbuf` checks the result against a running DS. The two
  residuals it still has are in TODO.md §5.
