# songtext — a DS-10 song as text

```sh
python3 tools/songtext.py dump song.dat [out.txt]   # and `load` for the way back
python3 tools/sav.py unpack save.SAV dir/           # 18 songs + bank.txt; `repack` rebuilds
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
| `drumN patch …` | `+0x2B2C + (N-1)*31`, knobs 0..30 |
| `drumN mod …` | the same knobs, modulation slots only |
| `drumN extra …` | `+0x2BA8 + (N-1)*9`, unnamed |

## `[pattern N]` lines

| line | bytes |
|---|---|
| `steps N` | `+0x000` |
| `<track> patch …` / `mod …` | `+0x004`, 31 knobs per synth track |
| `<track> name "…"` | payload `+0x2BFC + N*48 + track*8` |
| `<track> note/gate/vel/pan …` | `+0x042 + track*64 + step*4` |
| `<track> laneL knob=K S=V …` | `+0x1C2 + track*64 + step*4` |
| `mix …` / `fx …` | `+0x242`, 25 knobs |
| `scroll A B C D E F` | `+0x25C + track*12`, each track's piano-roll position |

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

## Gates

Sub-tick counts are for a straight step of 12 (24 per step pair, split by swing).

| token | code | ticks | behaviour |
|---|---|---|---|
| `off` | 0 | 0 | released on the sub-tick it starts |
| `25%` `50%` `75%` | 1, 2, 3 | 3, 6, 9 | `code * subticks / 4` |
| `100%` | 4 | 11 | a tick short, so the next note articulates |
| `tie` | 5 | 13 | outlives the step; an identical next step is held, not restarted |
| `retrig` | 6 | — | restrikes, never auto-releases: the sample plays out |
| `hold` | 0xFF | — | sounds until an empty step releases it; no button, in no file |

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
| 15, 16 | `fx k15= k16=` | forwarded to the effect, which has no parameter there; 127 or 0 in every song |
| 17..22 | `mix vol=` | volume, same channel order - `value^2 * 0x10404 >> 15`, quadratic |
| 23, 24 | - | the setter returns early above 22; zero in all 1744 patterns |
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
factory patch is `CPU\0 1`. `\b` in a quoted name is the game's blank glyph and is
kept — dropping it changes the file.

## The instrument bank

Save-level, not song-level: 24 slots at `0x38000`, stride `0x100`, shared by all 18
songs and referenced by none of them. `sav.py unpack` writes it to `bank.txt`, one
line per written slot, absent slots omitted.

```
instrument 3 "TANK\01" vco2pitch=30 balance=51 sync=on cutoff=55 peak=71 drive=48
instrument 3 mod vco2p=saw:+16 cutoff=sine:+8
```

## Exactness

- The dumper parses its own output back and appends a `raw` line for every byte
  that did not survive, so unnamed fields round-trip too.
- All **109** songs in `data/saves` and the ROM round-trip byte for byte.
- All **six** saves in `songs/` unpack to text and repack to a byte-identical
  512 KiB save, from the text alone.
- `--clean` drops one thing: the gate, velocity and pan of a step with no note —
  dead bytes, 129 distinct combinations across the corpus. A song's text goes from
  491 lines to 270. On `DS10PLUS.SAV` that changes 54090 of 524288 bytes, all of
  them dead bytes or recomputed MD5s; all 18 slots still hash and 90720 live
  fields compare equal.

## Not named yet

- pattern `+0x25C`, the 12-byte per-track record: byte 0 is `scroll`, bytes 2 and 3
  copy the lanes' knob ids (agreeing with the lane array in 94% of cases, and not
  what playback reads), bytes 1, 4, 5, 6 and 9 unknown.
- payload `+0x2EFC` (168 bytes, PLUS) and `+0x2FA4` (56, only its last two bytes
  ever non-zero).
- `drumN extra`, nine bytes per drum.
