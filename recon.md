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

### The drum parts use the same engine

The 24 names are:

```
INIT  BASS1..BASS7  LEAD1..LEAD4  UFO  THUNDR  CPU 1  PHONE      (16 tonal)
KICK1 KICK2 SNARE1 SNARE2 SNARE3 HIHAT TOM ZAP                   ( 8 percussive)
```

The percussion patches use the **same 63-byte format** as the tonal ones. That
means the drum parts are the same virtual-analog voice driven by different
parameters, not sample playback — so there is one engine to reverse, not two.

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

### ARM9 = GUI (hardware 3D) + the DSP tables — confirmed tables, hypothesis on the DSP

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

**A dense cluster of monotonic curve tables sits in ARM9 at ~0x0207A000–0x02081000:**

```
0x0207A360   62 x u16   0 .. 0x1000     (mostly zeros - probably a false positive)
0x0207E3D8  129 x u16   0 .. 0x2000     0x2000 = 1.0 in Q13
0x0207EF44  127 x u16   0 .. 0x7FFF
0x0207F042  130 x u16   0 .. 0xDF6A
0x020803C2   65 x u16   0 .. 0xFEF8
0x02080400  226 x s16   -13614 .. +32767, zero crossing at 0x02080444
```

The `0x02080400` region is one contiguous signed table; `0x02080444` (64 entries)
and `0x020804C4` (128 entries) are the *same curve at two resolutions*
(`C[i] == A[2i+1]`), and a single function references both via adjacent literals
at `0x02075454` and `0x02075458`. **That function is the best entry point for
Phase 1.**

Shape fitting was inconclusive — mean |error| / full scale against simple models:

```
0x0207EF44:  linear 0.165  quarter-sine 0.030  half-sine 0.295   (max err still 5.6%)
0x02080400:  increments grow, but the growth ratio is not constant (1.030 -> 1.025)
             so not a clean exponential either
```

So: **these are unidentified.** They could be waveform/wavetable data, pitch or
gain curves, or filter coefficients. Curve fitting is the wrong tool — settle it
by xref (what indexes them, and how often) plus differential probing against the
emulation oracle. A table read once per note is a parameter curve; one read in a
tight loop off a phase accumulator is a waveform.

**Open question, highest priority: is the audio render loop on the ARM9 or the
ARM7?** The table cluster and multiply density say ARM9; the sound hardware is
ARM7-only, so there must be a buffer handoff over the IPC FIFO either way.
`tools/emu.py` can settle this by hooking writes and looking for the routine
that fills a large block of sequential 16-bit samples.

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

1. **ARM9 or ARM7 for the audio render loop?** Blocks all synth work. Method in
   §5.
2. What are the `0x0207A000–0x02081000` tables? Start from the function at
   `0x02075454`.
3. Are patch bytes `0x00..0x0F` a checksum? If so, which algorithm — needed to
   write patches the original will load.
4. Song/pattern layout in the 12044-byte `ds10init.dat`. Diffing it against the
   demo songs should expose most of it.
5. Sample rate and block size of the render loop, and how buffers reach the
   ARM7 (IPC FIFO command? shared WRAM ring?).
