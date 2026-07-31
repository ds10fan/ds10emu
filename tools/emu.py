#!/usr/bin/env python3
"""Unicorn harness: run original DS-10 ARM code in isolation as a test oracle.

This is the backbone of verification. Once a DSP routine is located, the same
inputs are fed to (a) the original ARM code running here and (b) the C
reimplementation; the outputs must match bit-for-bit. Everything is integer
fixed point (neither ARM7TDMI nor ARM946E-S has an FPU), so bit-exactness is
achievable rather than approximate.

Usage:
    python3 tools/emu.py smoke              # prove the memory image is sane
    python3 tools/emu.py call <addr> [args...]
"""
import os
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_INSN_INVALID, UC_HOOK_MEM_INVALID,
                     UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_CPU_ARM_926, UC_CPU_ARM_946)

OUT = "out"

# ARM9 and ARM7 do NOT share an address space: 0x02FE0000 is the ARM9's
# private DTCM but on the ARM7 it is a mirror of main RAM (& 0x3fffff ->
# 0x023E0000). Both CPUs have an autoload block there, so each CPU needs its
# own image. Regions are 4K-aligned because Unicorn requires it.
COMMON = [
    (0x02000000, 0x00400000, "main ram (4M)"),
    (0x03000000, 0x00010000, "shared wram"),
    (0x04000000, 0x00002000, "io registers (stubbed)"),
    (0x0F000000, 0x00010000, "harness scratch (stack, buffers)"),
]
CPUS = {
    "arm9": {
        "model": UC_CPU_ARM_946,
        "regions": COMMON + [
            (0x01FF8000, 0x00008000, "arm9 itcm"),
            (0x02FE0000, 0x00020000, "arm9 dtcm"),
            (0x06000000, 0x00100000, "vram"),
            (0x07000000, 0x00002000, "oam"),
        ],
        # (blob, load addr, byte limit) - limit trims autoload payload off the
        # static module, which `ndsx.py autoload` has already split out.
        "images": [
            ("arm9.bin", 0x02004000, 0x84100),
            ("arm9_01ff8000.bin", 0x01FF8000, None),
            ("arm9_02fe0000.bin", 0x02FE0000, None),
            ("overlay9_0.bin", 0x02160AC0, None),
        ],
        "entry": 0x02004800,
    },
    "arm7": {
        "model": UC_CPU_ARM_926,          # ARM7TDMI is ARMv4T; 926 decodes it
        "regions": COMMON + [
            (0x02FE0000, 0x00020000, "main ram mirror (== 0x023E0000)"),
            (0x037F8000, 0x00018000, "shared wram mirror + arm7 wram"),
        ],
        "images": [
            ("arm7.bin", 0x02380000, 0x644),
            ("arm7_037f8000.bin", 0x037F8000, None),
            ("arm7_02fe0000.bin", 0x02FE0000, None),
        ],
        "entry": 0x02380000,
    },
}

STACK_TOP = 0x0F008000
MAGIC_LR = 0x0F00FFF0          # returning here means "function finished"


def build(cpu="arm7", trace_io=False):
    """Map memory for one CPU and load its blocks at their real addresses."""
    spec = CPUS[cpu]
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
    try:
        uc.ctl_set_cpu_model(spec["model"])
    except UcError:
        pass                              # older unicorn: generic ARM core
    for base, size, _name in spec["regions"]:
        uc.mem_map(base, size)

    loaded = []
    for fn, addr, limit in spec["images"]:
        p = os.path.join(OUT, fn)
        if not os.path.exists(p):
            continue
        data = open(p, "rb").read()
        if limit is not None:
            data = data[:limit]
        uc.mem_write(addr, data)
        loaded.append((fn, addr, len(data)))
        # ARM7 sees this block through the main-RAM mirror as well.
        if cpu == "arm7" and addr == 0x02FE0000:
            uc.mem_write(0x023E0000, data)

    # Unicorn/QEMU does not implement the ARM946E-S cp15 cache and TCM region
    # registers (c9,c1 etc.), so the DS boot code faults on them. Skipping cp15
    # entirely is safe here: the harness backs ITCM/DTCM with plain RAM already,
    # and the DSP routines under test never touch cp15.
    def on_insn_invalid(uc_, _u):
        pc = uc_.reg_read(UC_ARM_REG_PC)
        insn = struct.unpack("<I", uc_.mem_read(pc, 4))[0]
        if (insn & 0x0F000F10) == 0x0E000F10:      # MCR/MRC ..., p15, ...
            uc_.reg_write(UC_ARM_REG_PC, pc + 4)
            return True
        return False
    uc.hook_add(UC_HOOK_INSN_INVALID, on_insn_invalid)

    io = []
    if trace_io:
        def on_invalid(uc_, access, address, size, value, _u):
            io.append((access, address, size, value))
            return False
        uc.hook_add(UC_HOOK_MEM_INVALID, on_invalid)
    return uc, loaded, io


def call(uc, addr, args=(), max_insns=2_000_000):
    """Call addr(args...) with an AAPCS-ish setup; return r0."""
    regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
    for r, v in zip(regs, list(args) + [0] * 4):
        uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, MAGIC_LR)
    uc.emu_start(addr, MAGIC_LR, count=max_insns)
    return uc.reg_read(UC_ARM_REG_R0)


def cmd_smoke():
    """Load each CPU image and execute from its reset vector.

    This does not run the game (no hardware, no IRQs, no BIOS). It only proves
    the memory map from `ndsx.py autoload` is coherent: instructions decode and
    execution proceeds for a while instead of immediately faulting on garbage.
    """
    for cpu in ("arm9", "arm7"):
        uc, loaded, io = build(cpu, trace_io=True)
        print(f"\n=== {cpu} ===")
        for fn, addr, n in loaded:
            print(f"  {fn:24} @ {addr:#010x}  {n} bytes")
        uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        uc.reg_write(UC_ARM_REG_LR, MAGIC_LR)
        entry = CPUS[cpu]["entry"]
        try:
            uc.emu_start(entry, 0, count=2000)
            print(f"  entry {entry:#x}: ran 2000 insns, "
                  f"pc={uc.reg_read(UC_ARM_REG_PC):#x}")
        except UcError as e:
            print(f"  entry {entry:#x}: stopped after "
                  f"pc={uc.reg_read(UC_ARM_REG_PC):#x} ({e})")
        if io:
            print(f"  unmapped accesses: {len(io)}")
            for acc, ad, sz, v in io[:6]:
                print(f"    access={acc} addr={ad:#x} size={sz} value={v:#x}")


def cmd_call(cpu, addr, *args):
    uc, _loaded, _io = build(cpu)
    r0 = call(uc, int(addr, 0), [int(a, 0) for a in args])
    print(f"r0 = {r0:#x} ({r0})")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    if sys.argv[1] == "smoke":
        cmd_smoke()
    elif sys.argv[1] == "call":
        cmd_call(*sys.argv[2:])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
