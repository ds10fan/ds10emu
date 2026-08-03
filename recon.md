# recon: ds10plus.nds

Initial reconnaissance pass. Everything below was produced by the scripts in
`tools/` and can be re-derived with `make recon`. Claims are marked
**confirmed** (cross-checked, reproducible) or **hypothesis** (needs work).

Subject:

```
ds10plus.nds   33554432 bytes   sha1 85064992fe671f6e34ffeb01570c9cce56a6a245
Nintendo DS ROM "DS10PLUS" (VKGE, maker XJ, rev 00), DSi-enhanced, decrypted
```

`VKGE` / `XJ` is KORG DS-10 PLUS. Unit code `0x02` = runs on both DS and DSi.
The ROM is padded to 32 MB; the DS-mode payload ends at `0x71A400`.

---

## 0. Prior art — substantial, and for the identical ROM

There is an existing DS-10 reverse-engineering effort by **thement**:

- Project log: <http://ibawizard.net/~thement/ds10rev.html> (plain HTTP only —
  the host refuses TLS, so tools that force HTTPS cannot fetch it)
- Code: <https://github.com/thement/ds10-vst>

**Their ROM is byte-identical to ours** — `md5 70474bb824b56912cc419221aa8ae015`
matches — so every address they publish applies directly to our extraction
without rebasing. Their findings are integrated into the sections below and
attributed as *(thement)*. Where our independent work agrees, that is noted,
because two derivations from opposite directions agreeing is much stronger than
either alone.

What they established: the synth runs on **ARM9**, at **32768 Hz**, in frames of
**64 samples**; six voices (2 synth + 4 drum) plus mixer and effects; the struct
layouts; the 31-knob parameter table; the event encoding; and the key function
addresses. See §5 and §10.

What they did **not** finish, which is where the real work remains: the effects
block ("attempt to make effects work didn't work very well"), the mixer, the
drum knob handlers, and the save-file contents beyond the patch parameters.

Important about the nature of their deliverable: `ds10-vst` is **not a
reimplementation**. It is a mechanical ARM-to-C translation of the original code
(`R5 = R2 + R1; goto lab0204720;`) with the DS-10 image embedded in the DLL,
plus a hand-written VST wrapper. The author's own assessment of the translator
was that it "works only by chance" and needed a rewrite. It is accurate by
construction and it proves the engine's behaviour is reproducible — but it is not
readable, portable, or cleanly distributable C. That distinction drives the road
choice in `PLAN.md` §1.

---

## 1. Container — confirmed

```
                 rom offset     load addr     size
ARM9 static      0x004000       0x02004000    0x055DD8   BLZ-compressed
ARM7 static      0x05B400       0x02380000    0x028F84   plain
ARM9 overlay tbl 0x059E00                     0x20       1 overlay
FNT              0x084400                     0x0021C7
FAT              0x086600                     0x000FA0   500 entries, 499 files
```

ARM9 entry is `0x02004800`, i.e. load + 0x800 — the first 2 KB is the KEY1
secure area. In this dump its `encryObj` marker has been replaced by two
`0xE7FFDEFF` words, which is what the firmware writes after verifying it; the
rest looks random because the whole module is BLZ-compressed.

### ARM9 is BLZ (BackwardsLZ) compressed

`ModuleParams.compressed_static_end` = `0x02059DD8` = exactly
`arm9_load + arm9_size`, which confirms compression. For ARM7 the same field is
`0`, i.e. not compressed. Decompresses cleanly: `0x55DD8 → 0x84820`, and the
result disassembles as valid ARM from `+0x800` onward.

The decoder (`tools/ndsx.py`), footer layout `[-8..-5]=enc_len`, `[-5]=hdr_len`,
`[-4..]=inc_len`; the compressed region is stored reversed:

```python
def blz_decode(buf):
    pak_len = len(buf)
    inc = struct.unpack_from("<I", buf, pak_len - 4)[0]
    if inc == 0:
        return bytes(buf[:pak_len - 4])          # not compressed
    hdr = buf[pak_len - 5]
    enc = struct.unpack_from("<I", buf, pak_len - 8)[0] & 0xFFFFFF
    dec_len = pak_len - enc
    seg = bytes(buf[dec_len:dec_len + enc - hdr])[::-1]
    raw_len = dec_len + enc + inc

    raw = bytearray(raw_len)
    raw[:dec_len] = buf[:dec_len]
    p, r, mask, flags = 0, dec_len, 0, 0
    while r < raw_len:
        mask >>= 1
        if mask == 0:
            if p >= len(seg):
                break
            flags = seg[p]; p += 1; mask = 0x80
        if not flags & mask:
            if p >= len(seg):
                break
            raw[r] = seg[p]; p += 1; r += 1
        else:
            if p + 1 >= len(seg):
                break
            pos = (seg[p] << 8) | seg[p + 1]; p += 2
            n = min((pos >> 12) + 3, raw_len - r)
            pos = (pos & 0xFFF) + 3
            for _ in range(n):
                raw[r] = raw[r - pos]; r += 1
    tail = raw[dec_len:]; tail.reverse(); raw[dec_len:] = tail
    return bytes(raw)
```

---

## 2. Memory map — confirmed, and it is the most important finding

**Loading either module as one flat blob at its load address is wrong.** Both
have autoload blocks that the startup code copies elsewhere before running.

```
ARM9  (module 0x02004000 .. 0x02088820)
  STATIC     0x02004000..0x02088100   file[0x00000..0x84100]   bss to 0x02160AC0
  AUTOLOAD   0x01FF8000..0x01FF8660   file[0x84100..0x84760]   ARM9 ITCM
  AUTOLOAD   0x02FE0000..0x02FE00A0   file[0x84760..0x84800]   ARM9 DTCM
  DESCRIPTORS 0x02088800..0x02088820  (2 x 16 bytes)
  OVERLAY 0  0x02160AC0..0x02162440   6528 bytes

ARM7  (module 0x02380000 .. 0x023A8F84)
  STATIC     0x02380000..0x02380644   file[0x00000..0x00644]
  AUTOLOAD   0x037F8000..0x03809414   file[0x00644..0x11A58]   70676 B  WRAM
  AUTOLOAD   0x02FE0000..0x02FF750C   file[0x11A58..0x28F64]   95500 B  main RAM mirror
  DESCRIPTORS 0x023A8F64..0x023A8F84  (2 x 16 bytes)
```

So the ARM7 static section is only 0x644 bytes; **97% of the ARM7 code lives at
two other addresses.**

Note `0x02FE0000` appears in *both* maps and means different things: on the ARM9
it is private DTCM, on the ARM7 it is a mirror of main RAM (`& 0x3FFFFF` →
`0x023E0000`). The two CPUs do not share an address space and must be modelled
separately.

### The descriptors are 16 bytes, not the usual 12

Widely documented `ModuleParams` says the autoload list holds 12-byte
`{dst, size, bss_size}` entries. Here they are 16 bytes:

```
{u32 dst, u32 data_size, u32 bss_dst, u32 bss_size}
```

Reading them as 12 bytes yields garbage (`bss_size = 0x01FF8000`). The 16-byte
reading is confirmed arithmetically — payload sizes account for the module
exactly, with nothing left over:

```
ARM9:  0x660 + 0x0A0  = 0x700   = 0x84800 - 0x84100   OK
ARM7: 0x11414 + 0x1750C = 0x28920 = 0x28F64 - 0x00644  OK
```

`tools/ndsx.py` therefore does not trust the list pointers at all. It searches
for the descriptor count whose payload sizes close the gap exactly, and raises
if none does:

```python
for n in range(1, 33):
    ls = len(b) - 16 * n
    ents = [(u32(b, ls + 16*i), u32(b, ls + 16*i + 4),
             u32(b, ls + 16*i + 8), u32(b, ls + 16*i + 12)) for i in range(n)]
    if sum(e[1] for e in ents) == ls - (data_start - load):
        ...                      # found it
raise ValueError("no consistent autoload descriptor count found")
```

Only two `ModuleParams` fields turned out to be reliable — `static_bss_end`
(+0x0C) and `compressed_static_end` (+0x10). The two list-pointer words
(+0x00, +0x04) do not match the documented `{list_start, list_end}` order for
this ROM: +0x00 is the module end and +0x04 duplicates +0x08. The map above is
derived from the tail scan instead, not from those fields.

### Independent check that the map is right

`python3 tools/probe.py branchcheck` counts `bl` targets that land outside any
mapped range:

```
arm7.bin           @0x02380000   3336 bl,  3320 out of range (99.5%)   <- one flat base
arm7_037f8000.bin  @0x037f8000   1260 bl,    56 out of range ( 4.4%)   <- correct base
arm7_02fe0000.bin  @0x02fe0000   2060 bl,    13 out of range ( 0.6%)   <- correct base
```

The residual few percent is literal-pool data being disassembled as code, which
is expected from a linear sweep. A worked example: the `bl` at file+0x1930 looks
like it targets `0x01B742F4` (unmapped) at the flat base; at the correct base it
is `0x037F92EC → 0x02FEBD10`, inside the other autoload block.

**Consequence: import the six extracted blobs into Ghidra as separate memory
blocks at the addresses above. A single flat ARM7 image gives a ~99% wrong call
graph.**

### Second check: both CPUs boot under emulation

`python3 tools/emu.py smoke` builds a per-CPU Unicorn image and runs from reset:

```
arm9  entry 0x2004800: ran 2000 insns, pc=0x1ff8008   <- reached ITCM autoload block
arm7  entry 0x2380000: ran 2000 insns, pc=0x3800014   <- reached WRAM autoload block
```

Both land in autoloaded code, which only happens if the copy addresses are
right. (Unicorn/QEMU does not implement the ARM946E-S cp15 cache/TCM registers,
so the harness skips `mcr/mrc p15` — harmless, since ITCM/DTCM are backed by
plain RAM here.)

---

## 3. NitroFS — confirmed

499 files. All asset containers are stock Nintendo formats, which are documented
and already have open tooling:

| ext  | n   | what |
|------|-----|------|
| NCGR | 141 | character/tile graphics |
| NSCR | 111 | tilemap (screen) |
| NCLR |  82 | palette |
| NCER |  40 | OAM cell definitions |
| NANR |  40 | cell animation |
| NCBR |  28 | bitmap graphics |
| NFTR |   8 | fonts |
| dat  |  47 | songs, patches, UI strings |
| bin  |   1 | default save image |

Non-graphics files:

```
/data/ds10backup_080425.bin      262144   default save image (dated 2008-04-25)
/data/work                       262144   scratch
/data/demo/ds10demo00..07.dat    ~11-12K  demo songs (DS-10 set)
/data/demo/ds10pdemo00..03.dat    12268   demo songs (PLUS set)
/data/demo/ds10init.dat           12044   initial/empty song
/data/demo/ds10synth00..23.dat        63   24 factory synth patches
/data/demoSP/...                          duplicate init + PLUS demos
/data/str/strdata_{eng,engna,fre,ger,ita,spa}.dat   UI strings
```

`ds10init.dat` at 12044 bytes and the demos at 12044/12268 give the song
document size directly. **A 63-byte patch and a ~12 KB song are both small
enough to map exhaustively.**

### GUI screen inventory

`/data/mainbg/*.NSCR` names the screens, so the UI scope is enumerable up front
(65 layouts, the functional ones being):

```
01_MEIN_MENU  15_OPTION  30_MIXER  31_SONG  32_PATTERN  33_DRUM_SELECTOR
51_ALL_DRUMS  52_FX  53_DRUMEDIT  54_DRUM_FX  55_DRUMEDITMAP
60_SYN_SEQ_{GATE,KAOSS,PAN,PIANO,VOL}  61_SYN_EDIT  62_DRUM_SEQ_GATE
63_DRUM_EDIT_PATCH  70_PATCH  71_KAOSS  81_KEY  90_MAP
```

plus title/credits/ad screens (`AD0_TITLE`, `AD1_KORG`, `CREDITS*`) and error
dialogs. The six `60_SYN_SEQ_*` variants confirm the step sequencer has per-step
gate, pan, volume, piano-roll and KAOSS lanes.

Strings `DRY/WET` and `L/R RATIO` in ARM9 confirm the FX section has those
parameters.

---

## 4. Patch format — partially mapped

24 files, 63 bytes each, identical structure:

```
      00                                              10          16
00 3a f7 c4 8f 68 85 cb 74 e8 53 58 0f 62 7c 71 f7 | 49 4e 49 54 08 08 | 00 x10
01 e2 0d 45 14 cc 19 d6 22 b9 97 d8 9b 33 0b b8 a1 | 42 41 53 53 31 08 | ...
02 77 a5 07 fb 3b 79 02 fa 51 2b 79 5f 48 aa bd 91 | 42 41 53 53 32 08 | ...
```

Distinct values per byte column across all 24 patches:

```
col  00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16..1f
n    23 21 24 22 22 24 22 24 24 23 22 24 22 21 23 24 11  8  9  9 12  6   1 (all)
col  20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37 38..3e
n     5  5  2  7  3  7  4  4 14 13  2 15 18  2 15  2  2  4 15 11  4 21 15 19 13  2 5 2 5 2 3
```

- `0x00..0x0F` — 16 bytes, maximum entropy across patches. Checksum, hash, or
  obfuscation. **Hypothesis**, and it matters: if it is a checksum the C code
  must reproduce it to write patches the original accepts.
- `0x10..0x15` — 6-byte name field. Padding is inconsistent — some patches pad
  with `0x08`, some with `0x00`, and `CPU\x00 1` has an embedded NUL followed by
  a real space. So treat it as 6 raw bytes, not a C string.
- `0x16..0x1F` — constant zero in all 24. Reserved or unused.
- `0x20..0x3E` — 31 bytes of actual parameters. Byte `0x20` takes 5 distinct
  values and `0x26`/`0x27` take 4 — plausibly waveform selectors. Columns
  `0x21..0x25`, `0x2A`, `0x2D`, `0x2F..0x31`, `0x34`, `0x39..0x3E` are zero in
  18–23 of the 24 patches, so most parameters sit at a default and the factory
  set exercises only a handful each. Useful for Tier 1: the sweeps will cover
  far more of the space than these 24 patches do.

### Parameter block fully decoded — bytes 0x20..0x3E are knobs 0..30

thement published a 31-entry knob table with names and ranges. Our patch block is
exactly 31 bytes wide, and **all 31 ranges are consistent with all 24 factory
patches** (`tools/probe.py knobs`). Two independent derivations agreeing:

| off | knob | name | range | enc |
|-----|------|------|-------|-----|
| 0x20 | 0 | octave | -2..2 | s8 |
| 0x21 | 1 | egint (pitch) | -63..63 | s8 |
| 0x22 | 2 | porta | 0..127 | u8 |
| 0x23 | 3 | pitch.in | -63..63 | s8 |
| 0x24 | 4 | pitch1.in | -63..63 | s8 |
| 0x25 | 5 | pitch2.in | -63..63 | s8 |
| 0x26 | 6 | vco1 waveform | 0..3 | u8 |
| 0x27 | 7 | vco2 waveform | 0..3 | u8 |
| 0x28 | 8 | vco2pitch | -63..63 | s8 |
| 0x29 | 9 | balance | 0..127 | u8 |
| 0x2A | 10 | vcosync | 0..1 | u8 |
| 0x2B | 11 | cutoff | 0..127 | u8 |
| 0x2C | 12 | peak | 0..127 | u8 |
| 0x2D | 13 | vcftype | 0..2 (0=LP,1=BP,2=HP) | u8 |
| 0x2E | 14 | egint (cutoff) | -63..63 | s8 |
| 0x2F | 15 | cutoff.in | -63..63 | s8 |
| 0x30 | 16 | vcaeg | 0..1 | u8 |
| 0x31 | 17 | vca.in | -63..63 | s8 |
| 0x32 | 18 | drive | 0..127 | u8 |
| 0x33 | 19 | level | 0..127 | u8 |
| 0x34 | 20 | attack | 0..127 | u8 |
| 0x35 | 21 | decay | 0..127 | u8 |
| 0x36 | 22 | sustain | 0..127 | u8 |
| 0x37 | 23 | release | 0..127 | u8 |
| 0x38 | 24 | mg.freq (LFO) | 0..127 | u8 |
| 0x39 | 25 | mg.bpm sync | 0..1 | u8 |
| 0x3A | 26 | pitch.in source | 0..6 | u8 |
| 0x3B | 27 | pitch1.in source | 0..6 | u8 |
| 0x3C | 28 | pitch2.in source | 0..6 | u8 |
| 0x3D | 29 | cutoff.in source | 0..6 | u8 |
| 0x3E | 30 | vca.in source | 0..6 | u8 |

Modulation sources (knobs 26–30): `0` none, `1` sine, `2` saw, `3` square,
`4` S&H, `5` EG, `6` VCO2. Only the bipolar knobs use signed encoding; the
factory set happens to keep several of them non-negative, so signedness per knob
comes from the range column, not from the data.

Confirmed independently by thement against DS-10 PLUS **save files**, where the
same structure appears from about `0x38000` with parameters likewise at `+0x20`.
So the patch record is the same on cartridge and in saves.

### Correction: the drum parts are resampled, not live-synthesised

The percussion patches use the same 63-byte format, which initially looked like
evidence that drums are the same virtual-analog voice. They are not, quite. The
drum voices play a **precomputed 0x8000-sample buffer** (`0x8000` = 32768 = one
second at 32768 Hz) resampled by a play-speed field, with **linear
interpolation** — the renderer is at `0x02072354` *(thement)*:

```
DRUM  +0x10  samples to render (u16)     +0x38  offset
      +0x30  pointer to volblock         +0x40  sample length (= 0x8000)
      +0x34  play speed                  +0x44  sample buffer address
```

Trigger is `play_speed = 0x8000 * 2^((note-60)/12)` *(thement)*. So the shared
patch format is because a drum patch is **rendered once by the synth engine into
a one-second buffer**, then pitched by resampling. One engine, two playback paths.

Song format (~12 KB) not yet touched.

---

## 5. Where the code lives

### ARM7 = stock SDK sound driver + WiFi — confirmed

Hardware-register literals per module (`tools/ndsx.py regs`):

```
arm7: SOUNDCNT x1, SOUNDCNT+1 x5, SNDCAP0CNT x1, SNDCAP1CNT x1,
      IPCSYNC x1, IPCFIFOCNT x3,
      171 references to 0x4804xxx / 0x4808xxx / 0x484xxxx  <- WiFi MAC/RF
```

The WiFi register count dominates. Since multiplayer is out of scope, most of
the ARM7 is irrelevant to this project. Note there are **no** sound *channel*
register literals (`0x4000400 + ch*16`) — those are reached via a computed base,
which is what the stock NitroSDK driver does.

### ARM9 = GUI (hardware 3D) + the whole synth engine — confirmed

ARM9 references the 3D geometry engine heavily: `GXFIFO`, `MTX_MODE`,
`MTX_PUSH/POP/SCALE/TRANS/IDENTITY`, `POLYGON_ATTR`, `TEXIMAGE_PARAM`,
`PLTT_BASE`, `DIF_AMB`, `SHININESS`, `SWAP_BUFFERS`, `VIEWPORT`. Combined with
`/data/mainobj/3D_TEX.NCBR` and `3D_MESS_TEX.NCBR`, **part of the UI is drawn as
textured 3D quads, not just tiles.** (Careful: `0x40004xx` is the sound channel
block on the ARM7 but the 3D engine on the ARM9 — same addresses, different
hardware. The first pass of this recon misread these as sound registers.)

Multiply density is a weak signal but points the same way:

```
arm9.bin            1258 muls / 134337 lines  (0.94%)
arm7_037f8000.bin     99 muls /  17664 lines  (0.56%)
arm7_02fe0000.bin     78 muls /  23846 lines  (0.33%)
```

**The synth engine is on the ARM9.** thement's entire address map lands in ARM9
static (`0x0207xxxx`) and ARM9 ITCM (`0x01FF8xxx`), and their ITCM buffer
addresses fall just past the end of our loaded ITCM block — exactly where runtime
ITCM data should be. The ARM7 only receives finished frames.

### The DSP tables are u32, not u16

Our first scan looked for rising `u16` runs and reported six "unidentified curve
tables" that fit no clean model. That was an artifact: **the tables are `u32`**,
so a `u16` scan reads alternating low/high halves and manufactures false
structure. Read correctly, three of them are unambiguous, and every one grows by
exactly `2^(1/12)` per entry — one entry per semitone:

| address | entries | width | what | check |
|---|---|---|---|---|
| `0x0207E950` | ~120 | u32 | pitch table *(thement)* | ratio `1.05946` = `2^(1/12)` holds for the first 119 steps |
| `0x0207ED38` | ~105 | u32 | drum pitch table *(thement)* | ratio `2^(1/12)` for 104 steps; entry 4 is exactly `1024`, i.e. unity playback speed |
| `0x020807C4` | **151** | u32 | filter coefficients *(thement)* | `2^(1/12)` while small, then saturates; entry 150 = `2147391259` ≈ `0x7FFFFFFF` and entry 151 is unrelated data — confirms the published count exactly |

Only the filter table's length is pinned. The two pitch tables' exact lengths are
not yet established: the semitone ratio stops holding after ~119 and ~104 steps
respectively, and for the drum table that happens well below saturation, so it is
probably a shorter table followed by other data rather than a curve that bends.
Worth nailing down, but not blocking.

The filter table's own departure from a constant ratio near the top *is*
saturation toward `0x7FFFFFFF` as cutoff approaches Nyquist, not a different
curve. That is also why our first tolerance-band detector stopped at entry 108.

The addresses our u16 scan reported (`0x02080400`, `0x0207EF44`, …) were spurious
and are withdrawn. Lesson for the rest of this project: **guess the element width
before fitting shapes.** `tools/probe.py tables` now reads u32.

### Voice topology — confirmed

Six voices, each rendering a 64-sample frame into its own ITCM buffer *(thement)*:

```
0x01FF87E0  synth table: 022489A0 02248FE0 02249620 02249900 02249BE0 02249EC0, null-terminated
0x01FF8820  buf syn1     0x01FF88A0  buf syn2     0x01FF8920  buf drum1
0x01FF89A0  buf drum2    0x01FF8A20  buf drum3    0x01FF8AA0  buf drum4
```

Buffer stride is `0x80` = 128 bytes = **64 samples of s16**, which independently
confirms the frame size. Frame = 64 samples = half of a double-buffered DMA
transfer.

Timing comes from the ARM7 over the IPC FIFO *(thement)*:

```
0x00004007   generate one sound frame
0x00004047   one sequencer tick
```

Multiply density, for what it is worth, pointed the same way before any of this
was known:

```
arm9.bin            1258 muls / 134337 lines  (0.94%)
arm7_037f8000.bin     99 muls /  17664 lines  (0.56%)
arm7_02fe0000.bin     78 muls /  23846 lines  (0.33%)
```

---

## 6. DSi-mode sections — not usable, and not needed

```
arm9i  rom 0x783000  load 0x02400000  size 0x002E74
arm7i  rom 0x787000  load 0x02E80000  size 0x0470F8
```

Both begin with the identical 24 bytes, and `dsi_total_rom_size` (+0x210) is
`0`. Identical heads for two different modules means these are encrypted
(modcrypt) or the DSi region was not properly dumped. Either way they are not
analysable as-is.

This is fine: unit code `0x02` means the DS-mode build is complete and
self-contained. **Target DS mode.** Revisit only if some PLUS feature turns out
to be DSi-exclusive.

---

## 7. Tooling status

Present and verified working:

```
arm-none-eabi-objdump    disassembly
python3 + unicorn 2.1.2  emulation oracle (smoke-tested, both CPUs boot)
Ghidra                   live via MCP, project "rev2"
SDL 1.2.68 + headers     libsdl1.2-dev - note this is sdl12-compat over SDL2,
                         not original SDL 1.2 (which ended at 1.2.15)
gcc, cmake, meson
```

Not installed, wanted later: a DS emulator for whole-system golden capture
(DeSmuME preferred — deterministic movie replay plus WAV recording).

---

## 8. Scripts

| file | what |
|------|------|
| `tools/ndsx.py` | header, NitroFS walk, BLZ decode, extraction, autoload map, patch dump, register classification |
| `tools/emu.py`  | per-CPU Unicorn image + function-call harness (the verification oracle) |
| `tools/probe.py`| branch-target validation, table detection/fitting, xref, multiply density |

```
python3 tools/ndsx.py header|fs|extract|autoload|patches|regs
python3 tools/emu.py  smoke | call <cpu> <addr> [args...]
python3 tools/probe.py branchcheck|monotonic|tables|xref <addr>|muls
```

The NitroFS walk, for reference:

```python
def walk(dir_id, path):
    ent = fnt + 8 * (dir_id & 0xFFF)
    p = fnt + u32(d, ent)
    fid = u16(d, ent + 4)
    while d[p]:
        t = d[p]; p += 1
        ln = t & 0x7F
        name = d[p:p + ln].decode(); p += ln
        if t & 0x80:                       # subdirectory
            sub = u16(d, p); p += 2
            walk(sub, path + name + "/")
        else:                              # file -> resolve via FAT
            start = u32(d, fat + 8 * fid)
            end = u32(d, fat + 8 * fid + 4)
            out.append((path + name, fid, start, end - start))
            fid += 1
walk(0xF000, "/")
```

---

## 9. Open questions

Resolved since the first pass: the render loop is on the **ARM9** (§5); the
tables are u32 and identified (§5); sample rate is **32768 Hz** with **64-sample
frames** (§5); the patch parameter block is **fully decoded** (§4).

Still open:

1. **The effects block.** thement's log ends with "attempt to make effects work
   didn't work very well". Struct offsets are known (§10) but the delay / chorus
   / flanger algorithms are not. This is genuinely unreversed.
2. **The mixer**, likewise: offsets known, behaviour not. Includes the
   unexplained `0x4444` scaling factor in the pan law, and the odd pan
   approximation `left = 0.533*(1-bal^2)`, `right = 0.533*(1-(1-bal)^2)` where
   `[cos, sin]` would be expected.
3. **Drum knob handlers** — never analysed.
4. Are patch bytes `0x00..0x0F` a checksum? Needed only to write patches the
   original will load, not to read them.
5. Song/pattern layout in the 12044-byte `ds10init.dat`. Diffing against the
   demo songs should expose most of it. Also: "what is stored in patch saves
   other than the synth configuration?"
6. Output stage: the DAC is documented as 16-bit but thement suspects filtered
   PWM, "12 bits tops". Affects only end-to-end comparison, not the engine.

---

## 10. Address and structure reference *(thement, verified same build)*

Functions:

```
0x02074F98  head of synthesis loop         0x02072354  drum sampler (linear interp)
0x02075410  store output of one synth      0x020722B8  drum render handler
0x0207546C  synth event dispatch           0x02073150  mixer + effect code
0x02075648  parameter (knob) set           0x02071FAC  set note
0x02075650  knob dispatch table            0x020758EC  drive knob handler
0x01FF846C  next-machine dispatcher (ITCM) 0x02075AD4  recompute LFO from BPM
```

Method tables — slot layout is shared across machine types:

```
0x02087CEC  synth   (+0x0C render, +0x18 knob move, +0x28 noteon)
0x02087C7C  mixer
0x02087BCC  drum
```

Live instances:

```
0x022489A0 syn1   0x02248FE0 syn2   0x02249620 drum1  0x02249900 drum2
0x02249BE0 drum3  0x02249EC0 drum4  0x0224A1A0 mixer  0x0224A480 effects
0x0226A580 delay buffer   0x02248680 output soundbuf   0x021B04A0 precomputed sample
```

Every machine has `+0x08` = pending event count, `+0x80` = event area (mixer uses
`+0xC0`). Events are 8 bytes; writing the count is the trigger.

Key struct offsets:

```
SYN     +0x30 env  +0x34 LFO  +0x3C pitchctrl  +0x40 OSC  +0x44 filter  +0x48 AMP
OSC     +0x2C vco1 type  +0x30 vco2 type  +0x34 balance  +0x38/+0x3C vco volumes
FILTER  +0x1C cutoff  +0x20/+0x24 peak  +0x28 coef  +0x34 highpass  +0x38 lowpass
        +0x40 type (0=LP 1=BP 2=HP)
AMP     +0x08 env segment  +0x0C extra distortion  +0x18 level (knob*0x20408)
        +0x38 drive coef 1 = ((knob*0x102)^2)>>15   +0x3C drive coef 2 = 0x7FFF-knob*0xE8
EFFECT  +0x0C type (0 delay 1 chorus 2 flanger)  +0x10 delay time
        +0x12 L/R ratio  +0x14 LFO  +0x16 depth  +0x18 feedback  +0x1A dry/wet
```

Event opcodes (low byte of the first word):

```
01 noteoff   02 noteon (note in byte 7)   04 set bpm   05 lfo reset
18 reset env/lfo   19 stop   1C bar start   1D pattern change   1E drum shaping
mixer: cc 06 / vv 07 = set volume on channel;  cc 06 / pp 0A = set pan
```

---

## 11. Synth object graph and the working oracle — our own work

`tools/oracle.py` renders real DS-10 audio by executing the original ARM9 code.
Everything in this section was derived here, not from prior art.

### The synth is a C++ object with a heap-allocated sub-object graph

```
0x02075FF8  Synth::create()      alloc(0x54) then ctor; returns object in r0
0x02074360  Synth::Synth()       stores method table, builds the sub-objects
```

The object is **0x54 bytes**. The constructor allocates and constructs eight
sub-objects, which pins down the graph and the sizes that thement's offset list
only named:

| off | size | ctor | ctor args | role |
|---|---|---|---|---|
| +0x30 | 0x14 | `0x02075938` | — | ENV |
| +0x34 | 0x34 | `0x02075A18` | — | LFO |
| +0x38 | 0x14 | `0x02075C3C` | — | NOTECTRL |
| +0x3C | 0x58 | `0x02071F48` | env, lfo, notectrl | PITCHCTRL |
| +0x40 | 0xA0 | `0x02071D80` | pitchctrl | OSC |
| +0x44 | 0x64 | `0x02075E2C` | env, lfo, osc | FILTER |
| +0x48 | 0x40 | `0x02075C9C` | env, lfo | AMP |
| +0x4C | 0x10 | `0x02075B14` | pitchctrl, osc, filter, amp | render chain |

The constructor does **not** set `+0x04` (event queue), `+0x08` (event count),
`+0x10` (frame size) or `+0x1C`. Those are wired by the machine-setup code at
`0x02032398`, which creates all six voices into a context object. `+0x1C` is a
pointer **to** the output buffer pointer — render does
`ldr r1,[this,#0x1C] ; ldr r1,[r1]`.

### Allocator, and why a static image faults

`0x0202927C` / `0x020292A0` (new) and `0x020292C4` / `0x020292E8` (delete) are
thunks of identical shape that tail-call the real allocator:

```
ldr r1,[HEAP_GLOBAL] ; ldr r1,[r1,#20] ; mov r0,#0 ; bx <real>
```

with `HEAP_GLOBAL = 0x021238C8`, `alloc = 0x020186B0`, `free = 0x020187B8`. The
heap global is in bss, so in a static image it reads NULL and the thunk faults
reading `[0x14]`. The harness therefore stubs **both** the global and the real
allocator. Size arrives in `r2`, not `r0`, and the original rounds up to 64
bytes (`add r1, r7, #63`) — which is why the game's objects sit 0x40/0x80 apart.

### Oracle results — verified

- **Deterministic**: identical samples across runs. Required for tier 1.
- **Pitch correct**: with INIT (octave 0), the measured period tracks
  `440 * 2^((note-69)/12)` at 32768 Hz to within measurement resolution
  (0.11–0.20%, which is one sample of zero-crossing quantisation):

  | note | expected period | measured |
  |---|---|---|
  | 48 | 250.50 | 250.00 |
  | 60 | 125.25 | 125.00 |
  | 67 | 83.59 | 83.50 |
  | 72 | 62.62 | 62.55 |

- **`0x02075410` writes exactly 64 contiguous halfwords** to the output buffer,
  confirming both thement's label for it and the 64-sample frame.
- The **hardware divider is genuinely used** — 26 writes to `0x040002xx` per
  frame. Without the `DIVCNT` model the output would be silently wrong.

### VCO waveform enum — measured, not assumed

Sweeping knob 6 with INIT and balance forced to VCO1, over one full period:

| value | rising | falling | flat | jumps | distinct slopes | waveform |
|---|---|---|---|---|---|---|
| 0 | 63 | 62 | 0 | 1 | 2 (+430/-490) | **triangle** |
| 1 | 3 | 122 | 0 | 3 | 1 (-230) | **sawtooth** |
| 2 | 2 | 4 | 119 | 3 | 1 (0) | **square** |
| 3 | 61 | 64 | 0 | 116 | 9 | **noise** |

So `0=triangle, 1=saw, 2=square, 3=noise`. Note this is a *different* enum from
the modulation-source list for knobs 26–30 (`1=sine, 2=saw, 3=square, 4=S&H,
5=EG, 6=VCO2`) — do not conflate them.

---

Behavioural notes that matter for a bit-exact reimplementation:

- **Envelopes are computed once per frame, not per sample.**
- The engine uses the DS **hardware divider** (`DIVCNT`, `0x04000280`) and square
  root accelerator. Any emulation harness must implement them or the output is
  wrong — thement hit this twice.
- Their translator bugs are a free list of traps for our C code: ASR without sign
  extension, `SMULL` without sign extension on the second operand, and `STRH Rn,
  [Rm], #2` post- vs pre-increment.

---

## 12. Mixer and effects — our own work, and previously unreversed

thement's log ends with "attempt to make effects work didn't work very well".
This section is why: the effect's parameter mappings sit behind a vtable
indirection, so reading the setters linearly gets you nowhere. Structure below
is decompiled from `0x02072C88` (mixer ctor), `0x02072E8C` (mixer render),
`0x0207251C` (effect ctor) and `0x020726D4` (effect parameter setter).

### The machine factories

All five "machines" are built the same way — `alloc(size)` then constructor:

| factory | size | ctor | machine |
|---|---|---|---|
| `0x02075FF8` | 0x54 | `0x02074360` | synth voice |
| `0x02076014` | 0x48 | `0x02072194` | drum voice |
| `0x02076030` | 0x8C | `0x02072C88` | mixer |
| `0x0207604C` | 0x40 | `0x02073890` | effect (parameter front end) |
| `0x02076068` | 0xE0 | — | not yet identified |

### Mixer object, 140 bytes

```
+0x00  method table (0x02087C7C)
+0x08  pending event count          +0x10  sample count (short)
+0x18  -> voice buffer array, 6 entries at 8-byte stride
+0x1C  -> {int16_t *L, int16_t *R} output pointer pair
+0x30  master gain, ramped (0x7000 from the ctor)
+0x38 + 4*ch   pan    (6 channels)
+0x50 + 4*ch   volume (6 channels)
+0x68 + 4*ch   pan*volume >> 15, recomputed each frame
+0x80  -> effect processor (the 168-byte object below)
+0x84  effect routing, 0..5
+0x88  mute flag (short)            +0xC0  event queue
```

Channel order is `0 = syn1, 1 = syn2, 2..5 = drums`.

**Panning is linear, not equal-power.** Per channel per sample, with
`P = pan*volume>>15`:

```
right = in * P >> 15
left  = in * (volume - P) >> 15
```

**Routing** (`+0x84`) selects what the effect sees; everything else is summed
dry into the output. Confirms thement's list:
`0 syn1, 1 syn2, 2 syn1+syn2, 3 drums, 4 all, 5 off`.

**The master gain at `+0x30` is a click-free mute ramp**, and answers
thement's "used by mixer, volume of sorts, (?)". Per sample:

```
gain = (gain * 0x7F35 >> 15) + (muted ? 0 : 0xCB)
```

`0x7F35` is `0x7FFF - 0xCA`, i.e. a one-pole smoother with coefficient
0.99429. It decays toward zero when muted and rises when not, and scales the
final output. The mute is therefore a ~200-sample fade, not a hard gate.

### Effect processor, 168 bytes — one DSP, three strategies

The design that defeated the earlier attempt: the constructor allocates **three
4-byte strategy objects**, one per effect type, and the type field selects
between them by indexing the object itself:

```
+0x00  delay strategy    vtable 0x02087C54
+0x04  chorus strategy   vtable 0x02087C2C
+0x08  flanger strategy  vtable 0x02087C04
+0x0C  type (short), 0..2 -- also the index: *(void**)(fx + type*4)
```

The parameter setter `0x020726D4(fx, param, value)` stores the raw knob and then
dispatches through the selected strategy's vtable:

| param | stores | vtable slot | writes to | delay | chorus | flanger |
|---|---|---|---|---|---|---|
| 1 | `+0x0C` type | — | — | resets the DSP | | |
| 2 | `+0x10` time | `+0x08` | `+0x50` | `0x02072ACC` | `0x02072AB8` | `0x02072A34` |
| 3 | `+0x12` L/R ratio | `+0x0C` | `+0x98` | `0x02072C50` | `0x02072A9C` | `0x02072A20` |
| 4 | `+0x14` LFO rate | `+0x10` | `+0x44` | `0x020728B0` (shared) | | |
| 5 | `+0x16` depth | `+0x14` | `+0x48` | `0x02072C44` | `0x02072A68` | `0x020729EC` |
| 6 | `+0x0E` delay sync | — | — | type 0 only | | |
| 7 | `+0x18` feedback | `+0x1C` | `+0x4C` | `0x02072C20` | `0x02072A5C` | `0x020729D0` |
| 8 | `+0x1A` dry/wet | `+0x18` | `+0x8C` | `0x020728C4` (shared) | | |

LFO rate and dry/wet are shared across all three types; the other four differ.
So **the three effects are one DSP with three parameter mappings** — 14 distinct
small functions in total, all still to be transcribed.

### The shared DSP

Two **65536-sample** ring buffers (`0x20000` bytes each, allocated at `+0x20`
and `+0x24`), a fractionally-interpolated read, feedback, and dry/wet:

```
w  = fx[0x94] & 0xFFFF                       /* write index */
rL = (w - (fx[0x5C] >> 15)) & 0xFFFF         /* integer part of the delay */
dL = bufL[rL] + ((fx[0x5C] & 0x7FFF) * (bufL[(rL-1) & 0xFFFF] - bufL[rL]) >> 15)
bufL[w] = clamp16(inL + (dL * fx[0x74] >> 15))    /* input + feedback */
outL = bypassL + ((inL * fx[0x8C] + dL * fx[0x90]) >> 15)   /* dry + wet */
fx[0x94] = w + 1
fx[0x5C] += fx[0x64]                         /* delay time ramps */
fx[0x74] += fx[0x7C]                         /* feedback ramps  */
```

Right channel identical with `+0x68`/`+0x80`/`+0x24`. Both the delay time and
the feedback are ramped per sample from per-frame targets, the same scheme the
filter and amp use — so sweeping delay time does not zipper.

Delay time is 15.15 fixed point: the integer part indexes the buffer and the low
15 bits interpolate. The read offset target has a floor of `0x8000`, i.e. one
sample minimum. Chorus and flanger get their movement by modulating that offset
with a squared LFO contour (`+0x2C`/`+0x30` and `+0x38`/`+0x3C`), which is why
they share the delay's code path entirely.

### Status

Structure and data flow: **decoded**. Parameter mappings: **implemented and
bit-exact** (`src/core/fx.c`, 489 parameter sets x 17 fields, 0 mismatches).
Not yet done: the DSP itself (`ds10_fx_update` / `ds10_fx_step` are written but
untested, since testing them needs a mixer harness).

Mapping facts worth recording:

- Only the delay derives its time from the knob. Chorus is a fixed 228.5-sample
  delay, flanger a fixed 4.63-sample one; all their movement comes from LFO
  modulation of the read offset. That is the entire difference between the three
  effects.
- Delay time free-running is `knob^2 * 0x103BA`, so knob 1 is 2 samples and knob
  127 is 32727 - just under a second, and just inside the 65536-sample buffer.
- The tempo-sync divisor is the **tempo** at `+0xA0`, not the sync flag. The
  dispatcher passes `fx->tempo` when sync is on and literal `0` when off, so
  sync enabled with no tempo event yet still takes the free-running path.
- Dry/wet uses the same quadratic curve as the oscillator balance law, minus its
  0x4444 gain.
- Chorus has feedback disabled twice over: its mapping returns 0 *and* the
  type setter clears the DSP's enable flag at `+0x1C`.
- Flanger feedback is negative, from a table indexed from its centre
  (`0x0207EFC2` = index 63 of the table at `0x0207EF44`) so the knob can be
  bipolar. The DSP is
straightforward now that the shape is known; the mappings are the remaining
work, and they are all small.

A test will not need the 256 KB of delay memory in its vectors — zeroing both
ring buffers on both sides and running N frames keeps them in lockstep.

---

## 13. Sequencer — decompiled

Prior art skipped this entirely ("sequencer - don't care"). Decompiled from
`0x0206D79C` (ctor), `0x0206D830` (config), `0x0206DB14` (swing),
`0x0206E5AC` (tick), `0x0206E8C8` (gate/note emission), `0x0206E1EC`
(automation lanes) and `0x0207025C` (event router).

### The machine graph

`0x0203235C` builds everything into one context object and wires it up:

```
context +0x274 + 4*i   synth voices   (count at +0x33C)
        +0x284 + 4*i   drum voices    (count at +0x340)
        +0x2A8 / +0x2AC  two mixers; +0x2A4 is the active one
        +0x2B0         effect front end
        +0x26C         SEQUENCER, 0x214 bytes
        +0x144 / +0x1D8  two event routers
```

### Event router, `0x0207025C`

The single path from sequencer to machines, and it confirms the queue layout
from the other side: queue pointer at machine `+0x04`, pending count byte at
`+0x08`, 8-byte events appended at `queue + count*8`.

The target byte is a bitmask, not an index:

```
0x20  route into the machine array
0x10  ...and broadcast to all of them rather than one
0x40  also route to router[1] (the mixer)
0x80  also route to router[2] (second bus)
0x0F  channel index - also written into event byte 1
```

Event layout: byte 0 opcode, byte 1 channel (filled in by the router), bytes
4..7 the payload. So `0x70` means "all machines plus the mixer", which is how
transport events reach everything.

### Sequencer object, 532 bytes

```
+0x0C  transport: 1 = running        +0x0D  start latch
+0x10  tempo BPM                     +0x14  period = 60000000 / tempo
+0x18  accumulator                   +0x1C  increment (240000)
+0x20  sub-tick counter              +0x24  swing percent (50 = straight)
+0x28  monotonic step counter        +0x2C  steps per pattern (16)
+0x2D  current step                  +0x2E  countdown to next step
+0x30 / +0x31  sub-ticks for even / odd steps
+0x32  current step's count          +0x33  automation sub-step
+0x34  event router                  +0x3C  song-player callback (self by default)
+0x40  current pattern (0 = none)    +0x44  previous pattern
+0x48  bar counter                   +0x4C / +0x50  synth / drum counts
+0x54  total tracks                  +0x64 + 0x24*i  per-track state, 36 bytes
```

**Timing.** `period = 60000000 / tempo`. Each tick call adds `incr` to the
accumulator and fires a sub-tick per `period` crossed, so one call can produce
several sub-ticks. **24 sub-ticks per step pair**, split by swing as
`even = (swing*24 + 50)/100`, `odd = 24 - even` — 50% gives 12/12, and the
extremes give 0/24 and 24/0. Verified for all 300 tempos and all 101 swing
values (`src/core/seq.c`, `make test-seq`).

**Song-player callback.** The tick does not know about song structure. It writes
a message id and argument into the callback object and calls its vtable slot 1:

```
cb +0x00  vtable, slot 1 is the handler, called as handler(cb, seq)
   +0x04  message id
   +0x08  argument in, answer out
```

The ids are u32 constants that read as FourCCs when written in hex:

```
0x73746570  "step"  before each step advance, arg = the step about to be left
0x65647470  "edtp"  end of pattern, arg = bar; the pattern is then dropped
0x73747074  "stpt"  bar start, arg 0; the ANSWER is the next pattern pointer
0x726E7470  "rntp"  sent after a pattern is installed, arg = bar
```

**Correction.** An earlier note here said a non-zero answer to the bar message
stops the transport. It does not - the answer *is* the next pattern, and the
sequencer installs it:

```
if (cb->answer) {
    set_pattern(seq, cb->answer);            /* 0x0206DBE4 */
    cb->msg = "rntp"; cb->arg = seq->bar; cb->handler(cb, seq);
    post(seq->router, 28, 0x70, seq->tempo); /* 0x1C BAR_START, broadcast */
}
```

An answer of zero leaves the sequencer with no pattern, which is the
already-verified idle path. So the seam between sequencer and song format is one
pointer per bar, and the chain walk lives entirely on the song-player side.

**`set_pattern`, `0x0206DBE4`.** Runs with interrupts masked. Clears the old
pattern's back-pointer, stores the new one, and then:

```
pattern +0x006  u8   steps in this pattern -> seq +0x2C
        +0x008  u32  <- seq +0x4C  synth count
        +0x00C  u32  <- seq +0x50  drum count
        +0x010  u32  <- seq +0x54  track count
        +0x014  ptr  per-track automation defaults, +4 per track
        +0x024  step records, 4 bytes, + track*0x68 + step*4
        +0x064  automation bytes, 2 per step, + track*0x68 + step*2
        +0x508  u32  <- seq, back-pointer; cleared when the pattern is dropped
```

With a NULL pattern it resets `steps` to 16 and posts to target 16. With a
pattern that differs from the previous one it broadcasts `0x1D`
PATTERN_CHANGE carrying the tempo, then re-initialises every track
(`0x0206DFCC`), a global (`0x0206EC10`) and both automation lanes of every synth
track (`0x0206DEE4`). So a bar boundary is where tempo and per-pattern settings
propagate to the machines.

**Per-step velocity and pan.** The 16-bit value in a step record is two bytes,
and when a track has a note the tick posts both to the mixer (`0x0206EB44` and
`0x0206EBB0`):

```
volume: (velocity * chan[idx]) / 127, truncated to int8   -> knob 7
pan:    clamp(pan + chan[idx+1] - 0x40, 0, 127)           -> knob 10
```

Both go out as opcode 6 to target `track | 0x40`, which the router sends to the
mixer with the channel in the low nibble. `chan` is the byte array at seq
`+0x60`, indexed through a two-byte-per-track table at `0x02160808`.

Note that both router wrappers (`0x02070204`, `0x0207022C`) build the 8-byte
event on the stack and leave bytes 2..3 uninitialised, so those bytes are
whatever was on the stack. Only bytes 0, 1 and 4..7 carry anything.

**Per-track mode bytes.** Inside the 36-byte track block at `seq + 0x64 +
track*0x24`:

```
+0x00..0x0F  note and gate state (+0x05 note, +0x06 velocity, +0x07 pan,
             +0x0C live gate, +0x0D live note)
+0x10..0x1F  two 8-byte automation lanes
+0x20        automation enable: 1 = per-step, 2 = a second path
+0x21        automation mode, 0..3
+0x22        play mode: 0 = play the pattern, 1 = muted,
             2 = play only if no live note, 3 = record (writes the live
             note and gate back into the pattern)
```

### Pattern data

Per track, stride `0x68`. Step records are 4 bytes at `pattern + track*0x68 +
step*4 + 0x24`:

```
byte 0  note, negative means no note
byte 1  gate length
bytes 2..3  a 16-bit per-step value
```

Automation lanes are read at `pattern + track*0x68 + step*2 + 0x64`, one signed
byte per lane, the two lanes adjacent. Confirmed from the raw ARM at
`0x0206E250`, so the address arithmetic is not in doubt:

```
ldr  r1, [r9, #0x40]     ; pattern
mov  r0, #104            ; 0x68
mla  r0, r5, r0, r1      ; pattern + track*0x68
ldrb r1, [r9, #0x2d]     ; step
add  r0, r0, r1, lsl #1  ; + step*2
ldrsb r1, [r0, #0x64]    ; lane 0
ldrsb r0, [r0, #0x65]    ; lane 1
```

**Resolved — the earlier "overlap" was my own mis-grouping.** `track*0x68 + 0x24`
is `0x24 + track*0x68`: the `0x24` is the **pattern header size**, not a
per-track offset. Regrouping, the layout is consistent:

```
pattern +0x00..0x23              36-byte pattern header
pattern +0x24 + track*0x68       per-track block, 104 bytes:
          block +0x00..0x3F        16 steps x 4 bytes  (note, gate, u16)
          block +0x40..0x5F        16 steps x 2 bytes  (two automation lanes)
          block +0x60..0x67        8 bytes spare
```

Both arrays fit inside the 104-byte stride with room over, and it agrees with
the song file: 6 tracks x 104 = 624, plus the 36-byte header = 660, inside the
676-byte record the file diff showed. The automation loop runs over
`seq[0x4C]` (synth count) rather than `seq[0x54]`, so only the two synth tracks
have lanes; the step-record loop runs over all six.

### Note emission, `0x0206E8C8`

Per track, a gate countdown (`0x0206EABC`) returns 1 (note off), 2 (note on) or
3 (both, i.e. retrigger). Targets are `track | 0x20` for note-off and
`track | 0xA0` for note-on — so note-ons also go to the second bus. The payload
is the note in the low byte with `0x7F` above it.

### The tempo has to reach the machines

`0x0206DA90` is the sequencer's tempo setter. It clamps to **10..250** and, when
the value changes, broadcasts **opcode 4 to target `0x70` - every machine and the
mixer** - before deriving `period = 60000000 / tempo`.

That broadcast is the only thing that tells a *voice* the tempo. A voice does not
read it from the sequencer, and neither the `0x1C` bar line nor the `0x1D` pattern
change sets it, even though both carry the tempo in their payload. Nothing else
sends opcode 4.

It matters because a BPM-synced LFO's increment is `f(rate_knob) * lfo.bpm`. Miss
the broadcast and every synced LFO runs at whatever tempo the constructor left,
which is 120 - so at 87 BPM it runs 1.38x too fast, and at 165 BPM 0.73x. On a
patch with the LFO routed to pitch that is audibly wrong pitch movement, which is
how it was spotted.

### Automation lanes, `0x0206E1EC`

Two lanes per track, 8 bytes each at `track + 0x10 + lane*8`: knob id, value
(`-1` means take the pattern default), offset, scale, then a current value and a
per-sub-tick delta. These do **not** post events — they call the machine's knob
handler directly through vtable slot `+0x18`, which for a synth is the same
`0x02075648` the patch loader uses. Track mode `+0x20` selects between held and
interpolated, and `+0x22` selects between play, record and overdub.

### Note and gate logic, `0x0206E9D8` and `0x0206EABC`

The step trigger and the per-sub-tick countdown, both verified bit-exact.
Track state is the first six bytes of the block: sounding flag, pending event,
gate countdown, gate mode, note.

Gate values are special-cased rather than being a plain length, and the six a
song file uses are exactly the six the interface offers. Sub-tick counts are for
a straight step of 12:

| gate | interface | count | behaviour |
|---|---|---|---|
| `0` | 0% | 0 | released on the sub-tick it started |
| `1`..`3` | 25%, 50%, 75% | 3, 6, 9 | `gate * subticks / 4` |
| `4` | 100% | 11 | a sub-tick short, so the next note articulates |
| `5` | TIE | 13 | outlives the step, so an identical next step is held rather than restarted |
| `6` | (drums) | 0 | retriggers, and never auto-releases: the sample plays out even if the following steps are empty |
| `0xFF` | none | 0 | sounds until an empty step releases it |

Measured: gate `6` is on 12691 drum steps and no synth step in the ROM or in the
108 saved songs; `0xFF` occurs in none of them, and the interface has no button
for it. So the tie a song actually contains is gate `5` plus a repeat of the same
step, not `0xFF` - see `docs/songtext.md`.

Because the count is in sub-ticks and the sub-tick count per step comes from
swing, **gate length is swing-dependent** - the same gate value is a different
absolute duration on swung even and odd steps.

Two behaviours worth naming:

- A step whose note equals the note still inside its gate emits nothing, so
  repeated identical steps sustain rather than stutter.
- A step that lands while something is still sounding emits a retrigger (3)
  rather than a note-on, so the envelope restarts. A note-on only happens from
  silence.
- A no-note step releases a running tie but does nothing otherwise, since an
  ordinary note has already been released by its own gate.

### Status

Verified bit-exact (`src/core/seq.c`, 15457 checks, `make test-seq`):
transport, tempo mapping, swing mapping, accumulator, step advance, bar
counting, the step trigger and the gate countdown.

Also verified bit-exact: **event emission and router fan-out** — six tracks
driven through the real `0x0206E8C8` into a router with fake machines, checking
every queue entry's destination, opcode, channel byte and payload. A machine is
only a queue pointer at `+0x04` and a count byte at `+0x08`, which is what made
this testable without building the whole context object.

Also verified bit-exact: **automation lanes** and the **song file format**
(section 14).

Also verified bit-exact: **the pattern-driven tick**, by replaying a real demo
pattern (`src/core/seq.c`, 1781444 checks over 34 cases and 20244 routed events,
`make test-replay`). Each case installs one pattern out of one demo `.dat` into
the original sequencer at that song's own tempo and swing and calls the real tick
700 times - several bars - comparing the whole sequencer state, all six track
blocks and every routed event after every tick. Covered: the step record read,
all four play modes including the record write-back, the per-step velocity and
pan posts, the end-of-pattern drop, and the automation sub-step.

Two bugs in already-"verified" code came out of it, both invisible to the earlier
sequencer test because it ran with no pattern loaded and so never fed real step
records through:

- The gate byte is read **unsigned**. Reading it signed turns gate `0xFF` from a
  tie into a negative countdown.
- A note event's payload is `0x7F00 + note`, an **add**, not `note | 0x7F00`. The
  note is sign-extended first, so a negative note borrows: `0x7F00 + (-22)` is
  `0x7EEA`. Every demo note used so far was positive, which is why an or looked
  right.

### Rendering a song as audio

`src/app/songplay.c` renders a `.dat` to a WAV using nothing but the
reimplementation, and `make test-songrender` checks the result against the
original: the original's own sequencer wired to two of the original's own synth
voices through the original's own router, with the patches and step data of a
real demo pattern. **2188800 samples over 19 cases, 0 mismatches.**

The pattern's synth patches are what made this possible. `0x0206DFCC` runs on
every pattern change and pushes a per-track knob array into the machine one knob
at a time, and the synth machine reports 31 knobs, so a pattern's two patches are
the 62 bytes at `pattern +0x04`. All 544 patches in the ROM's songs land inside
the per-knob ranges `patch.c` already knew - 16864 range checks, no violations.

Two more not-zero-after-construction traps, both found by this test:

- The sequencer's accumulator starts at `period - incr + 1` for the default 120
  BPM, i.e. 260001, so the first tick call fires immediately. Starting from zero
  delays the first step by three frames.
- The envelope's retrigger byte at `+0x05` is not zero after construction, and
  without it a note-on glides instead of restarting the attack - which is silence
  when the envelope is already at zero. It was missing from the captured voice
  state, so `tests/test_voice.c` had been passing without it; the field lists and
  the loader now live in one place, `tests/voice_state.h`.

**Tick rate - derived, not observed.** The tick runs on its own thread
(`0x0206D604`) woken by an OS event this project has not traced. The rate is
pinned by arithmetic instead: the accumulator advances 240000 per call against a
period of `60000000/BPM`, and 12 sub-ticks make a step at 50% swing, so a
musically correct tempo needs exactly **200 calls per second** whatever the BPM.
`32768/200` is 163.84 samples, which the renderer hits exactly with 1600 per
64-sample frame against 4096 per tick. As a check, `ds10demo01` predicts 16560
sub-ticks over its 90 bars, or 164.29 s; the renderer produces 164.30 s.

### Drums

There is no separate drum synth. `0x0206CE04` renders each of the four drum
patches once into a 0x8000-sample buffer - one second at 32768 Hz - and hands it
to the drum voice with root note 60 (`0x02072268`), which then resamples it. One
engine, two playback paths.

The drum patches are the last part of the song file's first tail block, and
`0x0206B9DC` pins them exactly: it reads song record `+0x42AF4 + drum*31` and
pushes those 31 bytes into a machine one knob at a time - the same 31-knob space
as a synth patch - then reads nine more bytes at `+0x42B70 + drum*9` into a second
parameter space. Those are payload `+0x2B2C` and `+0x2BA8`, so the 160-byte block
is fully accounted for: `4*31 + 4*9 = 160`.

The demo patches are unmistakably percussive: attack 0, decay 14..56, sustain
mostly 0, level 127, and the snare-ish ones select the noise waveform.

Playback trigger, from `0x020724E4`: `speed = drumpitch[clamp(note - root + 64,
0, 127)]` over the table at `0x0207ED38`. Note-on also rewinds the sample to 0,
and restarts the envelope only if a note-off has been seen - the same legato trick
the synth envelope uses.

**Verified bit-exact including drums**: `make test-songrender` renders each song's
four drum patches through a real synth voice, hands each buffer to a real drum
voice, and compares the buffers themselves as well as all six channels every
frame. **7597568 samples over 19 cases, 0 mismatches.**

Getting there turned up a real bug in `patch.c`. Mod-source **6 is the other
oscillator** - audio-rate modulation the VCO, filter and amp do themselves rather
than through the modulation matrix - so knobs 26..30 have a side effect on top of
the routing:

```
dest 0..2 (pitch)  src 6 -> vco.xmod = dest + 1
                   src 0 -> vco.xmod = 0, but ONLY if it already equals dest + 1
dest 3 (filter)    vcf.osc_depth = (src == 6)
dest 4 (amp)       amp.dist      = (src == 6)
```

The pitch case is the awkward one: one field says which destination is
cross-modulated, so selecting an LFO on a cross-modulated destination leaves cross
modulation running, and clearing a *different* destination does nothing. Getting
that wrong silenced cross-modulation in four of the ROM's drum patches.

The LFO's BPM-sync flag and tempo copy also had to join the captured voice state -
the constructor leaves sync on at 120 BPM, and without them knob 24 picks the
free-running rate table instead of the synced one.

What the offline render does NOT reproduce: how the original drives it. That goes
through a recorder the sequencer plays into (`0x0206EEA0`), which is not
decompiled. Both sides here render one second from a freshly constructed voice at
note 60, so the buffer *content* is checked against the original but the driving
conditions are inferred.

### Mixer and effect

The mixer's knob dispatch, `0x020736C0`, is the whole of a song's mix:

```
0        route; also forces the effect fully dry when it is 5 (OFF)
1..8     the effect, forwarded to the fx setter with the SAME parameter id
9..14    pan, channels 0..5     value * 258              (linear)
15, 16   forwarded to the effect as well
17..22   volume, channels 0..5  value^2 * 0x10404 >> 15  (quadratic)
23, 24   nothing at all
```

Knob 8 is dry/wet: the value is stored at mixer `+0x34` either way but only
reaches the effect when the route is not OFF, so switching to OFF and back
restores it. Values arrive **signed** - the pushers read them with `ldrsb` - so
the effect's bipolar knobs see negatives.

A pattern carries all 25 as bytes at payload `+0x242`, and `0x0206EC10` pushes
them in on a pattern change. Route across the ROM's 272 patterns is only ever 0,
1, 2 or 4; pans cluster around centre and volumes near full, which is what
identified the block before the dispatch confirmed it.

**Mixer events**, `0x020735D0`:

```
4        the payload word is the BPM; retunes a tempo-synced delay
6        knob event: id 7 -> knob channel+17 (volume), id 10 -> channel+9 (pan)
20, 21   mute off, mute on
```

Everything else is ignored, so the sequencer's bar and pattern-change broadcasts
do not reach the mixer and the transport has to send opcode 4 itself.

That knob-event mapping is also why the sequencer indexes its channel array with
`17+track` and `9+track`: the array IS the mixer's knob array, so a step's
velocity and pan scale that channel's own knob rather than replacing it.

**Verified bit-exact end to end** (`make test-songrender`): the six channels
through a real mixer driven by the pattern's own knobs, **9299968 samples over 19
cases, 0 mismatches**. That covers song file to stereo - patches, sequencer, note
emission, routing, two synth voices, four drum voices and their one-second
buffers, the mixer and the effect.

### Automation lanes

Verified for the path a recorded song plays back through. What is established:

- A lane's **offset is the knob's minimum and its scale is the knob's range**
  (`max - min + 1`). `0x0206F538` builds a RAM table from the synth's knob
  descriptors at `0x0207F8E4` (38 bytes each, min at `+0x20`, max at `+0x22`) and
  `0x0206DEE4` reads offset from `+1` and scale from `+3` of that table. All 31
  agree with the ranges the patch work already had, so a recorded 0..127 lane
  sweeps exactly one knob's range. `ds10_lane_map` is already verified against
  this arithmetic.
- The **runtime track block** (104 bytes at `pattern + 0x24 + track*0x68`) is now
  fully accounted for:

```
+0x00..0x3F  16 steps x 4 bytes  note, gate, velocity, pan
+0x40..0x5F  16 steps x 2 bytes  the two lanes' recorded values
+0x64        lane 0 knob id, 0xFF = lane off
+0x65        lane 1 knob id
+0x66        automation enable -> track state +0x20; 1 and 2 are two paths
```

- The **knob dispatch** at the tail of `0x0206E1EC` is
  `machine->vtable[0x18](machine, lane->knob, lane->cur >> 8)` for each lane whose
  knob is not `0xFF`, and the processor ends by bumping the automation sub-step.
- In the file, a pattern's automation area is `+0x1C2 + track*64 + step*4` as
  `raw[2], knob[2]`, and the 73 bytes at `+0x25B` hold 12-byte per-track records
  from `+0x25C` whose bytes 2 and 3 are that track's two lane knob ids.

The processor branches on track state `+0x20` (1 or 2) and `+0x21` (0..3). The
path a recorded song plays back through is `+0x20 == 1` with `+0x21 == 0`: latch
both lanes on a step's first sub-tick, then push their held values into the
machine every sub-tick. That is implemented and **included in the bit-exact
render** - the lane values go into the original's own knob handler, and the 19
songrender cases still match sample for sample with automation enabled. The other
paths consume a pending value from live knob input, which is editing rather than
playback; a track asking for one is left un-automated.

Still open:

- How the original drives the offline drum render (`0x0206EEA0` plays into a
  recorder). Affects only the first samples of a buffer.
- The nine extra bytes per drum at payload `+0x2BA8`, and the parts of a
  pattern's 12-byte per-track records other than the knob ids.
- The sequencer tick rate, still derived from the accumulator arithmetic rather
  than observed.

Still decompiled but not verified, and all of it on the path to replaying a song
as audio rather than as events:

- `set_pattern` (`0x0206DBE4`) itself, though what it does is now reproduced:

```
steps and the automation sub-step are reset whichever pattern arrives, but
if (prev != pat) {
    prev = pat
    broadcast 0x1D PATTERN_CHANGE with the tempo   <- arms the voices' bar flags
    0x0206DFCC  reload every synth patch knob
    0x0206EC10  push all 25 mixer knobs
    0x0206DEE4  re-initialise both automation lanes of every synth track
}
```

  Two things about this were wrong and both were audible.

  **When.** It runs inside the tick, before the new pattern's first step is
  processed. Doing it after the tick returned silenced the first note of every
  song: the step's own volume event had already gone out, scaled against a mixer
  knob array that was still zero, so channel 0 was set to silence and stayed there
  until the next note. The array the sequencer scales against is the mixer's own
  live knob array, not a copy taken later.

  **The 0x1D broadcast.** Missing it left the voices' `bar_lfo` and `bar_env`
  flags unset, so the 0x1C bar line had nothing to consume and a BPM-synced LFO
  never re-phased - an LFO filter sweep drifted against the bar instead of
  restarting with it. Note the guard: only a pattern that differs from the
  previous one broadcasts, so a pattern repeating across bars deliberately does
  not re-phase.
- Automation modes other than per-step latch, and the knob dispatch at the tail
  of `0x0206E1EC` - which is `machine->vtable[0x18](machine, lane->knob,
  lane->cur >> 8)` for each lane whose knob is not `0xFF`.
- The song player itself - nothing walks the bar chain yet, so the replay loops
  one pattern.
- The unnamed regions of a pattern record, which is where the per-pattern synth,
  drum and mixer settings live. **This is what blocks audio.** Until those are
  named, neither side can set the machines up from a song file, so a replay can
  be compared as events but not as samples.

## 14. Song file format - decompiled and verified

`ds10*.dat` in `/data/demo` and a save-RAM slot are the same record. Save RAM
holds 18 of them at `save + 0x200 + slot*0x3000`, and the scanner at
`0x0205D088` walks all 18 checking each one.

### The container

```
+0x00  16-byte MD5 of everything after it
+0x10  payload: a byte image of the in-memory song record
```

The hash routine is `0x02015404`, checked by `0x0205D038` (hash the payload into
a stack buffer, compare 16 bytes). It is plain RFC 1321 MD5: a reference
implementation reproduces the header of all 17 song and patch files in the ROM.

### The save file around those slots

The 512 KiB save is the same idea three times over - hash first, and a valid hash
is what tells a written record from untouched flash. Measured across the six
saves in `songs/`:

```
0x00000   16  "DS10PSAVEDATA000"
0x00010   12  00 ff ff ff, then eight zero bytes
0x0001C   16  MD5 of the 28 bytes above. Identical in all six saves, so it
              covers nothing that a slot write changes
0x00100       18 song slots, stride 0x3000, 16 + 12252 each; on disk the run
              starts at 0x100, in RAM at 0x200
0x38000       96 patch slots, stride 0x40: the 63-byte record of §4 with one
              pad byte after it. 24 written in DS10PLUS.SAV, 0 in FRESH10.SAV
elsewhere     0xff
```

Nothing outside a slot refers to one - no count, no directory, no free list -
which is what makes writing a single slot a local edit. `tools/sav.py` does that:
every one of the 97 written slots across the six saves extracts and packs back to
a byte-identical save, and a cross-pack into another save's empty slot changes
exactly the slot's 12268 bytes.

A slot's payload is a fixed 12252 bytes with the hash over all of it, so only a
PLUS record fits one; the shorter DS-10 and pre-release lengths exist as files
but cannot be a slot.

Four payload lengths exist. The format only ever grew by appending, so an older
file is a prefix of a newer one - which is why the loader dispatches on size:

```
11212  pre-release demos (version word 0 or 0xFFFF)
11260  + 48
12028  DS-10, version 1
12252  DS-10 PLUS, version 2
```

### The payload is not marshalled

`0x0205D320` writes a slot as a handful of unrolled copies. Every one has the
same source and destination offset, so the file layout IS the struct layout:

```
payload   size   from
0x0000       4   record +0x0000
0x0004      32   record +0x0004
0x0024     200   record +0x0024     bar chain
0x00EC   10816   record +0x00EC     16 patterns x 676
0x2B2C     160   record +0x2B2C
0x2BCC      48   record +0x2BCC
0x2BFC     768   record +0x2BFC     end of the DS-10 payload, 0x2EFC
0x2EFC     168   record +0x2EFC     PLUS only
0x2FA4      56   record +0x2FA4     end of the PLUS payload, 0x2FDC
```

The blocks are separate only because the compiler unrolled each field group.
Nothing is reordered or packed, so a parser that copies the unnamed regions
verbatim round-trips exactly.

### Header

```
+0x00  u16  version    1 = DS-10, 2 = PLUS, 0 or 0xFFFF = pre-release
+0x02  u16  revision
+0x04  u32  tempo, BPM                 117..132 across the demos
+0x08  u16  12 from version 1 on, 16 before it
+0x0A  u16  swing percent              50 straight, up to 65 in the demos
+0x0C  8    zero in every file
+0x14  16   name, 6 characters shown, NUL padded
+0x24  200  bar chain, 100 u16 pattern indices, first 0xFFFF ends the song
```

Chain lengths in the demos run from 0 (`ds10init`) to 90 bars.

### Pattern, 676 bytes

The editor's live model and the file agree from the pattern base onwards but not
before it: the file has 0xEC bytes of header, the live model 0xB4. The accessors
therefore use displacements 0x38 higher than the file offsets - which is what
makes it possible to point them straight at a file image and have them read it
correctly, and that is how the layout below was verified rather than guessed.

```
+0x000  u32   steps in this pattern      6, 8, 10, 12 and 16 all occur
+0x004  0x3E  two 31-knob synth patches, one per synth track
+0x042  0x180 6 tracks x 16 steps x 4 bytes    (2 synth + 4 drum)
+0x1C2  0x080 2 lanes x 16 steps x 4 bytes     automation, synth tracks only
+0x242  0x019 the mixer's 25 knobs
+0x25B  0x049 a zero byte, then six 12-byte per-track editor records
```

The editor records are read by an accessor family at `0x0206B100`, one variant per
displacement, and the displacements it uses are `0x310`..`0x317` and `0x319` -
bytes 0..7 and 9 of the record. Byte 0 is the piano-roll scroll (0..84, and
`0x02034500` reaches it to set the scrollbar up), bytes 2 and 3 are a copy of the
two automation lanes' knob ids that agrees with the lane array in 94% of cases and
is not what playback reads, and the rest are read but not named. Bytes 8, 10 and
11 have no accessor and are zero in all 1744 patterns here.

Accessors, all leaf functions taking `(doc, pattern, track, step, out)` and
copying signed bytes:

```
0x02033F8C  get step        doc +0x40116 + pattern*676 + track*64 + step*4
0x02033FE4  get step count  doc +0x400D4 + pattern*676   (pattern from +0x40070)
0x020343B4  get automation  doc +0x40296 + pattern*676 + lane*64  + step*4
```

An automation record is `raw[2], knob[2]`, and `raw == -1` is the "no automation
here" case `ds10_lane_map` already handles.

A step record is **`gate, note, velocity, pan`** - and note that this is NOT the
order the sequencer's runtime pattern uses, which is note first. The accessor
above only copies the four bytes; it does not say which is which, so the order
came from the data:

```
byte 0  only ever 0..6 across all 26112 steps in the ROM: the gate codes
        (4 = full step, 5 = one over, 6 = retrigger)
byte 1  24..95 peaking hard at 60, plus 0xFF on 16910 steps - the "no note"
        marker. Drum steps sit on 60, which is the root note a drum buffer is
        rendered at, i.e. unity playback speed
byte 2  peaks at 127 - velocity
byte 3  peaks at 64 - centre pan
```

Having these two swapped is what a differential test cannot catch: both sides of
the comparison build the runtime pattern from my mapping, so they agree with each
other while disagreeing with the music. It turns every rest into a tie and takes
every pitch from the gate byte, which sounds like a drone with noise over it.
`tools/mksong.py` writes a synthetic song - one sawtooth, filter open, a major
scale up and down - as a listening check that pitches come out where they should.

The automation accessors fold the track index with `if (track >= 2) track -= 2`,
so the two lanes are shared between the synth track pair and whatever second
numbering the UI uses.

### Status

Verified bit-exact (`src/core/song.c`, 26858 checks, `make test-song`): the MD5
header on every file, every step record, every automation record and every
pattern length of all 13 distinct songs read back through the original's own
accessors, and a parse/serialise round trip that reproduces each file byte for
byte - which is what makes the parse total, since a field I could not name still
survives it.

### The trailing blocks, named

Two of the three are name fields, six per row at an 8-byte stride - 6 characters
then two pad bytes - in track order, 2 synth then 4 drums:

```
+0x2BCC   48   one row: the names as the running machines have them. Byte for
               byte the row for pattern 0 in all 104 songs long enough to hold
               both, and read at `0x0206BDDC`
+0x2BFC  768   16 rows, one per pattern
```

So a name in `/data/demo/ds10synth*.dat` and a name in a song are the same
6-byte field. Padding is `0x08` as often as NUL - and the interface's own font has
no glyph mapped at `0x08`, so it advances nothing, which is why a 6-character
field can end in it.

`+0x2BA8`, the nine bytes per drum, is **read**: `0x0206B9DC` pushes a drum's 31
knobs into its machine through `0x0206DF64`, then loops `j = 0..8` over
`record + 0x42B70 + drum*9` and pushes each byte through a second setter,
`0x0206E084`. So it is a second parameter set of a drum voice, not dead space -
what the nine mean is still open.

They **are** the `54_DRUM_FX` page, or at least they are a route and the eight
effect parameters. The path, traced end to end:

```
0x0206B9DC(song, drum, seq)   the 31 knobs through 0x0206DF64, then
  j = 0..8: 0x0206E084(seq, j, byte)
    if seq+0x40 (current pattern) is 0, or the dispatch table has no entry for j:
      router = seq+0x34; machine = router[+4]; machine.vtable[0x18](j, value)
```

The dispatch table is built at run time in BSS at `0x02160828` by the loop at
`0x0206D754`, which inverts a ROM table: for each channel `i` it writes
`dest[volume_id(i)] = (i, kind 1)` and `dest[pan_id(i)] = (i, kind 2)`. The ROM
table at `0x02087900` is the id list, and it is the mixer's own knob numbering -
`(17, 9)`, `(18, 10)`, `(19, 11)` … which is `volume = 17 + channel` and
`pan = 9 + channel`, exactly `mixer.h`. The two kinds are the per-step velocity
and pan appliers, `0x0206EB44` and `0x0206EBB0`: both fetch the pattern's mixer
knob through the pointer at `seq+0x60`, combine it with the track's latch at
`+0x6A` or `+0x6B` (a multiply for volume, an offset around 64 clamped to 0..127
for pan) and post opcode 6 with knob id 7 or 10 - `DS10_MIX_EV_KNOB` with
`EVK_VOL` and `EVK_PAN`.

And the push happens *inside the drum-buffer render*: the call site is
`0x0206CE10`, in `0x0206CE04`, which is what renders each drum patch into its
one-second buffer. So these are render-time settings - the sample is put through
the effect as it is written, at load and again whenever the drum patch is edited,
and the voice then resamples a buffer that already has the effect in it. That is
also why there is no contradiction in one effect processor and four stored sets:
each drum's set is pushed just before that drum is rendered.

Ids 1..8 have no entry in that table, so a drum's nine bytes take the fallback and
land as `setKnob(0..8)` on the machine at `router+4`. Knobs 0..8 of the mixer are
route, effect type, time, L/R ratio, LFO rate, depth, sync, feedback and dry/wet -
and the nine bytes match that slot for slot, over 388 drum records:

| j | mixer knob | drum bytes | the mixer's own |
|---|---|---|---|
| 0 | route | 0..2 | 0..5 |
| 1 | type | 0..2 | 0..1 |
| 2 | time | 0..106 | 0..127 |
| 3 | L/R ratio | 0..255, mostly 0 | 0..255 |
| 4 | LFO rate | 0..63 | 20..64 |
| 5 | depth | 60..115 | 43..104 |
| 6 | sync | 0..1, and equal to the mixer's on 71% of records | 0..1 |
| 7 | feedback | 0..255, -1 common | 0..249 |
| 8 | dry/wet | 0..127, 0 on 347 of 432 | 11..89 |

They are mostly *not* equal to the master effect's settings, which is what a
separate per-drum setting looks like rather than a mirror. There is still one
effect processor, so four stored sets do not mean four effects running: whichever
drum's set was pushed last is what the shared effect holds.

Not measured: what the rendered difference sounds like. Nothing renders them - our
engine renders drum buffers dry and so does the oracle, which pushes only the 31
knobs, so `make test-songrender` compares two effect-free buffers and agrees. It is
not a small gap: 10 of the 48 drums in the ROM's demos, across 4 of its 12 songs,
have a non-zero dry/wet, and so do 75 of 388 drums in 59 of the 97 songs in
`data/saves`. TODO.md §5.

**The live record, from a RAM dump.** `tools/drumbuf.py` finds it by the four
pointers the drum part objects hold: a live song record is 0x43040 bytes and it is
`[4 x 0x10000 drum buffer][the song, with the live 0xB4 header]`. That is the
stride the wrapper at `0x020347EC` multiplies by, and it explains the drum knobs
being at record +0x42AF4: 0x40000 past the buffers, plus the song's own 0x2AF4,
which is the file's 0x2B2C less the 0x38 the live header is shorter. The four
parts sit 0x2E0 apart, each pointing at its buffer.

The buffers themselves say the render is not what `songplay.c` assumes. Their
first sample is -1, the value the mixer produces from silence, so they are the
mixer's output rather than the voice's; all four peak at exactly 32727; and
against our own render of the same song the zero-crossing rates agree to a percent
while the envelopes and peak positions do not (correlations +0.93, +0.54, -0.08,
+0.55). TODO.md §5 has the table.

**A second knob space.** The same ROM table continues past the mixer's 25 knobs
with `(42, 34)`, `(43, 35)`, `(44, 36)` … - `volume = 42 + channel` and
`pan = 34 + channel`, which is the mixer's layout offset by 25. So the original's
knob numbering has room for two 25-knob mixers, and the second table at
`0x0208790A` is the same list starting five entries in. Nothing here models that
yet.

Not verified: `+0x2EFC` (168 bytes, PLUS only) and `+0x2FA4` (56). Only the last
two bytes of the second are ever non-zero across the 100 PLUS songs here, and
their values look like a counter rather than a setting - 0x0000, 0x0015, 0x0236,
0xFFFF, 0x1782. A base near the first, `record + 0x42F5C`, is in the literal pool
of `0x0206C3A0` along with the drum-knob and name-row bases, so the region is
reached by the same code that loads a pattern; which bytes it touches is not
traced. The first block's non-zero offsets fall in two halves of 84, each with
data at 0..35 and 42..53.

---

## 16. GUI — the graphics path

### The reference oracle changes for graphics

Unicorn runs the CPU and stops at the hardware, so it cannot produce pixels.
`tools/ndsref/` builds the melonDS core headless and dumps, per frame, the whole
2D state - the nine VRAM banks and their VRAMCNT mapping, palette, OAM, both
engines' registers, the 3D engine's output plane - alongside the two
framebuffers melonDS composited from it. Scripted touch and key input reaches
any screen deterministically (`tools/sweep.py` walks the whole interface).

That gives two separable checks, which `make test-2d` runs:

- our VRAM bank mapper against melonDS's flattened buffers;
- our renderer, fed those buffers, against melonDS's pixels.

**72 frames over 29 distinct screens, 0 mismatches on both.**

### What the hardware is actually asked to do

Across that whole sweep the game only ever uses **BG mode 0 and display mode 1**.
No affine, extended or bitmap background is ever selected, so `src/gfx/nds2d.c`
does not implement them; `ds10_2d_unsupported` reports it if one is reached.

```
POWCNT1 0x020f   both engines on, no screen swap (L toggles the swap)
engine A -> the touch screen, engine B -> the top screen
```

| engine | layer | contents | tiles | tilemap |
|---|---|---|---|---|
| A | BG0 | the 3D layer | - | - |
| A | BG1 | the page, 256 colour | 0x04000 | 0x0800 |
| A | BG2 | `S_bg`, 512x512 | 0x10000 | 0x1000 |
| B | BG0 | the page | 0x04000 | 0x0800 |
| B | BG1 | `S_bg` | 0x10000 | 0x1000 |

Banks: B is BG-A, C is BG-B, E is OBJ-A, I is OBJ-B, G and H hold the extended
palettes, A and F belong to the 3D engine.

The pages are 256-colour **with extended palettes enabled**, so what they read is
palette 0 of an extended slot in bank G (engine A) or H (engine B), not palette
RAM. The same 252 colours from `PALETTE.NCLR` go into palette RAM at entry 4 and
into each 8 KB extended slot at the same offset.

### Sprites

```
OBJ-A   0x0000  DS10FONT       (32 KB)
        0x8000  CHARACTER_c    (29824 bytes)
OBJ-B   0x0000  CHARACTER_S    (13600 bytes)
palette CHARACTER_c at 0x208, CHARACTER_S at 0x608, both at entry 4
```

`CHARACTER_c` is one combined sheet: the per-screen `*_c` files and `FONT` all
appear inside it at fixed offsets, and only the combined file is loaded. No OBJ
extended palettes are used.

The game's own OAM builder (`0x0202e750`) adds **0x200** to a cell's tile number,
which is exactly `CHARACTER_c`'s base in the 64-byte tile units DISPCNT selects -
that is what confirmed the layout. The same function shows the whole widget-to-
OAM path: a widget holds x, y, priority, palette and an affine index, and writes
attr0/attr1 as one u32 and attr2 as a u16 into a software OAM buffer.

### The "3D" layer

The 3D engine is used as a textured-quad blitter, not for geometry: the register
census finds `MTX_SCALE` and `MTX_TRANS` but no perspective setup, and the only
textures are `3D_TEX`, `3D_MESS_TEX` and `UNFONT`, loaded at texture VRAM 0,
0x8000 and 0x10000 by `0x0202320c`. That function also builds a **dimmed copy**
of each palette at +0x200 by shifting each channel down, which is how inactive
captions are drawn.

It is not decorative. `51_ALL_DRUMS` draws its whole step grid this way: that
frame has **one** live OAM entry on the entire screen and 660 opaque pixels on
the 3D plane. The reason looks like capacity - sixteen steps by four drums is
sixty-four markers against 128 OAM entries in total, while the piano roll needs
at most one marker per step and does use sprites for them.

Every marker there is the same 11 by 10 block wherever it appears, so the
reimplementation puts that block straight into the plane, which the compositor
already takes as BG0. That is exact without any of the pipeline, and
`51_ALL_DRUMS` renders pixel-exact. A page that submits varying geometry would
need the real thing.

The plane stores 6-bit components, so its values read low: the marker's face is
`0x33` and its highlight `0x3b`, which are 51 and 59 out of 63 - near white, not
the dark grey the number suggests.

### Asset containers

Stock NITRO: a 16-byte header then blocks, magics stored byte-reversed. `NCLR`
palettes, `NCGR`/`NCBR` tiles or linear bitmaps, `NSCR` tilemaps, `NCER` cells,
`NANR` animations, `NFTR` fonts. `src/gfx/nitro.c` reads them; the parse is
checked against reality rather than the docs - `61_SYN_EDIT`'s tiles and tilemap
appear byte for byte in captured VRAM at the addresses its BGCNT points to.

The cell banks are not individually named: `CHARACTER_c.NCER`'s LABL holds one
label for 165 cells. The widget names in the string pool (`octUp`, `seqBank`,
`mxp_%d`) are the game's own identifiers, paired with the caption to draw
(`OCT+`, `SWING`), not asset labels.

### The screen table

Twenty entries at `0x02085f80`, `{u32, u32 index, char name[32]}`, read by
`0x02056ff4` into a name-to-index map. This is the navigation map's list:

```
 0 60_SYN_SEQ_PIANO   5 70_PATCH       10 52_FX          15 TARGET_PATTERN
 1 60_SYN_SEQ_PIANO   6 71_KAOSS       11 30_MIXER       16 54_DRUM_FX
 2 61_SYN_EDIT        7 71_KAOSS       12 51_ALL_DRUMS   17 53_DRUMEDIT
 3 61_SYN_EDIT        8 81_KEY_i       13 32_PATTERN     18 33_DRUM_SELECTER
 4 70_PATCH           9 81_KEY_i       14 01_MAIN_MENU   19 63_DRUM_EDIT_PATCH
```

Pairs are the synth 1 and synth 2 variants of the same page.

### The OAM allocator

Sprites do not go straight to OAM. Each engine owns a 0x540-byte block at
`0x0215F820` (engine B at +0x540):

```
+0x000  0x100  slot ownership, one u16 per OAM entry, 0xFFFF when free
+0x100  0x400  the OAM shadow, DMA'd to the hardware
+0x500   0x40  affine slot ownership, one u16 per rot/scale parameter set
```

- `0x02068864(alloc, start, count, engine)` reserves a run of OAM slots, and
  the matching affine slots, if every one of them is free. It stamps them with
  an incrementing owner id.
- `0x020689bc(alloc, src, n)` appends `n` entries into a reserved run. It
  copies **three halfwords per entry from a source of stride eight** - the
  fourth halfword of an OAM entry is the rotation parameter and belongs to
  whoever owns the affine slot, not to the sprite. That is why searching RAM
  for a widget's output byte for byte finds nothing.
- `0x02068a88(alloc, mat)` takes a 2x2 matrix in 20.12 and writes it, shifted
  down to 8.8, into the affine slot's four halfwords at `entry*0x20 + 6, 0xe,
  0x16, 0x1e`.

A knob pointer turns out to be one double-size rot/scale sprite with tile
`0x21a` - sixteen of them on the synth edit page, each with its own affine
index. Turning a knob is a matrix, not a different tile.

### What a drawable is

`0x02022c38(gfx, d, priority)` draws one element, and `0x02022ba4` shows what
`d` is - a 20-byte record:

```
+0x00  u16  cell index
+0x02  u16  rotation; >>4 indexes a sin/cos table at 0x02022c34
+0x04  s32  x scale, 20.12
+0x08  s32  y scale, 20.12
+0x0c  s32  x position, 20.12
+0x10  s32  y position, 20.12
```

The draw builds a matrix from the rotation and the scales, expands the cell
through it, offsets every tile by `0x200` if the element came from the first
resource, stamps the priority and appends. `0x02022d38` is the same with no
tile offset, for the sheet based at tile 0.

These records are findable in a RAM dump - scan for two consecutive `0x1000`
scales followed by a whole-pixel position - and the ones that are on screen
place their cell exactly where the OAM has it. They sit inside larger 0xFC-byte
widget objects, which is the stride the screen constructors step by.

### A screen constructor, read

`0x02065b04` builds 30_MIXER, and it is representative: a constructor is a walk
over geometry tables, one widget per entry.

```
pan knobs    table 0x02087610, stride 0x18: {s16 x, s16 y, ..., u32 knob}
             x = 21 + 43*ch, y = 47, hit box 28x28, knob 9 + ch
volume       table 0x020875b0, stride 0x10: {s16 x, s16 y, s32 h, ..., u32 knob}
             x = 10 + 43*ch, y = 85, 32 wide, 0x51 + 0x10 tall, knob 17 + ch
buttons      two rows of six, x = 3 + 43*ch, y = 62 + 11*row, 40x10
```

Those knob ids are the ones the **audio** work arrived at independently: the
sequencer's per-channel volume is knob `channel + 17` and its pan `channel + 9`
(section 15). Two halves of the ROM, read years-of-effort apart, agreeing.

### Screen constructors, by address

Every screen that places widgets is one of these. `0x0202e750` (button),
`0x0203a530` (slider), `0x020479d4` (rotary) and `0x0202ee0c` are the widget
constructors, so their callers enumerate the screens:

```
0x0202e01c  0x02039e68  0x02047384  0x02049150  0x0204bff8  0x0204e140
0x02050108  0x020572b0  0x020594d4  0x0205e1e4  0x02062914  0x02065b04
```

Read so far:

| address | screen | geometry |
|---|---|---|
| `0x02065b04` | 30_MIXER | tables above |
| `0x0204bff8` | 81_KEY | OCT- at (4,2), OCT+ at (0x23,2), both 0x1c x 0xe; synth selector at (0x42,4), 0x46 x 0xc |
| `0x020594d4` | 62_DRUM_SEQ_GATE | four drum rows at y = 0x21 + 13*i, name at x = 3 and solo at x = 0x16, both 19 x 13; a 64-entry grid state, four drums by sixteen steps |
| `0x0202e01c` | a shared widget helper, not a screen |
| `0x02039e68` | a settings page: wide slider at (5,22) 246 x 12, three vertical sliders at (5,54), six buttons at x = 3 + 43*i, y = 164, 36 x 16 |
| `0x020572b0` | 33_DRUM_SELECTER | four 117 x 76 panels at (7,25), (132,25), (7,110), (132,110), labelled DRUM1..DRUM4 (0x02086300) |
| `0x02047384` | 61_SYN_EDIT | sixteen knobs (0x020851d4) and three switches (0x02085180), each knob a 40 x 40 box |
| `0x0204e140` | 70_PATCH | six knobs at y = 93, x = 23 + 43*i (0x02085918); MG BPM slider at (11,38) |
| `0x02049150` | 71_KAOSS | two knob assignments, three buttons at x = 4 + 21*i, y = 2 |
| `0x02050108` | 60_SYN_SEQ_* | six page variants; two 16-step runs at x = 0x16 and 0x4a, y = 3 |
| `0x020594d4` | 62_DRUM_SEQ_GATE | four drum rows at y = 0x21 + 13*i, name at x = 3 and solo at x = 0x16, both 19 x 13; a 64-entry grid state, four drums by sixteen steps |
| `0x0202e01c` | a shared widget helper, not a screen |
| `0x02039e68` | a settings page: wide slider at (5,22) 246 x 12, three vertical sliders at (5,54), six buttons at x = 3 + 43*i, y = 164, 36 x 16 |
| `0x020572b0` | 33_DRUM_SELECTER | four 117 x 76 panels at (7,25), (132,25), (7,110), (132,110), labelled DRUM1..DRUM4 (0x02086300) |
| `0x02047384` | 61_SYN_EDIT | sixteen knobs (0x020851d4) and three switches (0x02085180), each knob a 40 x 40 box |
| `0x0204e140` | 70_PATCH | six knobs at y = 93, x = 23 + 43*i (0x02085918); MG BPM slider at (11,38) |
| `0x02049150` | 71_KAOSS | pad at (0,20), 256 x 172; three buttons at x = 4 + 21*i, y = 2 |
| `0x02062914` | 90_MAP | fifteen boxes (0x02087018) and the DRUMS submenu's four (0x02086f08), each with its rectangle, caption and an index into the screen table |
| `0x0205e1e4` | 52_FX | thirteen knobs sharing four positions at y = 161, x = 64/118/172/226; slider at (8,146); nine route buttons 66 x 14 |

52_FX's table at `0x02086874` carries each knob's caption inline and its
parameter id, and those ids are mixer knob ids - the mixer forwards 1..8
straight to the effect, which is what section 15 found from the audio side.
Three knobs show at a time, chosen by the effect type:

```
delay    DELAY TIME 2   L/R RATIO 3   FEEDBACK 7
flanger  LFO FREQ   4   DEPTH     5   -
chorus   LFO FREQ   4   DEPTH     5   FEEDBACK 7
always   DRY/WET    8                 slider 6
```

60_SYN_SEQ_PIANO's grid is 13 by 13 with its origin at (42,32), measured off
the rendered page - and the constructor agrees: it holds `0x9c` = 156 = twelve
rows of 13 for the visible height and `0x138` = 312 = twenty-four rows for the
whole note range, which is what the scrollbar down the left is for.

70_PATCH is the modulation patch bay rather than a patch browser. Its six
knobs are MG FREQ (24), PITCH IN INT (3), VCO1 P.IN INT (4), VCO2 P.IN INT (5),
CUTOFF IN INT (15) and VCA IN INT (17) - the *intensity* knobs. The ones
numbered 26..30 are the source selectors, the jack sockets along the bottom.

Two corrections that measurement had got wrong and the constructors fixed:

- the patch bay's knobs are at y = 93, not the y = 123 a blob detector found,
  and they are the intensity knobs rather than the source selectors;
- 61_SYN_EDIT's sixteen knobs are all out by (5,7) from where their bright cap
  sits, because the cap is not centred on the knob. The sixteen knob *ids*,
  read off the caption printed under each one, were all correct.

Which is the argument for reading the table rather than the screen.

A factory patch's 31 knobs sit at offset `0x20` of its 63-byte
`ds10synthNN.dat`, which is the 31 bytes that fill the file to its end.

### The map's boxes

The table is `{i16 x, y, w, h}`, then eight halfwords, then the caption
inline, at a stride of **0x44**. The third halfword after the rectangle is the
index into the screen table; the rest are the neighbours the D-pad moves to.
`0x02086f08` is the same table with the DRUMS submenu's four entries in front
of it.

The first ten screen-table entries are five pairs - sequencer, edit, patch,
KAOSS, keyboard - and a pair is the synth 1 and synth 2 variants of one page.
So the low bit of the index is which synth the box selects, which is what
makes SYN2 EDIT open synth 2's patch.

MENU and SONG are drawn on the map but are in neither table, and `31_SONG` is
not in the twenty-entry screen table at all, so nothing on the map opens it.


`0x02062914` builds 90_MAP out of two tables of 0x44-byte entries. Each carries
a rectangle, an inline caption and an index into the twenty-entry screen table
at `0x02085f80`, which is what names the page the box opens:

```
SYN1 SEQ   (7,25) 46x16   -> 60_SYN_SEQ_PIANO     MIXER  (203,25) 36x16 -> 30_MIXER
SYN1 KBD  (39,45) 54x16   -> 81_KEY_i             FX     (207,55) 30x16 -> 52_FX
SYN1 KAOSS(39,65) 54x16   -> 71_KAOSS             DRUMS (206,142) 46x16 -> 33_DRUM_SELECTER
SYN1 EDIT(117,25) 46x16   -> 61_SYN_EDIT          pattern  (4,2) 76x14 -> 32_PATTERN
SYN1 PATCH(131,54) 53x16  -> 70_PATCH
```

with the SYN2 row 62 pixels below its SYN1 counterpart, and the DRUMS submenu's
three at (85,64), (85,96) and (85,128).

Two screens share a layout with two others, which is why the same tables serve
both: 53_DRUMEDIT is the synth edit page and 63_DRUM_EDIT_PATCH is the patch
bay, and 54_DRUM_FX is the FX page with three effect buttons instead of nine
route buttons.

### A screen's per-frame update

Each screen's update is a four-state machine on its `+0x13c`, and the shape is
shared - `0x02066e78` is the mixer's, `0x0205f2cc` the FX page's, and both
read the same `+0x134`, `+0x13c` and `+0x144`:

```
0  build     when the transition object's +0x144 is 0 or 4, run the
             constructor and go to 1
1  settle    wait for the transition object's +0x140 to reach 5
2  run       the loop below
3  swap      flip the deck, rebuild, back to 2
```

State 2 is the whole of a screen's behaviour:

- take the widget the pen is on from the registry's `+0x44`;
- look its name up in the screen's own name-to-knob map - `+0x210` on the
  mixer - and read the knob id from the entry's `+0x0c`;
- fold the id into a channel: 9..14 are the pans and 0x11..0x16 the volumes,
  so the channel is the id minus 9 or minus 0x11;
- push the value into the machine (`0x0206e084`);
- run the two redraw passes.

Which is the same relation this reimplementation implements directly, with the
name lookup collapsed out: a hit box gives a knob id, the id goes to the
engine, and the widget redraws through the sweep or the span.

### The screen vtable

A screen is reached through a vtable, and the layout is the same for all of
them - 81_KEY's is at `0x02085868`, 52_FX's at `0x020867e4`, 30_MIXER's at
`0x0208758c`:

```
+0x00  the per-frame update
+0x04  a short query on the same state field, 44 to 336 bytes
+0x08  0x0202222c   shared by every screen
+0x0c  0x02022420   shared
+0x10  another screen method
```

Slot 0 is the update, and every one of them reads the state field. The long
ones lead with a lazy allocation of the screen's shared object, which is what
makes them long: 81_KEY's 1984 bytes are mostly one `if (singleton == 0)`
branch that builds the keyboard object and loads its nine `S_*` background
overlays, with the state machine after it.

### Every screen's update, by address

Each screen is a constructor plus an update, and the update is where its
behaviour lives. All twelve are located:

| screen | constructor | update | bytes | its own passes |
|---|---|---|---|---|
| 30_MIXER | `0x02065b04` | `0x02066e78` | 732 | `0x0206724c` `0x02067160` `0x0206617c` |
| 52_FX | `0x0205e1e4` | `0x0205f2cc` | 2220 | `0x0205eb84` |
| 61_SYN_EDIT | `0x02047384` | `0x02047de0` | 2220 | inline |
| 81_KEY | `0x0204bff8` | `0x0204c9d0` | 1984 | inline |
| 71_KAOSS | `0x02049150` | `0x0204a1d4` | 2168 | inline |
| 70_PATCH | `0x0204e140` | `0x0204e9a4` | 2204 | inline |
| 60_SYN_SEQ | `0x02050108` | `0x02051074` | 480 | `0x02050fd4` `0x02051254` `0x020513a4` `0x020514ec` `0x02051630` `0x02051774` |
| 62_DRUM_SEQ | `0x020594d4` | `0x0205a384` | 872 | `0x02058e94` `0x020590f0` `0x0205a704` `0x0205a8a0` `0x0205aa34` `0x0205abc4` |
| 90_MAP | `0x02062914` | `0x02063fd0` | 1308 | inline |
| 33_DRUM_SELECTER | `0x020572b0` | `0x02057f74` | 388 | `0x02057a74` `0x0205830c` |
| settings | `0x02039e68` | `0x0203ac3c` | 280 | `0x0203ac30` `0x0203ad54` `0x0203ad5c` `0x0203ad68` `0x0203ad70` |

`0x0202e01c` is **32_PATTERN's constructor**, not the shared widget helper it
was filed as here. Its table is at `0x020845dc`, 28 bytes an entry - x, y, w,
h, the caption's offset, then the name - and it places:

- COPY at (4,2) and LOCK at (35,2), both 28 by 14;
- sixteen `PAT%02d` at x = 6 + 63*(n%4), y = 24 + 33*(n/4), 55 by 27,
  captioned `pat %d`;
- two rows of twelve, `tm%d` and `ts%d`, at x = 3 + 43*n, y 160 and 171, 40 by
  10, captioned M and S. Twelve because the PLUS has a second deck; the widget
  index is n and n + 16, which is why the mute and solo pass at `0x0206724c`
  splits on bit 4 of it;
- a `ptdeck` button at (0xa2,2), 28 by 14, only when `0x020338ec` says the
  document has the second deck.

Three of those had been measured or tapped instead. The button grid's pitch was
one out in both axes, and the marker offset that compensated for it went to
zero when the origin was corrected.

The eleven fall into three kinds:

- **per-frame state machines**: 30_MIXER, 52_FX, 60_SYN_SEQ, 62_DRUM_SEQ. These
  read the state field, dispatch, and do the registry work - 52_FX's profile is
  eight registry inserts, five map lookups and five pushes into the machine,
  which is the mixer's shape.
- **one-shot setup**: the settings screen, 90_MAP and 33_DRUM_SELECTER. They
  configure or allocate and call the constructor once.
- **the editor screens**: 61_SYN_EDIT, 81_KEY, 71_KAOSS and 70_PATCH. These are
  dominated by a shared prologue that lazily builds the editor's common object
  and loads its nine `S_*` background overlays through nine calls to
  `0x020284a4`. Comparing their instruction streams with immediates masked,
  61_SYN_EDIT, 71_KAOSS and 70_PATCH agree on about half their instructions and
  81_KEY on under a third, so it is a shared prologue with per-screen work
  after it rather than one function emitted four times.

  Their tails are short. 61_SYN_EDIT and 70_PATCH fetch the machine and call
  the constructor; 71_KAOSS just calls it; 81_KEY silences every voice
  (`0x0206e114` per track) before calling it and then enables input. All the
  weight is in the prologue.

52_FX's is the fullest of the state machines and worth reading as the example:

```
state 0   run the constructor, then for each widget sprintf its name, look it
          up, and insert it into the registry - two inserts, or four when the
          widget's +0x78 is 5, matching the registry's maps
state 2   take the pen's widget, compare its name, look it up, branch on the
          knob id, and push the value into the machine - five such sites, one
          per control class
tail      call its own pass at 0x0205eb84 and then the constructor again
```

That tail is why the effect type's three knobs share their coordinates in the
table: changing the type rebuilds the screen.

About 14.5 KB across the eleven.

### Two more updates

`0x02051074` is 60_SYN_SEQ's. Same four-state machine, on `+0x140` here, but
its running state dispatches again on a **sub-mode** at `+0x144` - one handler
per page variant, which is what the constructor's six-name table was for:

```
0 -> 0x02051254   3 -> 0x020513a4
1 -> 0x020514ec   4 -> 0x02051774(screen, 0)
2 -> 0x02051630   5 -> 0x02051774(screen, 1)
```

Its constructor `0x02050108` is read now, and it settles the scrollbar, which
had been measured off a rendered page:

```
FUN_02039188(screen+0x390, 0, 0x54, <from the document>, 0x90)
```

- the slider is the one `ds10_ui_slider_pos` is decompiled from, so this is
  min 0, max 84, a value the document holds (`0x02034500` fetches it) and a
  span of 144 pixels;
- its origin is `+0x41C`/`+0x420` = 0x5000, 0x1f000 in 20.12, i.e. **(5, 31)**,
  which agrees with the handle measured at x 5..15;
- `+0x3A0` = 156 is the visible height and `+0x3A8` = 312 the scrollable one,
  twelve and twenty-four rows of 13 - the same numbers the grid constructor
  gives;
- `+0x44C` = 12 and `+0x448` = `+0x39C` = 24 are those row counts directly.

Where the value comes from *is* now established, and it is in the song:

```
FUN_02034500(store, 0)                       r1 = 0 at the call site
  -> (*0x0206b0c0)(store + 0x20 + deck * 0x43040, 0)     deck flag at +0x8cd9c
  -> ldrb [base + 0x40000 + 0x310 + pattern*676 + track*12]
     pattern at base + 0x40050, track at base + 0x40054
```

676 is the pattern stride and 12 the per-track record. The live model's header
is 0xB4 where the file's is 0xEC, and 0x310 - 0xB4 = **0x25C** - exactly the
per-track records this document already had at `+0x25C`, whose bytes 2 and 3
are the two lane knob ids. So **byte 0 of each track's 12-byte per-pattern
record is the piano roll's scroll position**, and it is per pattern and per
track, and it is saved.

The 0..84 value is settled, by `tools/probe_ram.py` tapping the bar and
diffing main RAM. **It counts upward: 84 is the top of the range, 0 the
bottom.**

    handle top y = 31 + (84 - value) * 144 / 84

The default in the ROM's own session is 46, which puts the handle at 96 and
the captured page draws it at 97. A tap at y 170 takes it to 6, which puts the
top at 164 with the finger six pixels in - a twelve-pixel handle hit in the
middle. Both agree.

84 is 7 x (24 - 12), so a row of the scroll range is seven of these units. The
default 46 is not a multiple of seven, so the roll does not rest on a row
boundary: the value tracks the finger rather than the grid, and a second tap
at the same place moved it by one.

**Correction.** An earlier pass here recorded the seven-per-row reading as
ruled out, on the grounds that (97-31)*84/144 = 38.5 is not a multiple of
seven. That arithmetic assumed the value counted downward. It counts upward,
the default is 46 rather than 38, and seven units a row is what 84 = 7 x 12
says it is. What is ruled out is only that the default happens to be
row-aligned.

Two runs of sixteen sprites are built at the end, at y 3 with x from 22 and
from 74, which is the top bar's two text fields.

Its build state also sets up the step grid:
`FUN_0205fdc4(scroller, 0x2d, 0x23, 0xd0, 0x9c, 0x10, 0xc)` - a scrollable
region at (45,35), 208 by 156, of 16 by 12 cells of 13 square. Measuring that
off the rendered page had put the origin three pixels out in both axes.

`0x02049150` is 71_KAOSS's constructor, and it names its top bar rather than
leaving the icons to be guessed at:

```
%d        x = 4 + 21*n, y 2, 18 x 14, three of them        the pad's 1 2 3
setting   x = 67,  28 x 14, captioned SET
playmode  x = 98,  28 x 14, caption rewritten by the page
rec       x = 162, 28 x 14, caption '}' - the record glyph
FUN_02060330(pad, 0, 0x14, 0x100, 0xac)                    the pad itself
```

`playmode` is the SMT/HLD button, which ties it to the two spellings in the
string pool - `OFF`/`TRIG`/`SMOOTH` at 0x02084b1c and `OFF`/`HLD`/`SMT` at
0x02084b34 - and to the `PLAY MODE` line in the SET dialog. The button at
x 131 is *not* here: the four editor screens share a prologue and it comes
from there, which is also where the transport and the screen swap come from.

The pad's own placement, `(0, 0x14, 0x100, 0xac)`, is the one this document
already had.

### The document's field accessors

`0x02034500(store, idx)` tail-calls a jump table at `0x0206b0c0` with the
per-deck block:

```
(*0x0206b0c0)(store + 0x20 + store[+0x8cd9c] * 0x43040, idx)     idx <= 9
```

`+0x8cd9c` is the deck flag and `0x43040` the per-deck stride. Index 0 reads a
byte at `base + 0x40000 + 0x310 + pattern*676 + track*12`, with the pattern
index at `base + 0x40050` and the track at `+0x40054` - so these are per
pattern and per track, and `0x310 - 0xB4` is `0x25C`, the per-track record in
the pattern. `0x02034548(store, a, b)` is a three-argument sibling.

Which index is which comes from the constructors that call it:

```
idx 0   60_SYN_SEQ +0x390, 62_DRUM_SEQ +0x3B4   the piano roll's scroll
idx 1   81_KEY +0x144                            the keyboard's octave
idx 2   71_KAOSS +0x394                          the pad's first knob id
idx 3   71_KAOSS +0x398                          the pad's second knob id
idx 4   71_KAOSS -> FUN_0204974c(+0x18c, v)      the pad, unnamed
idx 5   71_KAOSS -> FUN_02049754(+0x18c, v)      the pad, unnamed
idx 6   71_KAOSS +0x15c                          unnamed
idx 7   71_KAOSS +0x14c, and it selects which of the pad's three slot buttons
        comes up lit - `FUN_02029d88(+0x4a0 + v*0xfc, 1)`
```

Indices 2 and 3 being the pad's knob ids matters: the lane knob assignment the
song format keeps at the per-track record's bytes 2 and 3 is reached through
the same accessor, so the pad's two axes and the automation lanes are the same
two fields, which is what this reimplementation had inferred from the data.

`0x020338ec(store)` tests whether the document has the second deck, which is
what gates `ptdeck` on 32_PATTERN.

### The software float helpers

The screens use software floating point for anything curved, so these turn up
constantly and are worth naming:

```
0x0200bf08  int -> float          0x0200c548  float -> int
0x0200c608  multiply              0x0200ca18  add
0x0200c7e8  divide                0x0200cc9c  ?, takes a float and a constant
0x0200be50  compare against zero
0x0200c158  the compiler's signed integer divide: quotient r0, remainder r1
0x0201263c  atan2-ish             0x0201235c  square root
0x02011fc4  build a 2x2 rotation from (sin, cos)
0x02011fd0  scale a 2x2 by (sx, sy), each term >> 12
```

Two float-heavy functions located but not read:

- `0x0202cb20` builds a ribbon or curve: seventeen sampled points, then sixteen
  pairs of shorts written to `screen + index*0x40 + 0x68` and `+0x1e8`. It
  reads the sine table as an easing curve. Most likely the patch bay's cables
  on 70_PATCH or the pad's trail.
- `0x02047224` is a screen tween, also using the sine table's cosine column as
  an easing curve, driven by a state at `+0x1c`.

### The widget object

`0x0202e750(w, kind, x, y, cw, ch, name, cap)` is the constructor, and it maps
the object out:

```
+0x04 x        +0x08 y        +0x0C cw       +0x10 ch    +0x14/+0x18  cw*2, ch*2
+0x1C min = 0  +0x20 max = 1  +0x28 value    +0x38 name (sprintf'd from the fmt)
+0x7C kind     +0x80 0x44     +0x82 angle    +0x84/+0x88 scale x, y = 0x1000
+0x8C/+0x90    x, y in 20.12
+0xA4/+0xA8    x + 0x12, y + 2 - where its sprite run starts
+0xAC priority +0xB0 tile base +0xB4 palette
+0xC0..+0xD0   the OAM attribute pieces
+0xD4          a 0x100-byte OAM buffer, malloc'd    +0xD8 32 entries
+0xDC 1        the x step between them              +0xE4 caption
```

Two things this settles.

**The registry's split.** `+0x7C` is the `kind` argument, so which of the four
maps a widget lands in is chosen by the caller at construction, not derived.

**The knob chain closes.** `FUN_02022ba4` reads its angle at `+2` and its
scales at `+4` and `+8` of what it is handed - which is `widget + 0x80`, so
those are `+0x82`, `+0x84` and `+0x88`. That is exactly the field
`0x02055b98` writes the angle into. So knob value -> `0x02055b98` -> `+0x82`
-> `0x02022ba4` -> the sine table -> the affine matrix -> OAM is one path, and
both ends of it are now differentially verified against the ARM.

The constructor initialises the angle to 0 and both scales to 0x1000, which is
why every knob in this ROM is drawn at unit scale and why the forward and
inverse matrices coincide.

### The widget placers

Reading five constructors turns up the same handful of calls, which is the
shared machinery the screens are built out of. Each takes the widget, then a
rectangle, then the name and caption pointers:

```
0x0202e750(w, 0, x, y, cw, ch, name, cap)    place, one class
0x0202ee0c(w, 0, x, y, cw, ch, name, cap)    place, another class
0x020299bc(w, 0, x, y, cw, ch, name, cap, 0) place, a third
0x0202e9d4 / 0x0202a33c / 0x02029c24         set the hit box
0x0202ebd0 / 0x0202f07c / 0x02029c5c         set the caption and its position
0x02029d7c / 0x02029d88                      enable, and set the lit state
0x0202ee04 / 0x02029dac                      set the widget's index
0x02029c54                                   set its class
0x02029db4(registry, w, name)                register it
```

Which class a screen uses seems to follow the button's artwork rather than its
behaviour: 32_PATTERN's sixteen buttons and 71_KAOSS's `rec` go through
`0x0202e750`, its `setting` and `playmode` through `0x0202ee0c`, and the pad's
three slot buttons through `0x020299bc`.

Scanning every call to the three placers for an immediate x is how the shared
bar below was found. It does not find widgets placed in a loop with computed
coordinates - 51_ALL_DRUMS' four row buttons are one such case, and are still
measured off a render.

30_MIXER's mute and solo strip is placed in a loop at `0x02065ed8`, named
`tm%d` as 32_PATTERN's is, with `x = n*43 + 3` and the size as immediates -
40 by 10, the same. Both had been measured off a render at 34 by 9, which is
the drawn box and not the hit box. 33_DRUM_SELECTER's `EDIT` and `rec` are at
`0x020572f4` and `0x02057460`.

Widening the scan to sizes rather than positions reaches the widgets placed in
loops, and gives every statically-sized one in the ROM:

```
pat 1 1 / pat %d  55x27      tm%d              40x10   (mixer and pattern)
ptdeck/fxdeck/mxdeck 28x14   setting           28x14
octDown           28x14      rec               32x16 on 81_KEY, 28x14 elsewhere
SOLO0             28x14      synth             70x12
the pad's slots   18x14      the drum rows     19x13
menu / MENU       46x16      TRS 36x16, chain 28x14, bright0 66x16
```

Two things fall out. The wide button on the left of 61_SYN_EDIT and 70_PATCH,
which this document had as unidentified, is a `synth` selector - same name and
size as 81_KEY's. And every bar button is 28 wide, not the 26 the drawn frames
suggested; the drawn frame is consistently narrower than the hit box, which is
the same mistake the two mute and solo strips had.

### The shared top bar

Around `0x02053900` is the builder the pages share, and it registers every
button on the bar by name - which settles the two that had been found by
tapping and the several that had been read off their icons:

```
SOLO0 SOLO1 SOLO2   captioned "S"     x 131
AC0 AC1             captioned "CLR"   x 131
DECK                                  x 131
PAGE                captioned "{"     x 42, the sub-mode cycle
PLAY                captioned "$"     the triangle
menu swap0 swap1                      x 224, the up and down arrows
```

x 131 is one slot that different pages fill differently - solo on the editor
screens, CLR on the sequencer, the deck button elsewhere. Which page gets
which still comes from the captions the rendered pages draw; what the ROM
gives is the set and the slots.

Scanning every call to the three placers for an immediate x turns up twenty
statically-placed widgets in all. Besides the bar: `ptdeck`, `fxdeck` and
`mxdeck` at x 14 on their pages; `bright0`/`bright1`/`bright2` at 95 and 177
on the settings screen; and 90_MAP's bottom row - `subSwing` captioned SWING
at (76,176), `subTempo` at (135,176) and `subStep`/`subSave` at (200,176),
which is the bar `ds10_ui_map_labels` currently draws text into at guessed
positions.

That is why `x 131` is in none of the screens' own constructors. It also
confirms, independently, both of the hardware probes: `tools/probe_play.py`
identified the triangle as the transport by watching the screen move, and
`tools/probe_ram.py` plus the argument from which pages carry one identified
the S as a solo. Three SOLO widgets rather than three states - the toggle the
RAM probe saw was two-valued.

`0x0204bff8` is 81_KEY's constructor, and its bar agrees with the pad's where
they overlap:

```
octDown  (4,2)    28 x 14        octUp  (0x23,2)  28 x 14
synth    (0x42,4) 70 x 12        rec    (0xa2,2)  32 x 16
```

The three-entry table this document already had for OCT- , OCT+ and the synth
selector is exactly right. What was measured and is now read: those buttons'
widths on the shared bar, and that x 162 is `rec` here too. x 131 is again
absent, which is the third page to say the shared prologue places it.

`0x02057f74` is 33_DRUM_SELECTER's, and it is a commit rather than a redraw:
when the document's `+0x2ec` reaches 2 a drum has been picked, so it sets the
current drum, reloads the patches, re-renders each drum's buffer
(`0x0206dfcc` per drum, then `0x0206ec10`) and rebuilds the screen.

`0x020594d4` is 62_DRUM_SEQ's constructor. It places the four row pairs in a
loop - `0x0202ee0c` at x 3 and `0x0202e750` at x 22, both y = 33 + 13*row and
19 by 13, the 13 being the grid's own row pitch - which is what 51_ALL_DRUMS
shows down the left of its grid and what had been measured off a render at
17 by 11.

It also places the scrollbar, and agrees with the synth sequencer's exactly:

```
FUN_0203a530(bar, 5, 0x1f, 0x18, 0x9c, 5, ...)   x 5, y 31, 24 rows, 156 tall
FUN_02039188(bar, 0, 0x54, FUN_02034500(doc, 0), 0x90)
```

Same origin, same 0..84 over a 144-pixel span, and the same document field 0 -
so the scroll position is one field serving both sequencer pages.

`0x0205a384` is 62_DRUM_SEQ's, and it is the sequencer's shape again: a
sub-mode dispatch in the running state, and a grid placed by
`FUN_0205fdc4(scroller, 0x2d, 0x23, 0xd0, 0x34, 0x10, 4)` - the same origin
and cell as the synth sequencer's, four rows instead of twelve, one per drum.
Its settle state is where it registers the four drum rows' widgets by name.

`0x0203ac3c` is the settings screen's, and it is not a per-frame loop at all -
it configures the document, chooses a pattern and calls the constructor once.

`0x02063fd0` is 90_MAP's, and it is the same kind of thing but bigger: it
lazily allocates the app's shared singletons on first use - the document
(`DAT_020644ec`), the song and pattern store (`DAT_0206450c`, a single
allocation of about 575 KB, which is where the `+0x8cd9c` deck flag the other
screens read lives) and a third object at `DAT_02064534` - wires them into the
screen and calls the constructor.

So two of the eleven "updates" are one-shot setup rather than a loop.

### The mixer's redraw passes

The two passes state 2 ends with, on the mixer:

- `0x02067160` is the **level meters**. It reads the sequencer's current step
  and, if it has moved since the screen's `+0x148`, asks `0x0206f480` for each
  of the six tracks' velocity at that step and stores it divided by four into
  the six bytes at `+0x1dc`. With the transport stopped (`+0x32c` is 1) it
  zeroes them.
- `0x0206724c` is the **mute and solo buttons**. It takes the widget under the
  pen from the registry, requires its class to be 0xb and its state 4, and
  splits on bit 4 of its index at `+0x98` - one row calls `0x02052fec`, the
  other `0x02052fdc`.

The fader handles themselves are the six cell-15 entries in the display list,
at x = 10 + 43*ch, and they move to `85 + slider_pos(value)` - the slider
origin the constructor gave them, plus the span mapping.

### The widget registry

Every screen registers each widget by name into one global registry at
`0x02124634` - `0x02029db4` inserts, `0x0202b198` removes, and the screen
destructor (`0x0202a9e0` is one) unregisters the lot. The registry keeps
separate maps by widget class: `+0x0c` and `+0x18` split on the widget's
`+0x7c`, and a widget whose `+0x78` is 5 goes into `+0x24` or `+0x30` as well.
`+0x44` and `+0x4c` hold whichever widget currently has the pen.

So widget lookup is global and by name, not per-screen. That is what the
`sp_%d`, `mxp_%d`, `fxr_%d`, `octUp` and `seqBank` strings in the string pool
are for: they are the keys a screen's handlers use to reach its own widgets.

### A widget's update

`0x02055b98` is the knob's value-to-angle map, and it is the same for every
screen that has knobs:

```
angle = 0xA000 + 0xC000 * (value - min) / (max - min)
```

with 0x10000 a full turn - 225 degrees at the minimum, sweeping 270 clockwise
to 135. The 0xC000 is `(sweep/2) * 0x10000 / 360` doubled, and the divide by
360 is a multiply by 0xB60B60B7 that the compiler emitted; every screen passes
270 as the sweep.

The drawable's rotation field is that angle shifted right by four, indexing a
4096-entry table of (sin, cos) in 1.12 at `0x0207a3d8`.

Checked against a captured frame: all sixteen of the synth edit page's pointer
matrices lie inside that sweep, several sit exactly on its end stops, and
OCTAVE - whose 0 is the midpoint of a -2..2 range - carries the identity
matrix.

A slider's is `0x02039188`, and it is linear: the handle sits at
`span * (value - min) / (max - min)` along the span, measured from the far end
for the two orientations whose minimum is at the bottom or the right. The
original writes it into the drawable's x or y at 20.12 and leaves the other
axis at the slider's origin.

Between the two - the knob's sweep and the slider's span - that is the whole of
what a control redraws when its value changes.

### Text

The interface's text is **one 8x8 sprite per character**, straight out of the
character sheet. The sheet's first glyphs, in 32-byte tiles:

```
0x00..0x09  the digits
0x0a 0x0b   plus and minus
0x11..0x2a  A to Z
0x2b        hash
```

The pen advances by the glyph's ink width plus one. Drawing "RESIN#" at (27,177)
that way reproduces the captured OAM exactly - six sprites at x 27, 31, 35, 39,
41 and 46 with tiles 0x022, 0x015, 0x023, 0x019, 0x01e and 0x02b, palette 9,
priority 2 - every field.

Engine B addresses sprite tiles in **32-byte units**, not 64: DISPCNT bits
20-21 are 0 there and 1 on engine A. Getting that wrong is what made the text
look like it came from somewhere else.

Two wrong turns worth recording, because both looked convincing:

- The top screen's sprite VRAM holds far more than the one file loaded into
  it, and reading the surplus as tiles shows digits and letters. That looked
  like glyphs rasterised at runtime from the eight NFTR fonts in `/data`. It is
  not: the surplus is a second copy of CHARACTER_S, and **no NFTR filename
  appears anywhere in the ARM9**. The fonts are unused by this game.
  `src/gfx/nftr.c` reads them correctly and is kept, but nothing needs it.
- Matching a sprite's tile against the sheets at bases 0x200 and 0 leaves the
  map's captions unexplained, and the implied bases come out scattered. That is
  the signature of comparing against *empty* tiles, which match anything - not
  of a missing sheet.

### The step grids

Both sequencer pages put their grid at (45,35) with a 13-pixel cell, which is
the constructor's `FUN_0205fdc4(scroller, 0x2d, 0x23, 0xd0, ...)`. The piano
roll is 16 by 12 of a 24-row scrollable range; the drum grid is 16 by 4.

What the markers are differs: the piano roll draws cell 7 of the page's own
sheet at priority 2, and `51_ALL_DRUMS` draws quads on the 3D plane. Both pages
put a cursor above the grid - `CHARACTER_c` cell 4, the only sprite in that
band.

The row-to-note mapping comes from the captured frame's keyboard column.
Reading its whites and blacks off gives a full octave with A4 on the top row,
and the game labels row nine "C4" with two font sprites of its own; row 0 is
therefore the highest note and `note = top - row`. Notes outside the twelve
visible rows are not drawn - which is why no pattern in the ROM matches the six
markers that frame shows.

The button left of the grid's top row cycles the sequencer's sub-mode. Tapping
the same point seven times walks

```
PIANO -> GATE -> VOL -> PAN -> KAOSS -> KAOSS -> PIANO -> ...
```

so there are six positions, and the two that come up as KAOSS are the two
automation lanes the song format carries - the pattern's `lane[0]` and
`lane[1]`, which share a page image. That is `tools/probe_buttons.py`, which
taps a page and reports what changed: these buttons are not placed by the
shared widget helper, so there is no table to read them out of, and their
rectangles live inside the update's own code. The button responds between
x 41 and 58 at y 8, which is where tapping does and does not reach it rather
than a rectangle read out of the ROM.

The four value pages draw the same marker on the same grid as the piano roll,
one per step, at the row for that step's value:

| page | rows | mapping |
|---|---|---|
| GATE | 5, LEGATO to 25% | `row = 5 - gate` |
| VOLUME | 12, MAX to MIN | `(11 - row) * 127 / 11` |
| PAN | 5, L2 to R2 | `row * 128 / 4`, clamped |

Each is pinned by the data rather than measured. Every velocity on every step
of every song in the ROM is one of exactly twelve values - 0, 11, 23, 34, 46,
57, 69, 80, 92, 103, 115, 127 - which is that integer division over twelve
rows and nothing else. The gate byte runs 1 to 6 against five rows, and the
sixth is retrigger, which `song.h` already had as distinct from the four
lengths and the tie.

The check on all three is that the blank template's defaults land on a row
rather than between two: gate 3 is the 75% row, velocity 127 is MAX, pan 64
is C. A mapping that put the untouched value between rows would be the wrong
one.

A rest draws no value marker on any of them. All three captured pages carry
nine markers, and those are exactly the nine steps of that pattern which
sound, though the bytes exist on every step.

Sweeping the rest of that bar with the prompt dismissed between taps finds two
more:

| x | what it does |
|---|---|
| 130 - 158 | raises a modal CLEAR ALL DATA prompt with YES and NO |
| 222 - 255 | brings the navigation map to the touch screen, as the L shoulder does |

Both respond on either row of the bar. The prompt is modal enough to swallow
the map key, which is what made the first three probe runs useless. Nothing
between them changed anything the probe could see, so whatever the two
controls drawn there are, they are not simple buttons.

`32_PATTERN`'s sixteen buttons came out the same way. Its columns break
between x 64 and 69, 129 and 134, 194 and 199 - a pitch of 65 - and its rows
between y 56 and 60, 88 and 92, 120 and 124, a pitch of 32. The marker on the
selected one is `CHARACTER_c` cell 20 at the button's origin plus (4,-2),
which is where the captured page has it. Drawing it from the current pattern
reproduces that frame exactly; what the page still differs by is its live
text.

These markers are **state**, so `tools/pages.py` takes them out of the display
list rather than freezing the captured pattern into the page, and `make
test-page` draws them back from `tools/pagecheck.py`'s `SEQ_STATE`. That makes
reproducing the frame depend on the whole model, and it has teeth: one semitone
off is 204 pixels wrong, the cursor one step off 128, one missing note 102.

### Status

Reproduced from the asset files alone, and pixel-exact against melonDS for the
same page (`make test-page`): `61_SYN_EDIT`, `70_PATCH`, `30_MIXER`, `81_KEY_i`,
`60_SYN_SEQ_PIANO`, `51_ALL_DRUMS`, `33_DRUM_SELECTER`, `01_UNMAIN_MENU`,
`AD0_TITLE_i`. `71_KAOSS` differs only by its moving cursor, `52_FX` and
`32_PATTERN` by live values, and `90_MAP` by the values it displays.

The corpus those are checked against had been missing most of the interface:
`tools/sweep.py` tapped coordinates measured off a rendered screen, and several
missed their box by a pixel or two - the FX box starts at y 55 and was being
tapped at 54 - while three were nowhere near one. Driving the sweep from the
decompiled map table instead picked up `32_PATTERN`, `52_FX`,
`33_DRUM_SELECTER` and `51_ALL_DRUMS`.

`data/pages.bin` holds a **display list** per page - cell, position, matrix,
priority - and the sprites are generated from it by the decompiled expander.
`tools/pages.py` builds those lists by inverting the expansion against a
captured frame. The synth edit page's 38 visible sprites come out of 19 cells
with nothing left over, and its knob pointers turn because their matrix is
ours to set.

Not decompiled: the code that *produces* those lists - each screen's
constructor and its per-frame update. Until that is read, a page shows the
state it was captured in.

`01_UNMAIN_MENU` and `90_MAP` come out mostly as unattributed OAM rather than
cells, because most of what they draw is text - one sprite per character, not
a cell. The renderer for that is now in `src/app/ui.c` and the map's bottom bar
is generated from the loaded song rather than replayed. Attributing the rest to
text rather than to cells is what would empty the raw list.

A page that carries its own NCLR - the title and advertisement screens - puts
it in the extended palette slot **its own layer reads**, replacing the shared
colours there rather than sitting alongside them. Engine B's page layer is BG0,
which takes slot 0; engine A's is BG1, which takes slot 3 because its BGCNT has
bit 13 set. The offset within the slot is entry 4, the same as the shared
palette's - the slot is what differs, not the offset. With that, `AD0_TITLE_i`
is pixel-exact and the other five ad screens render.

The wrong turn here was assuming the two palettes overlapped, on the strength
of the page's colours appearing to start at entry 1. They do not overlap; they
are in different slots, and the entry was misread off a run of zeros.
