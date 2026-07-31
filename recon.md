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
a message id and argument into the callback object and calls its vtable slot 1.
The ids are readable FourCCs: `"step"`, `"edtp"` (end of pattern), `"stpt"` (bar
— the song player answers with the next pattern), `"rntp"` (end of song). If the
bar callback returns non-zero the transport stops and a bar-start event
(`0x1C`) is broadcast. That is the seam between sequencer and song format.

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

Gate values are special-cased rather than being a plain length:

| gate | meaning |
|---|---|
| `0xFF` | tie - sounds until the next note, never auto-released |
| `6` | retrigger |
| `5` | one sub-tick longer than a full step (bleeds into the next) |
| `4` | one sub-tick shorter than a full step |
| other | `gate * subticks / 4` |

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

Still decompiled but not verified: the song-player callback protocol, which is
understood but has no implementation - the chain walker that answers `"stpt"`
with the next pattern.

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
+0x004  0x3E  per-pattern parameters, not yet named
+0x042  0x180 6 tracks x 16 steps x 4 bytes    (2 synth + 4 drum)
+0x1C2  0x080 2 lanes x 16 steps x 4 bytes     automation, synth tracks only
+0x242  0x062 synth, drum and mixer settings, not yet named
```

Accessors, all leaf functions taking `(doc, pattern, track, step, out)` and
copying signed bytes:

```
0x02033F8C  get step        doc +0x40116 + pattern*676 + track*64 + step*4
0x02033FE4  get step count  doc +0x400D4 + pattern*676   (pattern from +0x40070)
0x020343B4  get automation  doc +0x40296 + pattern*676 + lane*64  + step*4
```

A step record is `note, gate, b2, b3` with a negative note meaning no note -
the same shape the sequencer consumes. An automation record is
`raw[2], knob[2]`, and `raw == -1` is the "no automation here" case
`ds10_lane_map` already handles.

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

Not verified: the meaning of the unnamed regions (pattern header, pattern
parameters, the three trailing blocks). They round-trip, but nothing here says
what they are.
