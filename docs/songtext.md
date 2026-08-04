# songtext — a DS-10 song as text

`tools/songtext.py` is the format: one implementation, in Python, while the format
is still moving. `make test-songtext` is the round trip over every song here. (A C
encoder existed alongside it and was byte-identical; it was removed because keeping
two in step cost more than it caught. Nothing about the format below depends on
that — exactness is checked the same way either way.)

```sh
python3 tools/songtext.py dump [--clean] x.ds10song [out.txt]   # load, check
python3 tools/sav.py unpack x.ds10save dir/   # slotNN.ds10song.txt + bank.ds10patch.txt
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
syn1  note D5   D6   C3   -    | -    .    .    .    | .    .    .    .
      gate 25%  25%  .    .    | 100% .    .    .    | .    .    .    .
      vel  .    .    92   .    | .    .    .    .    | .    .    .    .
drum1 note x    .    x    .    | x    .    x    .    | x    .    x    .
syn1  lane1 knob=cutoff 0=69 1=62
mix route=drums vol=105,105,104,105,66,105
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
| `syn1 auto interp=hld` | pattern `+0x25C + track*12 + 6` | 1 |

The step rows of `syn1`, four bytes per step at pattern `+0x042 + track*64 + step*4`:

| step | `note` | `gate` | `vel` | gate byte | note byte | vel | pan |
|---|---|---|---|---|---|---|---|
| 0 | `D5` | `25%` | `.` | 1 = 25% | 74 = D5 | 127 | 63 |
| 1 | `D6` | `25%` | `.` | 1 = 25% | 86 = D6 | 127 | 63 |
| 2 | `C3` | `.` | `92` | **5 = tie**, the default | 48 = C3 | 92 | 63 |
| 3 | `-` | `.` | `.` | **5 = tie** | 48 = C3 | 127 | 63 |
| 4 | `-` | `100%` | `.` | 4 = 100% | 48 = C3 | 127 | 63 |
| 5 | `.` | `.` | `.` | 3 | 255 = no note | 127 | 64 |

- Steps 2–4 are **one** C3 lasting three steps: each `-` ties the step before it,
  and `tie` is the only gate that holds a note.
- A run's last gate ends it — `100%` here, so C3 releases a sub-tick before step 4
  finishes. Left at the default it would tie instead, holding through step 4 and
  releasing at step 5.
- `tie` is the synth default, so the two staccato notes are the ones that need a
  cell. That is the way round the songs here are: 79% of their sounding synth steps
  are ties.
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
| `<track> auto interp=…` | `+0x25C + track*12 + 6`, the automation path |
| `<track> laneL knob=K [shown=K] S=V …` | `+0x1C2 + track*64 + step*4`, and the record's copy of the target at `+0x25C + track*12 + 2`; `S=V` per step that has a value |
| `mix …` / `fx …` | `+0x242`, 25 knobs |

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

`.` in `gate`, `vel` or `pan` means that step is at the default: `tie` on a synth
track, `retrig` on a drum one. So a synth line with no `gate` row at all is legato
throughout, which is what most of the songs here are, and a staccato step is the one
that says so.

`-` sets the step it is on to that default too, so `C3 - -` is three tied steps —
one note that holds through the run and releases on the step after it. Write the
last cell if you want it to stop sooner: `gate . . 25%` releases a quarter into the
third step.

A repeat that is not a tie keeps its note name: `x x x` under `retrig` is three
hits, `x - -` would be one held sample.

## Automation — `lane`, `auto`, and two copies of the target

```
syn1 lane1 knob=cutoff 0=69 1=62     # the target, then the steps that have a value
syn1 auto interp=hld                 # the path, when it is not the default
```

A lane's target is stored more than once, and the copies can disagree:

| where | what | who reads it |
|---|---|---|
| `+0x1C2 + track*64 + step*4 + 2 + lane` | a knob id on **every one of the 16 steps** | the lane initialiser `0x0206DEE4` reads step 0 and nothing else |
| `+0x25C + track*12 + 2 + lane` | one more copy per track | the KAOSS page: `0x0203CD5C` loads it into the page's own slots, `0x020445D0` writes it back |

`knob=` is the first one, the one that plays. The second is a KAOSS-page echo with no
line of its own; it rides in `[raw]` with the rest of the editor record.

- The disagreements are real, not a decoding error: they differ in 443 of 7176 lane
  slots, 103 of them on lanes that have recorded values. `new_16` pattern 2 has the
  array saying `peak` and the record saying `cutoff`, and 23 records hold `31` in the
  copy, which is not a knob id at all (the synth has 31 knobs, 0..30). The editor
  updates the two on different occasions; playback is unaffected, since it only ever
  reads step 0 of the array.
- In 31 lane slots the array disagrees *with itself* — a later step carrying a
  different id from step 0, which the engine cannot act on. Those come back as `raw`
  lines rather than as syntax for something the hardware ignores.
- `auto interp=` is the record's byte 6, the automation object's `enable`
  (`0x0206DD4C`, live `+0x8A`). `hld` latches a step's value for the whole step,
  `smt` ramps into the next step's, `off` is the third value one demo carries. Both
  paths are implemented, `songplay` installs the pattern's own value, and
  `make test-seq` holds the ramp to the original's own processor over 900 cases.
- Defaults: `smt` on a synth track, `hld` on a drum one — 3158 of the 3488 synth
  records here are `smt`, and every drum record is `hld`, drums having no lanes at
  all. That leaves 394 `auto` lines in the whole corpus.

## The editor records — no syntax, on purpose

Pattern `+0x25C` holds six 12-byte records, one per track, and the format has no
line for them. They are editor state: where that track's piano roll is scrolled,
what the KAOSS page has selected, which of `COPY`/`LOCK` the pattern page last had
down. A plain dump carries them in `[raw]`, which is what keeps it byte-exact;
`--clean` drops them, the way it drops the dead bytes of a step with no note.

They had named `view` lines for a while. One line per track per pattern, ~96 a song,
for state nothing plays — not worth it. The one byte of a record that *is* played is
the automation path, and it has its own line (`<track> auto interp=`).

What the twelve bytes are, since the accessor `0x0206B0C0` names nine of them (`cmp
field,#9`, then a ten-case jump table where case *i* is `ldrb [record + i]`, so a
field index is a byte index):

| byte | what | read by | seen |
|---|---|---|---|
| 0 | piano-roll scroll, counting upward | `0x02050108`, the sequencer page | 0..84 |
| 1 | keyboard octave — the six dots over the keys | `0x0204BFF8`, the keyboard | 0..5 |
| 2, 3 | the KAOSS page's copy of the two lanes' targets, which is *not* what plays | `0x02049150` | |
| 4 | KAOSS pad scale, an index into the table below | `0x0204AB00` | 0..29 |
| 5 | KAOSS pad key, 0 = C .. 11 = B | `0x0204AB00` | 0..11 |
| 6 | **the automation path** — 1 `HLD`, 2 `SMT`, 0 off | `0x02052864` → `0x0206DD4C`, into the sequencer | 0..2 |
| 7 | KAOSS pad slot, the `1`/`2`/`3` buttons | `0x02049150` | 0..2 |
| 8, 10, 11 | nothing calls the accessor with 8, and 10 and 11 have no case at all; zero in all 10464 records here | — | 0 |
| 9 | pattern page `LOCK`: 1 engaged, 0xFF neither it nor `COPY` | `0x0202E01C` | 0, 1, 0xFF |

Byte 6 is the only one that leaves the interface. Bytes 1, 4, 5 and 7 shape what the
keyboard and the pad *produce when touched* — `0x0204AB00` loads 4 and 5 into the
page's own state and the touch handler makes notes with them — so they matter to an
interactive build and to what a player records, not to rendering a song that is
already recorded. On this side, core reads `params[1 + track*12 + 6]` and nothing
else out of the record.

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
| `gate` | `tie` synth, `retrig` drum | what songs contain rather than what the interface starts at: 79% of sounding synth steps are ties. Against a `25%` default this drops 7525 of the corpus's 17546 explicit gate cells, and 1760 of its 3477 gate rows |
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
- All **109** songs in `data/saves` and the ROM round-trip byte for byte:
  `make test-songtext`.
- All **six** saves in `songs/` unpack to text and repack to a byte-identical
  512 KiB save, from the text alone.
- `--clean` drops the bytes nothing plays: every pattern's six editor records, and
  the gate, velocity and pan of a step with no note — 129 distinct combinations of
  those three across the corpus. `xfer2_10` goes from 538 lines to 267, `fresh10_01`
  from 266 to 174. On `DS10PLUS.ds10save` it changes 58962 of 524288 bytes, all of
  them editor state, dead step bytes or recomputed MD5s.

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
