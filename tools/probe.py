#!/usr/bin/env python3
"""probe - ad-hoc analysis passes over the extracted binaries.

These are the exploratory queries from the initial recon, kept runnable so
findings can be re-checked after any change to the extraction.

    python3 tools/probe.py branchcheck   # validate the autoload memory map
    python3 tools/probe.py monotonic     # find lookup-table candidates
    python3 tools/probe.py tables        # dump + shape-fit the ARM9 curve tables
    python3 tools/probe.py xref <addr>   # literal-pool references to an address
    python3 tools/probe.py muls          # multiply-instruction density per blob
"""
import math
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


# The curve tables clustered in ARM9. Names are provisional - see recon.md.
ARM9_TABLES = [
    (0x0207A360, 62, "curve62"),
    (0x0207E3D8, 129, "curve129_to_0x2000"),
    (0x0207EF44, 127, "curve127_to_0x7fff"),
    (0x0207F042, 130, "curve130"),
    (0x020803C2, 65, "curve65"),
    (0x02080400, 226, "signed_curve226"),
]


def cmd_tables():
    """Dump the ARM9 curve tables and score them against simple models.

    None of them fit a clean linear / sine / exponential model, so these are
    NOT identified yet. Identification happens by xref plus differential
    probing through the Unicorn oracle, not by curve fitting.
    """
    b, base = load("arm9.bin"), BLOBS["arm9.bin"]

    def rd(a, n, signed=False):
        f = "<h" if signed else "<H"
        return [struct.unpack_from(f, b, a - base + 2 * i)[0] for i in range(n)]

    for addr, n, nm in ARM9_TABLES:
        signed = nm.startswith("signed")
        v = rd(addr, n, signed)
        mx = max(max(v), 1)
        models = {
            "linear": [min(v) + (mx - min(v)) * i // (n - 1) for i in range(n)],
            "quarter_sin": [round(mx * math.sin(i / (n - 1) * math.pi / 2))
                            for i in range(n)],
            "half_sin": [round(mx * math.sin(i / (n - 1) * math.pi))
                         for i in range(n)],
        }
        scores = " ".join(
            f"{k}={sum(abs(a - c) for a, c in zip(v, m)) / n / mx:.3f}"
            for k, m in models.items())
        print(f"\n{nm} @ {addr:#010x} n={n} {'s16' if signed else 'u16'}")
        print(f"  range {min(v)} .. {max(v)}   mean|err|/fs: {scores}")
        print(f"  head {v[:10]}")
        print(f"  tail {v[-6:]}")


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
            "tables": cmd_tables, "xref": cmd_xref, "muls": cmd_muls}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    cmds[sys.argv[1]](*sys.argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
