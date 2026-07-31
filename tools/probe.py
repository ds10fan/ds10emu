#!/usr/bin/env python3
"""probe - ad-hoc analysis passes over the extracted binaries.

These are the exploratory queries from the initial recon, kept runnable so
findings can be re-checked after any change to the extraction.

    python3 tools/probe.py branchcheck   # validate the autoload memory map
    python3 tools/probe.py monotonic     # find lookup-table candidates
    python3 tools/probe.py tables        # dump + verify the ARM9 DSP tables
    python3 tools/probe.py knobs         # check knob table vs factory patches
    python3 tools/probe.py xref <addr>   # literal-pool references to an address
    python3 tools/probe.py muls          # multiply-instruction density per blob
"""
import os
import re
import struct
import subprocess
import sys

OUT = "out"

# blob -> load address, from `ndsx.py autoload`
BLOBS = {
    "arm9.bin": 0x02004000,
    "arm9_01ff8000.bin": 0x01FF8000,
    "arm9_02fe0000.bin": 0x02FE0000,
    "arm7.bin": 0x02380000,
    "arm7_037f8000.bin": 0x037F8000,
    "arm7_02fe0000.bin": 0x02FE0000,
    "overlay9_0.bin": 0x02160AC0,
}

# Address ranges that a legitimate ARM7 `bl` may target.
ARM7_VALID = [(0x037F8000, 0x03809414), (0x02FE0000, 0x02FF750C),
              (0x02380000, 0x02380644), (0x00000000, 0x00004000)]


def load(fn):
    with open(os.path.join(OUT, fn), "rb") as f:
        return f.read()


def disasm(fn, base, cpu="arm7tdmi"):
    """objdump a raw blob at a given VMA, caching the result next to it."""
    dst = os.path.join(OUT, fn.replace(".bin", "") + f".{base:08x}.asm")
    if not os.path.exists(dst):
        with open(dst, "w") as f:
            subprocess.run(["arm-none-eabi-objdump", "-D", "-b", "binary",
                            "-m", cpu, f"--adjust-vma={base:#x}",
                            os.path.join(OUT, fn)],
                           stdout=f, stderr=subprocess.DEVNULL, check=True)
    return dst


def cmd_branchcheck():
    """Show that the naive single-base ARM7 load produces a bogus call graph.

    Disassembling the whole ARM7 module at 0x02380000 puts ~99% of its `bl`
    targets outside any mapped range, because 70KB of it belongs at 0x037F8000
    and 95KB at 0x02FE0000. Splitting by the autoload map fixes it.
    """
    cases = [("arm7.bin", 0x02380000, "whole module at one base (WRONG)"),
             ("arm7_037f8000.bin", 0x037F8000, "autoload block, correct base"),
             ("arm7_02fe0000.bin", 0x02FE0000, "autoload block, correct base")]
    for fn, base, note in cases:
        path = disasm(fn, base)
        tot = bad = 0
        for ln in open(path):
            m = re.search(r"\tbl\s+0x([0-9a-f]+)", ln)
            if not m:
                continue
            t = int(m.group(1), 16)
            tot += 1
            if not any(lo <= t < hi for lo, hi in ARM7_VALID):
                bad += 1
        pct = 100 * bad / max(tot, 1)
        print(f"  {fn:24} @{base:#010x}  {tot:5} bl, {bad:5} out of range "
              f"({pct:5.1f}%)  {note}")


def cmd_monotonic():
    """Rising runs of u8 and u16 - a cheap lookup-table detector."""
    for fn, base in BLOBS.items():
        b = load(fn)
        for width, fmt, minlen in ((2, "<H", 24), (1, "B", 24)):
            n = len(b) // width
            get = (lambda i: b[i]) if width == 1 else \
                  (lambda i: struct.unpack_from(fmt, b, 2 * i)[0])
            runs, i = [], 0
            while i < n - minlen:
                j = i + 1
                while j < n and get(j) >= get(j - 1):
                    j += 1
                if j - i >= minlen:
                    runs.append((base + width * i, j - i, get(i), get(j - 1)))
                i = max(j, i + 1)
            if runs:
                print(f"\n{fn} u{width * 8}: {len(runs)} rising runs >= {minlen}")
                for a, ln_, lo, hi in sorted(runs, key=lambda t: -t[1])[:8]:
                    print(f"  {a:#010x} len={ln_:4} {lo:#7x}..{hi:#7x}")


# The ARM9 DSP tables. These are u32 - an earlier u16 scan read alternating
# low/high halves and manufactured six plausible-looking tables that did not
# exist. Names and addresses are thement's; see recon.md sections 0 and 5.
ARM9_TABLES = [
    (0x0207E950, 128, "pitchtable"),
    (0x0207ED38, 128, "drumpitch"),
    (0x020807C4, 151, "filter_coeff"),
]
SEMITONE = 2 ** (1 / 12)


def cmd_tables():
    """Dump the ARM9 DSP tables as u32 and check the semitone ratio.

    All three advance by 2^(1/12) per entry. filter_coeff departs from that
    near the top because it saturates toward 0x7FFFFFFF as cutoff approaches
    Nyquist - not a different curve.
    """
    b, base = load("arm9.bin"), BLOBS["arm9.bin"]
    rd = lambda a, n: [struct.unpack_from("<I", b, a - base + 4 * i)[0]
                       for i in range(n)]

    for addr, n, nm in ARM9_TABLES:
        v = rd(addr, n + 1)
        ratios = [v[i + 1] / v[i] for i in range(n - 1) if v[i]]
        near = sum(1 for r in ratios if abs(r - SEMITONE) < 1e-4)
        print(f"\n{nm} @ {addr:#010x}  {n} x u32")
        print(f"  head {v[:6]}")
        print(f"  tail {v[n - 4:n]}   (entry {n} = {v[n]}, outside the table)")
        print(f"  ratio == 2^(1/12) within 1e-4 for {near}/{len(ratios)} steps"
              f"   max {max(v[:n])} (0x7FFFFFFF = {0x7FFFFFFF})")


KNOBS = [
    ("octave", -2, 2), ("egint.pitch", -63, 63), ("porta", 0, 127),
    ("pitch.in", -63, 63), ("pitch1.in", -63, 63), ("pitch2.in", -63, 63),
    ("vco1", 0, 3), ("vco2", 0, 3), ("vco2pitch", -63, 63),
    ("balance", 0, 127), ("vcosync", 0, 1), ("cutoff", 0, 127),
    ("peak", 0, 127), ("vcftype", 0, 2), ("egint.cutoff", -63, 63),
    ("cutoff.in", -63, 63), ("vcaeg", 0, 1), ("vca.in", -63, 63),
    ("drive", 0, 127), ("level", 0, 127), ("attack", 0, 127),
    ("decay", 0, 127), ("sustain", 0, 127), ("release", 0, 127),
    ("mg.freq", 0, 127), ("mg.bpm", 0, 1), ("pitch.in.src", 0, 6),
    ("pitch1.in.src", 0, 6), ("pitch2.in.src", 0, 6),
    ("cutoff.in.src", 0, 6), ("vca.in.src", 0, 6),
]
PATCH_PARAM_OFF = 0x20


def cmd_knobs():
    """Check thement's knob table against the 24 factory patches.

    Patch bytes 0x20..0x3E are knob values 0..30 in this order. Every range
    must be consistent with every factory patch, read as u8 or s8.
    """
    pats = sorted(f for f in os.listdir(os.path.join(OUT, "nitrofs/data/demo"))
                  if f.startswith("ds10synth"))
    data = [open(os.path.join(OUT, "nitrofs/data/demo", f), "rb").read()
            for f in pats]
    print(f"{len(data)} patches, params at +{PATCH_PARAM_OFF:#x}, "
          f"{len(KNOBS)} knobs")
    bad = 0
    for k, (nm, lo, hi) in enumerate(KNOBS):
        col = PATCH_PARAM_OFF + k
        raw = [p[col] for p in data]
        sgn = [v - 256 if v > 127 else v for v in raw]
        enc = "u8" if all(lo <= v <= hi for v in raw) else \
              ("s8" if all(lo <= v <= hi for v in sgn) else "MISMATCH")
        if enc == "MISMATCH":
            bad += 1
        use = raw if enc == "u8" else sgn
        print(f"  {col:#04x} knob{k:<3}{nm:<15}[{lo:>4}..{hi:<4}] "
              f"distinct={len(set(raw)):<3} {min(use):>4}..{max(use):<4} {enc}")
    print(f"\n{len(KNOBS) - bad}/{len(KNOBS)} consistent")
    return 1 if bad else 0


def cmd_xref(addr):
    """Literal-pool references to an address, across every blob."""
    target = int(addr, 0)
    for fn, base in BLOBS.items():
        b = load(fn)
        for o in range(0, len(b) - 4, 4):
            v = struct.unpack_from("<I", b, o)[0]
            if target - 8 <= v < target + 16:
                print(f"  {fn:24} literal @ {base + o:#010x} -> {v:#x}")


def cmd_muls():
    """Multiply-instruction density - a weak hint at where DSP code lives."""
    pat = re.compile(r"\t(mul|mla|smull|smlal|umull|umlal)\b")
    for fn, base in BLOBS.items():
        path = disasm(fn, base)
        tot = mul = 0
        for ln in open(path):
            if "\t" in ln:
                tot += 1
            if pat.search(ln):
                mul += 1
        print(f"  {fn:24} {mul:5} muls / {tot:7} lines  "
              f"({100 * mul / max(tot, 1):.2f}%)")


def main():
    cmds = {"branchcheck": cmd_branchcheck, "monotonic": cmd_monotonic,
            "tables": cmd_tables, "xref": cmd_xref, "muls": cmd_muls,
            "knobs": cmd_knobs}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    cmds[sys.argv[1]](*sys.argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
