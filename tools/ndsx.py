#!/usr/bin/env python3
"""ndsx - Nintendo DS ROM recon/extraction for ds10plus.nds.

Subcommands:
  header            parse the NDS/DSi header
  fs                list NitroFS (name, file id, rom offset, size)
  extract [outdir]  write arm9.bin (BLZ-decompressed), arm7.bin, overlays, all NitroFS files
  patches           dump the 24 ds10synth*.dat patches and show which byte columns vary
  regs              classify hardware-register literals in arm9.bin / arm7.bin
  autoload          decode ModuleParams + autoload lists -> real memory map

Nothing here is specific to DS-10 except `patches`; the rest is generic NDS.
"""
import os
import struct
import sys

ROM = os.environ.get("DS10_ROM", "ds10plus.nds")


def load():
    with open(ROM, "rb") as f:
        return f.read()


def u8(d, o):
    return d[o]


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


# ---------------------------------------------------------------- header

def parse_header(d):
    h = {
        "title": d[0x00:0x0C].rstrip(b"\0").decode("ascii", "replace"),
        "gamecode": d[0x0C:0x10].decode("ascii", "replace"),
        "makercode": d[0x10:0x12].decode("ascii", "replace"),
        "unitcode": u8(d, 0x12),
        "arm9_off": u32(d, 0x20), "arm9_entry": u32(d, 0x24),
        "arm9_load": u32(d, 0x28), "arm9_size": u32(d, 0x2C),
        "arm7_off": u32(d, 0x30), "arm7_entry": u32(d, 0x34),
        "arm7_load": u32(d, 0x38), "arm7_size": u32(d, 0x3C),
        "fnt_off": u32(d, 0x40), "fnt_size": u32(d, 0x44),
        "fat_off": u32(d, 0x48), "fat_size": u32(d, 0x4C),
        "ov9_off": u32(d, 0x50), "ov9_size": u32(d, 0x54),
        "ov7_off": u32(d, 0x58), "ov7_size": u32(d, 0x5C),
        "rom_size": u32(d, 0x80), "hdr_size": u32(d, 0x84),
        # DSi extension
        "arm9i_off": u32(d, 0x1C0), "arm9i_load": u32(d, 0x1C8),
        "arm9i_size": u32(d, 0x1CC),
        "arm7i_off": u32(d, 0x1D0), "arm7i_load": u32(d, 0x1D8),
        "arm7i_size": u32(d, 0x1DC),
        "dsi_total_rom_size": u32(d, 0x210),
    }
    h["n_files"] = h["fat_size"] // 8
    h["n_ov9"] = h["ov9_size"] // 32
    h["n_ov7"] = h["ov7_size"] // 32
    return h


def cmd_header(d):
    h = parse_header(d)
    for k, v in h.items():
        print(f"{k:20} {v:#x} ({v})" if isinstance(v, int) else f"{k:20} {v}")
    print(f"{'file_size':20} {len(d):#x} ({len(d)})")
    print(f"{'arm9_compressed':20} {is_blz(d[h['arm9_off']:h['arm9_off'] + h['arm9_size']])}")
    print(f"{'arm7_compressed':20} {is_blz(d[h['arm7_off']:h['arm7_off'] + h['arm7_size']])}")


# ---------------------------------------------------------------- NitroFS

def fs_list(d):
    """Walk the FNT directory tree, resolving each file through the FAT."""
    h = parse_header(d)
    fnt, fat = h["fnt_off"], h["fat_off"]
    out = []

    def walk(dir_id, path):
        ent = fnt + 8 * (dir_id & 0xFFF)
        p = fnt + u32(d, ent)
        fid = u16(d, ent + 4)
        while d[p]:
            t = d[p]
            p += 1
            ln = t & 0x7F
            name = d[p:p + ln].decode("ascii", "replace")
            p += ln
            if t & 0x80:                      # subdirectory
                sub = u16(d, p)
                p += 2
                walk(sub, path + name + "/")
            else:                             # file
                start = u32(d, fat + 8 * fid)
                end = u32(d, fat + 8 * fid + 4)
                out.append((path + name, fid, start, end - start))
                fid += 1

    walk(0xF000, "/")
    return out


def cmd_fs(d):
    files = fs_list(d)
    for name, fid, off, size in files:
        print(f"{name:56} id={fid:3} off={off:#09x} size={size}")
    print(f"\n{len(files)} files")


# ---------------------------------------------------------------- BLZ

def is_blz(buf):
    """Heuristic: BLZ-compressed ARM binaries carry a trailing inc_len != 0."""
    if len(buf) < 8:
        return False
    inc = struct.unpack_from("<I", buf, len(buf) - 4)[0]
    enc = struct.unpack_from("<I", buf, len(buf) - 8)[0] & 0xFFFFFF
    return inc != 0 and 0 < enc <= len(buf)


def blz_decode(buf):
    """BackwardsLZ (CUE's blz.c) as used for NDS ARM9 static modules.

    Footer: [-8..-5]=enc_len|hdr_len<<24, [-5]=hdr_len, [-4..]=inc_len.
    The compressed region is stored reversed; decode forward, then reverse.
    """
    pak_len = len(buf)
    inc = struct.unpack_from("<I", buf, pak_len - 4)[0]
    if inc == 0:
        return bytes(buf[:pak_len - 4])
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
            flags = seg[p]
            p += 1
            mask = 0x80
        if not flags & mask:
            if p >= len(seg):
                break
            raw[r] = seg[p]
            p += 1
            r += 1
        else:
            if p + 1 >= len(seg):
                break
            pos = (seg[p] << 8) | seg[p + 1]
            p += 2
            n = min((pos >> 12) + 3, raw_len - r)
            pos = (pos & 0xFFF) + 3
            for _ in range(n):
                raw[r] = raw[r - pos]
                r += 1
    tail = raw[dec_len:]
    tail.reverse()
    raw[dec_len:] = tail
    return bytes(raw)


# ---------------------------------------------------------------- extract

def cmd_extract(d, outdir="out"):
    h = parse_header(d)
    os.makedirs(outdir, exist_ok=True)

    a9 = d[h["arm9_off"]:h["arm9_off"] + h["arm9_size"]]
    a9d = blz_decode(a9) if is_blz(a9) else a9
    write(f"{outdir}/arm9.bin", a9d, f"load={h['arm9_load']:#x}")
    write(f"{outdir}/arm7.bin", d[h["arm7_off"]:h["arm7_off"] + h["arm7_size"]],
          f"load={h['arm7_load']:#x}")

    # ARM9 overlays: 32-byte table entries {id, ram_addr, ram_size, bss, sinit*, ...}
    for i in range(h["n_ov9"]):
        e = h["ov9_off"] + 32 * i
        oid, ram, size = u32(d, e), u32(d, e + 4), u32(d, e + 8)
        comp = u32(d, e + 0x1C)
        start = u32(d, h["fat_off"] + 8 * oid)
        end = u32(d, h["fat_off"] + 8 * oid + 4)
        blob = d[start:end]
        if comp & 0xFFFFFF:                   # compressed_size field set -> BLZ
            blob = blz_decode(blob)
        write(f"{outdir}/overlay9_{i}.bin", blob, f"ram={ram:#x} size={size:#x}")

    if h["arm9i_size"]:
        write(f"{outdir}/arm9i.bin",
              d[h["arm9i_off"]:h["arm9i_off"] + h["arm9i_size"]],
              f"load={h['arm9i_load']:#x} (may be modcrypt-encrypted)")
    if h["arm7i_size"]:
        write(f"{outdir}/arm7i.bin",
              d[h["arm7i_off"]:h["arm7i_off"] + h["arm7i_size"]],
              f"load={h['arm7i_load']:#x} (may be modcrypt-encrypted)")

    n = 0
    for name, _fid, off, size in fs_list(d):
        dst = os.path.join(outdir, "nitrofs", name.lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(d[off:off + size])
        n += 1
    print(f"{outdir}/nitrofs/: {n} files")


def write(path, data, note=""):
    with open(path, "wb") as f:
        f.write(data)
    print(f"{path}: {len(data)} bytes {note}")


# ---------------------------------------------------------------- patches

def cmd_patches(d):
    pats = []
    for name, _fid, off, size in fs_list(d):
        if "ds10synth" in name:
            pats.append((name.rsplit("/", 1)[1], d[off:off + size]))
    if not pats:
        print("no ds10synth*.dat found")
        return
    n = len(pats[0][1])
    print(f"{len(pats)} patches, {n} bytes each\n")
    print("     " + " ".join(f"{c:02x}" for c in range(n)))
    for name, p in pats:
        print(f"{name[9:11]}   {p.hex(' ')}   {ascii_of(p[16:22])}")
    var = [len({p[c] for _n, p in pats}) for c in range(n)]
    print("\ndistinct values per byte column (1 = constant across all patches):")
    print("     " + " ".join(f"{v:2}" for v in var))
    const = [c for c in range(n) if var[c] == 1]
    print(f"\nconstant columns: {const}")


def ascii_of(b):
    return "".join(chr(c) if 32 <= c < 127 else "." for c in b)


# ---------------------------------------------------------------- autoload

NITROCODE = 0xDEC00621
NITROCODE_LE = 0x2106C0DE


def find_module_params(b):
    """ModuleParams ends with {nitrocode, nitrocode_le}; the pair sits at +0x18."""
    pat = struct.pack("<II", NITROCODE, NITROCODE_LE)
    i = b.find(pat)
    if i < 0 or i < 0x18:
        return None
    o = i - 0x18
    return {
        "off": o,
        "autoload_list_start": u32(b, o + 0x00),
        "autoload_list_end": u32(b, o + 0x04),
        "autoload_start": u32(b, o + 0x08),
        "static_bss_end": u32(b, o + 0x0C),
        "compressed_static_end": u32(b, o + 0x10),
        "sdk_version": u32(b, o + 0x14),
    }


def memory_map(b, load):
    """Resolve a static module into (static, autoload_blocks, list_range).

    Observed ModuleParams semantics for this ROM (all cross-checked below):
      +0x00 autoload_list_end   == load + len(module)
      +0x04 autoload_data_start (== +0x08 here)
      +0x0c static_bss_end
      +0x10 compressed_static_end (0 => module was not BLZ-compressed)

    Autoload descriptors are 16 bytes, not the commonly documented 12:
      {u32 dst, u32 data_size, u32 bss_dst, u32 bss_size}
    They sit between the end of the autoload payload and the end of the
    module. Validation: sum(data_size) must exactly equal
    (list_start - autoload_data_start). Raises if that does not hold.
    """
    mp = find_module_params(b)
    if not mp:
        return None
    end = u32(b, mp["off"] + 0x00)
    data_start = u32(b, mp["off"] + 0x04)
    if end != load + len(b):
        raise ValueError(f"module end mismatch: {end:#x} != {load + len(b):#x}")

    # Walk descriptors backwards from the module end until the payload sizes
    # exactly account for the gap between data_start and the descriptor list.
    for n in range(1, 33):
        ls = len(b) - 16 * n
        if ls < 0:
            break
        ents = [(u32(b, ls + 16 * i), u32(b, ls + 16 * i + 4),
                 u32(b, ls + 16 * i + 8), u32(b, ls + 16 * i + 12))
                for i in range(n)]
        if sum(e[1] for e in ents) == ls - (data_start - load):
            blocks, cur = [], data_start - load
            for dst, size, bss_dst, bss_size in ents:
                blocks.append((dst, size, bss_dst, bss_size, cur))
                cur += size
            return {"mp": mp, "static": (load, 0, data_start - load),
                    "blocks": blocks, "list": (load + ls, end)}
    raise ValueError("no consistent autoload descriptor count found")


def cmd_autoload(d, outdir="out"):
    for fn, load in (("arm9.bin", 0x2004000), ("arm7.bin", 0x2380000)):
        path = os.path.join(outdir, fn)
        if not os.path.exists(path):
            print(f"{path} missing - run `ndsx.py extract` first")
            continue
        b = open(path, "rb").read()
        print(f"\n=== {fn} (load {load:#x}, {len(b)} bytes) ===")
        m = memory_map(b, load)
        mp = m["mp"]
        print(f"  ModuleParams @ {load + mp['off']:#x}  "
              f"static_bss_end={mp['static_bss_end']:#x}  "
              f"compressed={'BLZ' if mp['compressed_static_end'] else 'no'}")
        _, so, se = m["static"]
        print(f"  STATIC    {load:#010x}..{load + se:#010x}  "
              f"file[{so:#x}..{se:#x}]  (bss to {mp['static_bss_end']:#x})")
        for dst, size, bdst, bsize, off in m["blocks"]:
            print(f"  AUTOLOAD  {dst:#010x}..{dst + size:#010x}  "
                  f"file[{off:#x}..{off + size:#x}]  bss {bdst:#x}+{bsize:#x}"
                  f"  <- {region_name(dst, load == 0x2380000)}")
            sp = os.path.join(outdir, f"{fn[:4]}_{dst:08x}.bin")
            with open(sp, "wb") as f:
                f.write(b[off:off + size])
            print(f"            wrote {sp}")
        print(f"  DESCRIPTORS {m['list'][0]:#x}..{m['list'][1]:#x} "
              f"({(m['list'][1] - m['list'][0]) // 16} x 16 bytes)")


def region_name(a, arm7=False):
    if arm7:
        for lo, hi, n in (
            (0x02000000, 0x03000000, "main RAM (mirrored, & 0x3fffff)"),
            (0x03000000, 0x03800000, "shared WRAM mirror"),
            (0x03800000, 0x03810000, "ARM7 WRAM"),
        ):
            if lo <= a < hi:
                return n
        return "?"
    for lo, hi, n in (
        (0x01000000, 0x02000000, "ARM9 ITCM (mirrored)"),
        (0x02000000, 0x02400000, "main RAM"),
        (0x02FE0000, 0x03000000, "ARM9 DTCM"),
        (0x037F8000, 0x03810000, "ARM7 WRAM"),
        (0x03000000, 0x03800000, "shared WRAM"),
    ):
        if lo <= a < hi:
            return n
    return "?"


# ---------------------------------------------------------------- regs

# 0x4000400..0x40005CC is the 3D geometry engine on ARM9 but the sound
# channel block on ARM7 - the same addresses mean different hardware per CPU.
GX = {0x4000400: "GXFIFO", 0x4000440: "MTX_MODE", 0x4000444: "MTX_PUSH",
      0x4000448: "MTX_POP", 0x4000454: "MTX_IDENTITY", 0x400046C: "MTX_TRANS",
      0x4000470: "MTX_SCALE", 0x40004A4: "POLYGON_ATTR",
      0x40004A8: "TEXIMAGE_PARAM", 0x40004AC: "PLTT_BASE",
      0x40004C0: "DIF_AMB", 0x40004D0: "SHININESS",
      0x4000500: "SWAP_BUFFERS", 0x4000504: "VIEWPORT"}
SND = {0x4000500: "SOUNDCNT", 0x4000501: "SOUNDCNT+1(master en)",
       0x4000504: "SOUNDBIAS", 0x4000508: "SNDCAP0CNT", 0x4000509: "SNDCAP1CNT",
       0x4000510: "SNDCAP0DAD", 0x4000518: "SNDCAP1DAD"}
IPC = {0x4000180: "IPCSYNC", 0x4000184: "IPCFIFOCNT", 0x4100000: "IPCFIFORECV"}
WIFI_LO, WIFI_HI = 0x4800000, 0x4900000


def cmd_regs(d, outdir="out"):
    import collections
    for fn, tbl, label in (("arm9.bin", GX, "3D/GX"), ("arm7.bin", SND, "SOUND")):
        path = os.path.join(outdir, fn)
        if not os.path.exists(path):
            print(f"{path} missing - run `ndsx.py extract` first")
            continue
        b = open(path, "rb").read()
        hits = collections.Counter()
        for o in range(0, len(b) - 4, 4):
            v = struct.unpack_from("<I", b, o)[0]
            if v in IPC:
                hits["IPC  " + IPC[v]] += 1
            elif WIFI_LO <= v < WIFI_HI:
                hits[f"WIFI {v:#x}"] += 1
            elif 0x4000400 <= v < 0x40005D0:
                hits[f"{label} {tbl.get(v, hex(v))}"] += 1
        print(f"\n{fn}:")
        for k, c in sorted(hits.items()):
            print(f"  {k:30} x{c}")
        print(f"  (total WIFI refs: {sum(c for k, c in hits.items() if k.startswith('WIFI'))})")


# ---------------------------------------------------------------- main

def main():
    cmds = {"header": cmd_header, "fs": cmd_fs, "extract": cmd_extract,
            "patches": cmd_patches, "regs": cmd_regs, "autoload": cmd_autoload}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    if not os.path.exists(ROM):
        print(f"ROM not found: {ROM} (set DS10_ROM)")
        return 1
    cmds[sys.argv[1]](load(), *sys.argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
