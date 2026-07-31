#!/usr/bin/env python3
"""oracle - render DS-10 audio by executing the original ARM9 code.

This is the differential-testing oracle from PLAN.md tier 1. It constructs a
real synth object using the game's own C++ constructor, loads a patch through
the game's own knob handler, posts note events, and calls the game's own render
handler. Nothing about the DSP is reimplemented here - the point is ground truth
to compare a reimplementation against.

    python3 tools/oracle.py probe [patch] [note]   # construct + render, trace
    python3 tools/oracle.py render [patch] [note] [frames]
    python3 tools/oracle.py wav <out.wav> [patch] [note] [frames]
    python3 tools/oracle.py waves [note]           # characterise vco1 = 0..3

Verified: output is deterministic across runs, and pitch tracks
440*2^((note-69)/12) at 32768 Hz to within measurement resolution.

Addresses come from recon.md section 10, cross-checked against our own
extraction (the dispatch tables at 0x02087CEC etc. match byte for byte).
"""
import collections
import os
import struct
import sys
import wave

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_PC

import emu

REG = {f"r{i}": getattr(__import__("unicorn.arm_const", fromlist=["x"]),
                        f"UC_ARM_REG_R{i}") for i in range(13)}

# --- probe points inside the per-sample loop of the render handler -----------
# Hooks fire before the instruction at that address executes, so each probe
# reads the register after the producing instruction has already run.
VCF_IN = (0x02075254, "r2")    # filter input  = (vco1*vol1 + vco2*vol2) >> 15
VCF_OUT = (0x02075334, "r5")   # filter output = selected node, already >> 9
VCO2_OUT = (0x0207534C, "r9")  # VCO2 output, still live at the AMP block
LOOP_HEAD = 0x02074F98

# --- ARM9 code addresses -----------------------------------------------------
SYNTH_NEW = 0x02075FF8      # alloc(0x54) + ctor; returns object in r0
SYNTH_CTOR = 0x02074360
SYNTH_MTABLE = 0x02087CEC
KNOB_SET = 0x02075648       # (this, knob_id, value)
RENDER = 0x02074720         # (this) - method table +0x0C
EVENT_DISPATCH = 0x0207546C  # method table +0x28, called by RENDER

# 0x0202927C / 0x020292A0 / 0x020292C4 / 0x020292E8 are thunks that tail-call
# the real allocator through a heap global at 0x021238C8. That global lives in
# bss, so it is NULL in a static image and the thunks fault on [NULL+0x14].
# Both the global and the real allocator therefore need stubbing.
NEW_THUNK = 0x0202927C
ALLOC = 0x020186B0          # (r0=heap id, r1=?, r2=size) -> r0 = ptr
DEALLOC = 0x020187B8
HEAP_GLOBAL = 0x021238C8

# --- harness memory ----------------------------------------------------------
ARENA_BASE = 0x02248000     # unused main RAM; also where the game's own objects
ARENA_PTR = 0x0F00E000      # happened to land, so addresses stay comparable
OUTBUF = 0x0F00C000
OUTBUF_SLOT = 0x0F00E010
HEAP_OBJ = 0x0F00E100       # dummy heap descriptor for the thunks to deref
FRAME = 64                  # samples per frame
RATE = 32768                # Hz

# Bump allocator in ARM code, patched over the game's allocator. Doing it as
# real ARM rather than a Unicorn hook keeps everything inside one emu_start.
# Size arrives in r2, result goes in r0. The 64-byte granularity mirrors the
# original's `add r1, r7, #63`, which is why its objects sit 0x40/0x80 apart.
ALLOC_STUB = struct.pack(
    "<8I",
    0xE59F1014,   # ldr  r1, [pc, #20]   -> &ARENA_PTR
    0xE5910000,   # ldr  r0, [r1]        -> return value
    0xE282203F,   # add  r2, r2, #63
    0xE3C2203F,   # bic  r2, r2, #63     (align 64)
    0xE0803002,   # add  r3, r0, r2
    0xE5813000,   # str  r3, [r1]
    0xE12FFF1E,   # bx   lr
    ARENA_PTR,
)
DEALLOC_STUB = struct.pack("<2I", 0xE3A00000, 0xE12FFF1E)   # mov r0,#0 ; bx lr

# Fields the constructor does NOT set - normally wired by the machine-setup code
# at 0x02032398. Values are from thement's dump of the live syn1 object at
# 0x022489A0 (recon.md section 0). Verified not to affect output: zeroing them
# produces identical samples, so they are outputs rather than inputs here.
# Reproducing the real wiring is follow-up work.
#   +0x04 event queue pointer        +0x1C pointer to the output buffer pointer
#   +0x08 pending event count (byte) +0x10 frame size in samples
SYN_FIELDS = {0x0C: 0x00000201, 0x10: FRAME, 0x14: 0x00007FD8,
              0x18: 0x00008000, 0x20: FRAME, 0x24: 0x00007FD8,
              0x28: 0x00000002, 0x2C: 0x00000075}

NOTEOFF, NOTEON = 1, 2


class Oracle:
    """One synth voice running the original ARM9 code."""

    def __init__(self, trace=False):
        uc, _loaded, _io = emu.build("arm9")
        uc.mem_map(0x0E000000, 0x00100000)
        uc.mem_write(ALLOC, ALLOC_STUB)
        uc.mem_write(DEALLOC, DEALLOC_STUB)
        self._w32(uc, ARENA_PTR, ARENA_BASE)
        uc.mem_write(HEAP_OBJ, b"\0" * 0x40)
        self._w32(uc, HEAP_GLOBAL, HEAP_OBJ)

        self.uc = uc
        self.writes = [] if trace else None
        if trace:
            def on_write(u, _acc, addr, size, val, _x):
                self.writes.append((u.reg_read(UC_ARM_REG_PC), addr, size, val))
            uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

        self._probes = {}
        self._probe_hooked = False
        self.probed = collections.defaultdict(list)
        self.syn = self._construct()

    # -- probes -------------------------------------------------------------
    def add_probe(self, name, pc, reg):
        """Log `reg` every time execution reaches `pc`, into self.probed[name]."""
        self._probes.setdefault(pc, []).append((name, REG[reg]))
        if not self._probe_hooked:
            def on_code(u, addr, _size, _x):
                for name_, rid in self._probes.get(addr, ()):
                    v = u.reg_read(rid)
                    self.probed[name_].append(v - (1 << 32) if v >> 31 else v)
            self.uc.hook_add(UC_HOOK_CODE, on_code)
            self._probe_hooked = True

    def add_snapshot(self, name, pc, addr, size):
        """Record a memory region the first time execution reaches `pc`."""
        def on_code(u, a, _size, _x):
            if a == pc and name not in self.snapshots:
                self.snapshots[name] = bytes(u.mem_read(addr, size))
        if not hasattr(self, "snapshots"):
            self.snapshots = {}
        self.uc.hook_add(UC_HOOK_CODE, on_code, begin=pc, end=pc)

    # -- low level ----------------------------------------------------------
    @staticmethod
    def _w32(uc, a, v):
        uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def u32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self._w32(self.uc, a, v)

    def call(self, addr, *args, limit=20_000_000):
        return emu.call(self.uc, addr, args, max_insns=limit)

    # -- setup --------------------------------------------------------------
    def _construct(self):
        syn = self.call(SYNTH_NEW)
        if not syn:
            raise RuntimeError("synth factory returned NULL")
        mt = self.u32(syn)
        if mt != SYNTH_MTABLE:
            raise RuntimeError(f"method table {mt:#x} != {SYNTH_MTABLE:#x}")
        self.evq = self.call(NEW_THUNK, 8 * 16)
        self.w32(syn + 0x04, self.evq)
        self.uc.mem_write(syn + 0x08, b"\0")
        for off, val in SYN_FIELDS.items():
            self.w32(syn + off, val)
        # +0x1C points TO the output buffer pointer:
        #   ldr r1,[this,#0x1C] ; ldr r1,[r1]
        self.w32(OUTBUF_SLOT, OUTBUF)
        self.w32(syn + 0x1C, OUTBUF_SLOT)
        return syn

    def subobjects(self):
        """{offset: pointer} for the sub-objects the constructor allocates."""
        return {o: self.u32(self.syn + o) for o in range(0x30, 0x50, 4)}

    def load_patch(self, params):
        for knob, val in enumerate(params):
            self.call(KNOB_SET, self.syn, knob, val)

    def event(self, opcode, note=0):
        """Events are 8 bytes: byte0 = opcode, byte4 = signed note."""
        self.uc.mem_write(self.evq, struct.pack("<Ii", opcode, note))
        self.uc.mem_write(self.syn + 0x08, bytes([1]))

    def note_on(self, note):
        self.event(NOTEON, note)

    def note_off(self):
        self.event(NOTEOFF)

    # -- render -------------------------------------------------------------
    def render(self, frames=1):
        out = []
        for _ in range(frames):
            self.call(RENDER, self.syn)
            out += struct.unpack(f"<{FRAME}h",
                                 self.uc.mem_read(OUTBUF, FRAME * 2))
        return out


def factory_patch(idx=1):
    """(name, 31 knob values) from ds10synthNN.dat. Bipolar knobs are signed."""
    p = os.path.join(emu.OUT, "nitrofs/data/demo", f"ds10synth{idx:02d}.dat")
    raw = open(p, "rb").read()
    name = raw[0x10:0x16].decode("latin1").rstrip("\x00\x08")
    params = [b - 256 if b > 127 else b for b in raw[0x20:0x3F]]
    return name, params


def play(idx, note, frames=1, trace=False, params=None):
    o = Oracle(trace=trace)
    name, p = factory_patch(idx)
    o.load_patch(params if params is not None else p)
    o.note_on(note)
    return o, name, o.render(frames)


# --- commands ----------------------------------------------------------------

def cmd_probe(idx="1", note="60"):
    o, name, s = play(int(idx), int(note), frames=1, trace=True)
    print(f"patch {name!r} note {note}")
    print(f"synth @ {o.syn:#010x}  event queue @ {o.evq:#010x}")
    print("  sub-objects: " + " ".join(f"+{k:#04x}={v:#010x}"
                                       for k, v in o.subobjects().items()))
    # Group writes by the instruction that made them; the audio store shows up
    # as one PC covering a contiguous 128-byte span.
    by_pc = collections.defaultdict(list)
    for pc, addr, size, _val in o.writes:
        by_pc[(pc, size)].append(addr)
    print(f"\n{len(o.writes)} writes from {len(by_pc)} instructions; "
          f"widest contiguous spans:")
    rows = []
    for (pc, size), addrs in by_pc.items():
        lo, hi = min(addrs), max(addrs)
        rows.append((hi - lo, pc, size, lo, hi, len(addrs)))
    for span, pc, size, lo, hi, n in sorted(rows, reverse=True)[:6]:
        tag = "  <- audio out" if lo == OUTBUF else ""
        print(f"  pc={pc:#010x} size={size} {n:4} writes "
              f"{lo:#010x}..{hi:#010x} (span {span}){tag}")
    print(f"\nfirst 16 samples: {s[:16]}")


def cmd_render(idx="1", note="60", frames="1"):
    _o, name, s = play(int(idx), int(note), int(frames))
    print(f"patch {name!r} note {note}: {len(s)} samples")
    print(f"  min {min(s)} max {max(s)} nonzero {sum(1 for v in s if v)}")
    print(f"  {s[:64]}")


def cmd_wav(path, idx="1", note="60", frames="64"):
    _o, name, s = play(int(idx), int(note), int(frames))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack(f"<{len(s)}h", *s))
    print(f"{path}: {name!r} note {note}, {len(s)} samples "
          f"({len(s) / RATE:.2f}s @ {RATE} Hz)")


# FILTER object field offsets, from the decompiled render loop. The signal
# format is Q24 clamped to +-(2^24-1); `f` and `damp` are the two state-variable
# coefficients, each linearly ramped across the frame by its *_delta.
VCF = {"q": 0x24, "band": 0x34, "low": 0x38, "type": 0x40,
       "f": 0x50, "f_delta": 0x54, "damp": 0x5C, "damp_delta": 0x60}
VCF_ORDER = ["q", "band", "low", "type", "f", "f_delta", "damp", "damp_delta"]
FILTER_PTR = 0x44          # synth + 0x44
VEC_MAGIC = b"VCF1"


def _s32(v):
    return v - (1 << 32) if v >> 31 else v


def capture_vcf(params, note, frames=4):
    """Per-sample filter input/output plus the filter state at each frame start.

    The state must be sampled at the loop head, not before RENDER: the
    per-frame preamble recomputes f/damp and their ramp deltas before the
    sample loop runs, so a snapshot taken earlier is one frame stale.
    """
    o = Oracle()
    fobj = o.u32(o.syn + FILTER_PTR)
    o.add_probe("in", *VCF_IN)
    o.add_probe("out", *VCF_OUT)

    snap = {"armed": False, "state": None}

    def on_loop_head(u, _a, _s, _x):
        if snap["armed"]:
            raw = bytes(u.mem_read(fobj, 0x64))
            snap["state"] = [_s32(struct.unpack_from("<I", raw, VCF[k])[0])
                             for k in VCF_ORDER]
            snap["armed"] = False
    o.uc.hook_add(UC_HOOK_CODE, on_loop_head, begin=LOOP_HEAD, end=LOOP_HEAD)

    o.load_patch(params)
    o.note_on(note)

    cases = []
    for _ in range(frames):
        o.probed["in"].clear()
        o.probed["out"].clear()
        snap["armed"] = True
        snap["state"] = None
        o.call(RENDER, o.syn)
        if snap["state"] is None:
            continue                        # loop did not run this frame
        cases.append((snap["state"], list(o.probed["in"]),
                      list(o.probed["out"])))
    return cases


# OSC object field offsets, from the decompiled oscillator block. Order must
# match ds10_vco in src/core/vco.h and the reader in tests/test_vco.c.
VCO = {"xmod": 0x0C, "phase1": 0x14, "phase2": 0x18, "inc1": 0x1C,
       "inc2": 0x20, "shape1": 0x24, "shape2": 0x28, "wave1": 0x2C,
       "wave2": 0x30, "vol1": 0x38, "vol2": 0x3C, "lfsr1": 0x40,
       "lfsr2": 0x44, "sync": 0x48, "prev2": 0x4C, "hold2": 0x50,
       "xm_60": 0x60, "xm_64": 0x64, "xm_68": 0x68, "xm_6c": 0x6C}
VCO_ORDER = ["xmod", "phase1", "phase2", "inc1", "inc2", "shape1", "shape2",
             "wave1", "wave2", "vol1", "vol2", "lfsr1", "lfsr2", "sync",
             "prev2", "hold2", "xm_60", "xm_64", "xm_68", "xm_6c"]
OSC_PTR = 0x40             # synth + 0x40


def capture_vco(params, note, frames=4):
    """Oscillator state at each frame start plus the mixed output per sample.

    The mix that feeds the filter is exactly the VCF_IN probe point, so no new
    probe is needed.
    """
    o = Oracle()
    oobj = o.u32(o.syn + OSC_PTR)
    o.add_probe("mix", *VCF_IN)

    snap = {"armed": False, "state": None}

    def on_loop_head(u, _a, _s, _x):
        if snap["armed"]:
            raw = bytes(u.mem_read(oobj, 0xA0))
            snap["state"] = [_s32(struct.unpack_from("<I", raw, VCO[k])[0])
                             for k in VCO_ORDER]
            snap["armed"] = False
    o.uc.hook_add(UC_HOOK_CODE, on_loop_head, begin=LOOP_HEAD, end=LOOP_HEAD)

    o.load_patch(params)
    o.note_on(note)

    cases = []
    for _ in range(frames):
        o.probed["mix"].clear()
        snap["armed"] = True
        snap["state"] = None
        o.call(RENDER, o.syn)
        if snap["state"] is None:
            continue
        cases.append((snap["state"], list(o.probed["mix"])))
    return cases


# ds10_vcf field order: per-sample state then per-frame control.
VCF_ALL = [0x24, 0x34, 0x38, 0x40, 0x50, 0x54, 0x5C, 0x60,
           0x04, 0x08, 0x0C, 0x20, 0x1C, 0x28, 0x3C, 0x44, 0x48, 0x4C, 0x58]


def capture_vcf_update(params, note, frames=6):
    """FILTER control state before/after the per-frame cutoff mapping.

    Preamble order is LFO, ENV, ..., FILTER, so the ENV/LFO values the filter
    block reads are the post-call ones. OSC+0x4C is the oscillator feedback and
    is written in the sample loop, so its pre-call value is what the preamble
    sees.
    """
    o = Oracle()
    fobj = o.u32(o.syn + FILTER_PTR)
    oobj = o.u32(o.syn + OSC_PTR)
    env = o.u32(o.syn + ENV_PTR)
    lfo = o.u32(o.syn + LFO_PTR)
    o.load_patch(params)

    def snap():
        fraw = bytes(o.uc.mem_read(fobj, 0x64))
        eraw = bytes(o.uc.mem_read(env, 0x14))
        lraw = bytes(o.uc.mem_read(lfo, 0x34))
        return ([_s32(struct.unpack_from("<I", fraw, off)[0]) for off in VCF_ALL],
                [_s32(struct.unpack_from("<I", eraw, 8 + 4 * i)[0]) for i in range(3)],
                [_s32(struct.unpack_from("<I", lraw, 4 * i)[0]) for i in range(5)],
                _s32(o.u32(oobj + 0x4C)))

    o.note_on(note)
    o.call(RENDER, o.syn)
    cases = []
    for _ in range(frames):
        before = snap()
        o.call(RENDER, o.syn)
        after = snap()
        cases.append(((before[0], after[1], after[2], before[3]), after[0]))
    return cases


def cmd_vcfupd_vectors(path="tests/vectors/vcfupd.bin"):
    _n, init = factory_patch(0)
    cases = []
    for cutoff in (0, 24, 48, 72, 96, 127):
        for peak in (0, 60, 100, 127):
            p = list(init)
            p[11], p[12] = cutoff, peak
            cases += capture_vcf_update(p, 60)
    for ftype in (0, 1, 2):
        for egint in (-63, 0, 63):
            p = list(init)
            p[13], p[14] = ftype, egint
            cases += capture_vcf_update(p, 60)
    for src in range(7):
        for depth in (-63, 0, 63):
            p = list(init)
            p[29], p[15] = src, depth
            cases += capture_vcf_update(p, 60)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        cases += capture_vcf_update(p, 60)

    b = open("out/arm9.bin", "rb").read()
    base = 0x02004000
    s16 = lambda a, n: [struct.unpack_from("<h", b, a - base + 2 * i)[0]
                        for i in range(n)]
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - base + 4 * i)[0])
                         for i in range(n)]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VCU1" + struct.pack("<I", len(cases)))
        f.write(struct.pack("<128h", *s16(0x020803C4, 128)))
        f.write(struct.pack("<128h", *s16(0x020804C4, 128)))
        f.write(struct.pack("<152i", *u32t(0x020807C4, 152)))
        for (fv, envv, lfov, prev2), want in cases:
            f.write(struct.pack(f"<{len(VCF_ALL)}i", *fv))
            f.write(struct.pack("<3i", *envv))
            f.write(struct.pack("<5i", *lfov))
            f.write(struct.pack("<i", prev2))
            f.write(struct.pack(f"<{len(VCF_ALL)}i", *want))
    print(f"{path}: {len(cases)} frame transitions")


# --- sequencer capture ------------------------------------------------------
SEQ_ALLOC = 0x0202927C     # the sequencer is plain alloc + ctor, no factory
SEQ_CTOR = 0x0206D79C
SEQ_CONFIG = 0x0206D830    # (seq, nsynths, ndrums)
SEQ_SET_SWING = 0x0206DB14 # (seq, swing_percent)
SEQ_TICK = 0x0206E5AC
DIVIDE = 0x0200C158        # (a, b) -> a/b in r0
SEQ_SIZE = 0x214

# ds10_seq compare order.
SEQ_FIELDS = [(0x0C, 1), (0x0D, 1), (0x10, 1), (0x14, 4), (0x18, 4), (0x1C, 4),
              (0x20, 4), (0x24, 1), (0x28, 4), (0x2C, 1), (0x2D, 1),
              (0x2E, 1), (0x30, 1), (0x31, 1), (0x32, 1), (0x33, 1),
              (0x40, 4), (0x48, 4), (0x54, 4)]


def _new_seq(o, nsyn=2, ndrum=4):
    seq = o.call(SEQ_ALLOC, SEQ_SIZE)
    o.call(SEQ_CTOR, seq)
    o.call(SEQ_CONFIG, seq, nsyn, ndrum)
    return seq


def capture_seq_maps():
    """Tempo and swing mappings, straight off the original's own routines."""
    o = Oracle()
    seq = _new_seq(o)
    tempo = []
    for t in range(1, 301):
        v = o.call(DIVIDE, DS10_SEQ_TEMPO_NUM, t)
        tempo.append((t, _s32(v)))
    swing = []
    for w in range(0, 101):
        o.call(SEQ_SET_SWING, seq, w)
        raw = bytes(o.uc.mem_read(seq + 0x30, 2))
        swing.append((w, raw[0], raw[1]))
    return tempo, swing


def capture_seq_ticks(tempo, swing, ticks=400):
    """Run the real tick with no pattern loaded, which exercises the transport,
    the accumulator, step advance and bar counting in isolation."""
    o = Oracle()
    seq = _new_seq(o)
    o.call(SEQ_SET_SWING, seq, swing)
    period = _s32(o.call(DIVIDE, DS10_SEQ_TEMPO_NUM, tempo))
    o.uc.mem_write(seq + 0x10, bytes([tempo & 0xFF]))
    o.w32(seq + 0x14, period)
    o.w32(seq + 0x40, 0)              # no pattern
    o.uc.mem_write(seq + 0x0C, bytes([1, 1]))   # playing, starting

    def snap():
        raw = bytes(o.uc.mem_read(seq, SEQ_SIZE))
        return [_rdf(raw, off, sz) for off, sz in SEQ_FIELDS]

    steps = []
    for _ in range(ticks):
        before = snap()
        o.call(SEQ_TICK, seq)
        steps.append((before, snap()))
    return steps


DS10_SEQ_TEMPO_NUM = 60000000
SEQ_AUTO = 0x0206E1EC         # automation lanes
BX_LR = 0x02072A44            # an existing `bx lr`, used to stub the knob call


def capture_seq_auto():
    """Drive the real automation-lane processor and read back the lane result.

    The processor ends by calling the machine's knob handler through vtable slot
    +0x18; pointing that at an existing `bx lr` makes the call harmless, after
    which the computed value is readable from the lane's own bytes.
    """
    o = Oracle()
    seq = _new_seq(o)
    vt = o.call(NEW_THUNK, 0x20)
    o.w32(vt + 0x18, BX_LR)
    mach = o.call(NEW_THUNK, 0x10)
    o.w32(mach, vt)
    arr = o.call(NEW_THUNK, 8)
    o.w32(arr, mach)
    o.w32(arr + 4, mach)
    router = o.call(NEW_THUNK, 0x20)
    o.w32(router, arr)
    o.w32(seq + 0x34, router)
    pat = o.call(NEW_THUNK, 0x2A4)
    defaults = o.call(NEW_THUNK, 0x80)
    o.w32(pat + 0x14, defaults)
    o.w32(pat + 0x18, defaults)
    o.w32(seq + 0x40, pat)
    o.w32(seq + 0x4C, 1)

    cases = []
    for raw in (-1, -64, -8, 0, 1, 7, 31, 63, 100, 127):
        for offset in (-64, -1, 0, 1, 63):
            for scale in (0, 1, 3, 64, 127, 255):
                step = 3
                o.uc.mem_write(seq + 0x2D, bytes([step]))
                o.uc.mem_write(seq + 0x33, bytes([0]))
                trk = seq + 0x64
                o.uc.mem_write(trk + 0x20, bytes([1, 0, 0]))
                for lane in range(2):
                    lb = trk + 0x10 + lane * 8
                    o.uc.mem_write(lb, bytes([5 + lane, 0,
                                              offset & 0xFF, scale]))
                    o.w32(lb + 4, 0)
                a = pat + 0 * 0x68 + step * 2
                o.uc.mem_write(a + 0x64, bytes([raw & 0xFF, raw & 0xFF]))
                o.call(SEQ_AUTO, seq)
                out = []
                for lane in range(2):
                    lb = trk + 0x10 + lane * 8
                    raw8 = bytes(o.uc.mem_read(lb, 8))
                    out.append(struct.unpack_from("<h", raw8, 4)[0])
                cases.append((raw, offset, scale, out[0], out[1]))
    return cases


SEQ_EMIT = 0x0206E8C8         # (seq) -> gate countdown + note event emission
ROUTER_ENABLE = 0x02160A90    # the router bails out if this word is zero
NMACH = 6


def capture_seq_emit():
    """Run the real emission through a router into fake machines.

    A "machine" only needs a queue pointer at +0x04 and a count byte at +0x08,
    which is the whole interface the router uses.
    """
    o = Oracle()
    seq = _new_seq(o)
    o.w32(ROUTER_ENABLE, 1)

    machines = [o.call(NEW_THUNK, 0x10) for _ in range(NMACH)]
    queues = [o.call(NEW_THUNK, 8 * 32) for _ in range(NMACH)]
    for m, q in zip(machines, queues):
        o.w32(m + 0x04, q)
        o.uc.mem_write(m + 0x08, b"\0")
    arr = o.call(NEW_THUNK, 4 * NMACH)
    for i, m in enumerate(machines):
        o.w32(arr + 4 * i, m)
    # Bus 1 (mixer) and bus 2 each get a machine too.
    bus = [o.call(NEW_THUNK, 0x10) for _ in range(2)]
    bq = [o.call(NEW_THUNK, 8 * 32) for _ in range(2)]
    for m, q in zip(bus, bq):
        o.w32(m + 0x04, q)
        o.uc.mem_write(m + 0x08, b"\0")
    router = o.call(NEW_THUNK, 0x20)
    o.w32(router + 0x00, arr)
    o.w32(router + 0x04, bus[0])
    o.w32(router + 0x08, bus[1])
    o.w32(router + 0x14, NMACH)
    o.w32(seq + 0x34, router)
    o.w32(seq + 0x54, NMACH)

    cases = []
    inits = [[0, 0, 0, 0, 0], [1, 1, 0, 8, 60], [1, 2, 0, 8, 48],
             [1, 3, 0, 8, 72], [1, 2, 3, 0xFF - 256, 36],
             [1, 1, 0, 6, 96], [1, 2, 5, 12, 24], [1, 0, 0, 4, 55]]
    for combo in range(len(inits)):
        # Give each track a different prior state so one call emits a mix.
        trk_state = [inits[(combo + i) % len(inits)] for i in range(NMACH)]
        for i, st in enumerate(trk_state):
            base = seq + 0x64 + i * 0x24
            o.uc.mem_write(base, bytes([st[0], st[1] & 0xFF, st[2] & 0xFF, 0,
                                        st[3] & 0xFF, st[4] & 0xFF, 0, 0]))
        for m in machines + bus:
            o.uc.mem_write(m + 0x08, b"\0")
        o.call(SEQ_EMIT, seq)
        posted = []
        for idx, (m, q) in enumerate(zip(machines + bus, queues + bq)):
            n = o.uc.mem_read(m + 0x08, 1)[0]
            for e in range(n):
                raw = bytes(o.uc.mem_read(q + 8 * e, 8))
                posted.append((idx, raw[0], raw[1],
                               struct.unpack_from("<H", raw, 4)[0]))
        after = []
        for i in range(NMACH):
            raw = bytes(o.uc.mem_read(seq + 0x64 + i * 0x24, 8))
            after.append([raw[j] - 256 if raw[j] > 127 else raw[j]
                          for j in TRK_FIELDS])
        cases.append((trk_state, posted, after))
    return cases


SEQ_STEP_TRACK = 0x0206E9D8   # (track, note, gate, subticks)
SEQ_GATE = 0x0206EABC         # (track) -> event code
TRK_FIELDS = [0, 1, 2, 4, 5]


def capture_seq_notes():
    """Sweep the note/gate logic and the per-sub-tick gate countdown.

    Both take just a track pointer, so this needs no router or machines.
    """
    o = Oracle()
    trk = o.call(NEW_THUNK, 0x24)
    cases = []

    def state():
        raw = bytes(o.uc.mem_read(trk, 8))
        return [raw[i] - 256 if raw[i] > 127 else raw[i] for i in TRK_FIELDS]

    # (note, gate) across the special values and a spread of ordinary ones,
    # from a variety of prior track states.
    inits = [[0, 0, 0, 0, 0], [1, 0, 0, 4, 60], [1, 0, 5, 4, 60],
             [1, 2, 3, 0xFF - 256, 60], [0, 1, 0, 6, 48],
             [1, 0, 0, 8, 72], [1, 3, 2, 12, 36]]
    gates = [0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 15, 0xFF]
    notes = [-1, 0, 36, 48, 60, 72, 96]
    subs = [0, 1, 6, 12, 13, 24]
    for init in inits:
        for note in notes:
            for gate in gates:
                for sub in subs:
                    o.uc.mem_write(trk, bytes([init[0], init[1] & 0xFF,
                                               init[2] & 0xFF, 0,
                                               init[3] & 0xFF,
                                               init[4] & 0xFF, 0, 0]))
                    before = state()
                    o.call(SEQ_STEP_TRACK, trk, note, gate, sub)
                    after_step = state()
                    r = o.call(SEQ_GATE, trk)
                    r = r - 256 if r > 127 else r
                    cases.append((before, note, gate, sub, after_step,
                                  state(), r))
    return cases


# --- song file capture ------------------------------------------------------
# The editor's song document is far too entangled with the GUI to construct, but
# the pattern accessors are leaf functions: they take the document pointer, do
# pure address arithmetic, and copy four signed bytes. So a bare region of
# memory with the current-song index zeroed is a good enough document to make
# them read a real song file, which pins the file layout against the original
# rather than against my reading of it.
SONG_GET_STEP = 0x02033F8C   # (doc, pattern, track, step, out4)
SONG_GET_AUTO = 0x020343B4   # (doc, pattern, track, step, out4)
SONG_STEP_COUNT = 0x02033FE4  # (doc) -> u32 at pattern[cur] + 0x00

SONG_DOC = 0x0E200000        # scratch document
SONG_DOC_SIZE = 0x00100000
SONG_CUR_SONG = 0x8CD9C      # doc + this: index into the per-song array
# Everything else hangs off B = doc + 0x20 + song*0x43040 + 0x40000: the live
# song the editor is showing. Its pattern array starts at B + 0xB4, whereas in a
# .dat file the same array starts at payload + 0xEC - the file header carries
# 0x38 bytes the live model does not. So to make the accessors read a file, the
# payload goes 0x38 bytes below B.
SONG_B = 0x20 + 0x40000
SONG_CUR_PATTERN = SONG_B + 0x50
SONG_CUR_TRACK = SONG_B + 0x54
SONG_RECORD = SONG_B - 0x38
SONG_PATTERN_OFF = 0xEC      # pattern array, file-relative
SONG_PATTERN_SIZE = 676

SONG_PATTERNS = 16
SONG_TRACKS = 6
SONG_STEPS = 16
SONG_LANES = 2
SONG_OUT = 0x0E2F0000        # 4-byte landing pad for the accessors


def song_files():
    """Every song .dat in the extracted NitroFS, deduplicated by content."""
    out, seen = [], set()
    for sub in ("demo", "demoSP"):
        d = os.path.join("out", "nitrofs", "data", sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if "synth" in name or not name.endswith(".dat"):
                continue
            blob = open(os.path.join(d, name), "rb").read()
            # ds10synth*.dat are 63-byte patch files, not songs.
            if len(blob) < 0x1000 or blob in seen:
                continue
            seen.add(blob)
            out.append((f"{sub}/{name}", blob))
    return out


def capture_song(o, blob):
    """Read every step and automation record of `blob` with the game's code."""
    o.uc.mem_write(SONG_DOC, b"\0" * 0x400)
    o.w32(SONG_DOC + SONG_CUR_SONG, 0)
    o.uc.mem_write(SONG_DOC + SONG_RECORD, blob[16:])

    def rec(fn, pattern, track, step):
        o.uc.mem_write(SONG_OUT, b"\0" * 4)
        o.call(fn, SONG_DOC, pattern, track, step, SONG_OUT)
        return bytes(o.uc.mem_read(SONG_OUT, 4))

    steps, autos = [], []
    for p in range(SONG_PATTERNS):
        for t in range(SONG_TRACKS):
            for s in range(SONG_STEPS):
                steps.append(rec(SONG_GET_STEP, p, t, s))
        for t in range(SONG_LANES):
            for s in range(SONG_STEPS):
                autos.append(rec(SONG_GET_AUTO, p, t, s))
    # The step-count accessor reads the editor's pattern cursor rather than
    # taking a pattern argument, and that cursor overlaps the file's bar chain -
    # so poke it only after the records are captured.
    counts = []
    o.w32(SONG_DOC + SONG_CUR_TRACK, 0)
    for p in range(SONG_PATTERNS):
        o.w32(SONG_DOC + SONG_CUR_PATTERN, p)
        counts.append(o.call(SONG_STEP_COUNT, SONG_DOC))
    return steps, autos, counts


# --- song replay capture ----------------------------------------------------
# The whole tick, driven by a real pattern out of a real demo song. Everything
# the sequencer needs is built by hand: a router with six fake machines and two
# buses, a runtime pattern, and a song player whose handler is an existing
# `bx lr`. Automation is switched off per track (the enable byte at +0x20 is
# neither 1 nor 2) so the original skips it, since only the lane mapping is
# verified so far and not the modes around it.
CHAN_IDX_TABLE = 0x02160808   # two bytes per track: volume index, pan index
# Track-block bytes the replay compares. +0x0C and +0x0D are the live gate and
# note; the constructor does not leave them zero, and the play modes read them.
TRK_REPLAY_FIELDS = (0, 1, 2, 4, 5, 6, 7, 0x0C, 0x0D)
RT_PAT_SIZE = 0x50C           # the back-pointer at +0x508 is the last field


def _rt_pattern(o, song_pat, ntracks, steps):
    """Build a runtime pattern from a parsed .dat pattern."""
    pat = o.call(NEW_THUNK, RT_PAT_SIZE)
    o.uc.mem_write(pat, b"\0" * RT_PAT_SIZE)
    o.uc.mem_write(pat + 6, bytes([steps]))
    for t in range(ntracks):
        for st in range(16):
            src = 0x42 + t * 64 + st * 4
            o.uc.mem_write(pat + 0x24 + t * 0x68 + st * 4, song_pat[src:src + 4])
    return pat


def capture_replay(blob, pattern_idx, ticks, tempo, swing, modes, chan, idx,
                   live=None):
    o = Oracle()
    seq = _new_seq(o, 2, 4)
    o.w32(ROUTER_ENABLE, 1)

    # Machines need a queue for the router and a vtable for anything that calls
    # through them; two buses stand in for the mixer and the UI bus.
    vt = o.call(NEW_THUNK, 0x20)
    o.w32(vt + 0x18, BX_LR)
    machines, queues = [], []
    for _ in range(NMACH + 2):
        m = o.call(NEW_THUNK, 0x10)
        q = o.call(NEW_THUNK, 8 * 64)
        o.w32(m + 0x00, vt)
        o.w32(m + 0x04, q)
        o.uc.mem_write(m + 0x08, b"\0")
        machines.append(m)
        queues.append(q)
    arr = o.call(NEW_THUNK, 4 * NMACH)
    for i in range(NMACH):
        o.w32(arr + 4 * i, machines[i])
    router = o.call(NEW_THUNK, 0x20)
    o.w32(router + 0x00, arr)
    o.w32(router + 0x04, machines[NMACH])
    o.w32(router + 0x08, machines[NMACH + 1])
    o.w32(router + 0x14, NMACH)

    # Song player: message word at +4, answer at +8, handler at vtable slot 1.
    pvt = o.call(NEW_THUNK, 0x10)
    o.w32(pvt + 0x04, BX_LR)
    player = o.call(NEW_THUNK, 0x10)
    o.w32(player + 0x00, pvt)

    payload = blob[16:]
    off = 0xEC + pattern_idx * 676
    song_pat = payload[off:off + 676]
    steps = struct.unpack_from("<I", song_pat, 0)[0]
    pat = _rt_pattern(o, song_pat, NMACH, steps)

    chan_arr = o.call(NEW_THUNK, 0x40)
    o.uc.mem_write(chan_arr, bytes(v & 0xFF for v in chan))
    o.uc.mem_write(CHAN_IDX_TABLE, bytes(idx))

    o.w32(seq + 0x34, router)
    o.w32(seq + 0x3C, player)
    o.w32(seq + 0x54, NMACH)
    o.w32(seq + 0x4C, 2)
    o.w32(seq + 0x50, 4)
    o.w32(seq + 0x60, chan_arr)
    o.call(SEQ_SET_SWING, seq, swing)
    period = _s32(o.call(DIVIDE, DS10_SEQ_TEMPO_NUM, tempo))
    o.uc.mem_write(seq + 0x10, bytes([tempo & 0xFF]))
    o.w32(seq + 0x14, period)
    o.w32(seq + 0x40, pat)
    o.w32(pat + 0x508, seq)
    o.uc.mem_write(seq + 0x2C, bytes([steps]))
    for t in range(NMACH):
        trk = seq + 0x64 + t * 0x24
        o.uc.mem_write(trk + 0x20, bytes([0, 0, modes[t]]))
        if live is not None:
            o.uc.mem_write(trk + 0x0C, bytes([live[t][0] & 0xFF,
                                              live[t][1] & 0xFF]))
    o.uc.mem_write(seq + 0x0C, bytes([1, 1]))

    def snap():
        raw = bytes(o.uc.mem_read(seq, SEQ_SIZE))
        return [_rdf(raw, o_, sz) for o_, sz in SEQ_FIELDS]

    def trkstate():
        out = []
        for t in range(NMACH):
            raw = bytes(o.uc.mem_read(seq + 0x64 + t * 0x24, 0x24))
            out.append([raw[j] for j in TRK_REPLAY_FIELDS])
        return out

    initial = (snap(), trkstate())
    runs = []
    for _ in range(ticks):
        for m in machines:
            o.uc.mem_write(m + 0x08, b"\0")
        o.call(SEQ_TICK, seq)
        posted = []
        for i, (m, q) in enumerate(zip(machines, queues)):
            n = o.uc.mem_read(m + 0x08, 1)[0]
            for e in range(n):
                raw = bytes(o.uc.mem_read(q + 8 * e, 8))
                posted.append((i, raw[0], raw[1],
                               struct.unpack_from("<H", raw, 4)[0]))
        runs.append((snap(), trkstate(), posted, steps))
        # Stand in for the song player: whenever the sequencer drops the pattern
        # at the end of a bar, put it straight back. The chain walk is not
        # decompiled, so both sides do the same thing here and neither is being
        # checked against the original. Snapshot first, so what is compared is
        # the tick's own output.
        if o.u32(seq + 0x40) == 0:
            o.w32(seq + 0x40, pat)
            o.uc.mem_write(seq + 0x2C, bytes([steps]))
    return initial, runs, steps


def cmd_replay_vectors(path="tests/vectors/replay.bin"):
    files = song_files()
    if not files:
        print("no song files - run `make extract` first")
        return
    # One case per demo song that has any notes, plus mode variations on one of
    # them so the mute, live and record paths get exercised too.
    cases = []
    for name, blob in files:
        payload = blob[16:]
        tempo = struct.unpack_from("<I", payload, 4)[0]
        swing = struct.unpack_from("<H", payload, 0x0A)[0]
        chain = struct.unpack_from("<100H", payload, 0x24)
        if chain[0] == 0xFFFF:
            continue
        for pidx in (chain[0], chain[1] if chain[1] != 0xFFFF else chain[0]):
            cases.append((name, blob, pidx, tempo, swing, [0] * 6,
                          [0x40] * 16, [0, 1] * 8))
    base = list(cases[0]) + [None]
    cases = [list(c) + [None] for c in cases]
    for modes in ([1] * 6, [0, 1, 0, 1, 0, 1], [2] * 6, [3] * 6,
                  [0, 1, 2, 3, 4, 5]):
        cases.append(base[:5] + [modes] + base[6:])
    for chan in ([0] * 16, [0x7F] * 16, [-64] * 16):
        cases.append(base[:6] + [chan] + base[7:])
    # Live input matters to the unless-live and record modes: mode 2 must skip
    # the pattern while a note is held, mode 3 must write the held note back.
    live = [(0x0C, 48 + 3 * t) for t in range(6)]
    for modes in ([2] * 6, [3] * 6, [0, 2, 3, 2, 3, 0]):
        cases.append(base[:5] + [modes] + base[6:8] + [live])
    cases.append(base[:5] + [[2] * 6] + base[6:8] + [[(0x0C, -1)] * 6])

    TICKS = 700
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RPL1")
        f.write(struct.pack("<3I", len(cases), TICKS, NMACH))
        nev = 0
        for name, blob, pidx, tempo, swing, modes, chan, idx, live in cases:
            initial, runs, steps = capture_replay(blob, pidx, TICKS, tempo,
                                                 swing, modes, chan, idx, live)
            f.write(struct.pack("<I", len(name)))
            f.write(name.encode())
            f.write(struct.pack("<6I", pidx, tempo, swing, steps, 2, 4))
            f.write(bytes(modes))
            f.write(struct.pack("<16b", *[c if c >= -128 else -128
                                          for c in chan]))
            f.write(bytes(idx))
            pat = blob[16 + 0xEC + pidx * 676:16 + 0xEC + (pidx + 1) * 676]
            f.write(pat)
            f.write(struct.pack(f"<{len(SEQ_FIELDS)}i", *initial[0]))
            for t in initial[1]:
                f.write(bytes(t))
            for state, trks, posted, _ in runs:
                f.write(struct.pack(f"<{len(SEQ_FIELDS)}i", *state))
                for t in trks:
                    f.write(bytes(t))
                f.write(struct.pack("<I", len(posted)))
                for d, op, ch, pl in posted:
                    f.write(struct.pack("<BBBxH", d, op, ch, pl))
                nev += len(posted)
    print(f"{path}: {len(cases)} cases x {TICKS} ticks, {nev} events")


def cmd_song_vectors(path="tests/vectors/song.bin"):
    files = song_files()
    if not files:
        print("no song files - run `make extract` first")
        return
    o = Oracle()
    o.uc.mem_map(SONG_DOC, SONG_DOC_SIZE)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"SNG1")
        f.write(struct.pack("<4I", len(files), SONG_PATTERNS, SONG_TRACKS,
                            SONG_STEPS))
        for name, blob in files:
            steps, autos, counts = capture_song(o, blob)
            f.write(struct.pack("<I", len(name)))
            f.write(name.encode())
            f.write(struct.pack("<I", len(blob)))
            f.write(struct.pack(f"<{SONG_PATTERNS}I", *counts))
            f.write(blob)
            f.write(b"".join(steps))
            f.write(b"".join(autos))
    print(f"{path}: {len(files)} songs, "
          f"{len(files) * SONG_PATTERNS * (SONG_TRACKS + SONG_LANES) * SONG_STEPS}"
          " records")



def cmd_seq_vectors(path="tests/vectors/seq.bin"):
    tempo, swing = capture_seq_maps()
    runs = []
    for t in (60, 120, 180, 240):
        for w in (0, 25, 50, 75, 100):
            runs.append((t, w, capture_seq_ticks(t, w)))
    notes = capture_seq_notes()
    emits = capture_seq_emit()
    autos = capture_seq_auto()
    nf = len(SEQ_FIELDS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"SEQ4" + struct.pack("<IIIIII", len(tempo), len(swing), nf,
                                       len(notes), len(emits), len(autos)))
        for r_, of, sc, l0, l1 in autos:
            f.write(struct.pack("<5i", r_, of, sc, l0, l1))
        for trk_state, posted, after in emits:
            for st in trk_state:
                f.write(struct.pack("<5i", *st))
            f.write(struct.pack("<i", len(posted)))
            for idx, op, ch, pay in posted:
                f.write(struct.pack("<4i", idx, op, ch, pay))
            for a in after:
                f.write(struct.pack("<5i", *a))
        for before, note, gate, sub, after_step, after_gate, r in notes:
            f.write(struct.pack("<5i", *before))
            f.write(struct.pack("<3i", note, gate, sub))
            f.write(struct.pack("<5i", *after_step))
            f.write(struct.pack("<5i", *after_gate))
            f.write(struct.pack("<i", r))
        for t, v in tempo:
            f.write(struct.pack("<ii", t, v))
        for w, a, b in swing:
            f.write(struct.pack("<iii", w, a, b))
        f.write(struct.pack("<i", len(runs)))
        for t, w, steps in runs:
            f.write(struct.pack("<iii", t, w, len(steps)))
            for before, after in steps:
                f.write(struct.pack(f"<{nf}i", *before))
                f.write(struct.pack(f"<{nf}i", *after))
    print(f"{path}: {len(tempo)} tempos, {len(swing)} swings, "
          f"{len(runs)} runs x {len(runs[0][2])} ticks, {len(notes)} note cases, "
          f"{len(emits)} emit cases, {len(autos)} lane cases")


# --- synth event handling capture -------------------------------------------
# (object, offset, size) for everything the event dispatcher can touch.
EV_FIELDS = [("env", 0x04, 1), ("env", 0x05, 1), ("env", 0x08, 4),
             ("env", 0x0C, 4),
             ("nc", 0x00, 4), ("nc", 0x04, 4), ("nc", 0x0C, 4),
             ("nc", 0x10, 1),
             ("lfo", 0x14, 4), ("lfo", 0x18, 4), ("lfo", 0x1C, 4),
             ("lfo", 0x28, 1), ("lfo", 0x2C, 4), ("lfo", 0x30, 2),
             ("syn", 0x2C, 4), ("syn", 0x50, 2), ("syn", 0x52, 2),
             ("vco", 0x14, 4), ("vco", 0x18, 4)]


def capture_events(params, seqs, bpm_sync=0):
    """Post event batches into the real queue and call the real dispatcher.

    Calling 0x0207546C directly isolates event handling from the DSP.
    """
    cases = []
    for seq in seqs:
        o = Oracle()
        base = {"syn": o.syn, "env": o.u32(o.syn + 0x30),
                "lfo": o.u32(o.syn + 0x34), "nc": o.u32(o.syn + 0x38),
                "vco": o.u32(o.syn + 0x40)}
        p = list(params)
        p[25] = bpm_sync
        o.load_patch(p)

        def snap():
            out = []
            for key, off, sz in EV_FIELDS:
                raw = bytes(o.uc.mem_read(base[key] + off, 4))
                out.append(_rdf(raw, 0, sz))
            return out

        steps = []
        for batch in seq:
            before = snap()
            for i, (op, note, arg) in enumerate(batch):
                o.uc.mem_write(o.evq + 8 * i,
                               struct.pack("<Bxxxi", op, arg if op == 4 else note))
            o.uc.mem_write(o.syn + 0x08, bytes([len(batch)]))
            o.call(EVENT_DISPATCH, o.syn)
            steps.append((batch, before, snap()))
        cases.append(steps)
    return cases


def cmd_event_vectors(path="tests/vectors/event.bin"):
    _n, init = factory_patch(0)
    seqs = []
    # Every opcode on its own.
    for op in (1, 2, 4, 5, 0x18, 0x19, 0x1C, 0x1D, 0x1E):
        seqs.append([[(op, 60, 120)]])
    # Note-on/off pairs and legato (note-on with no note-off between).
    seqs.append([[(2, 48, 0)], [(1, 0, 0)], [(2, 60, 0)], [(1, 0, 0)]])
    seqs.append([[(2, 48, 0)], [(2, 55, 0)], [(2, 60, 0)]])
    # Pattern change then bar start then note-on, which is the path where the
    # bar flags actually matter.
    seqs.append([[(0x1D, 0, 0)], [(0x1C, 0, 0)], [(2, 60, 0)]])
    seqs.append([[(0x1D, 0, 0)], [(2, 60, 0)], [(0x1C, 0, 0)]])
    # Tempo changes, then a bar start - the BPM-synced LFO re-phase path.
    for bpm in (60, 120, 240):
        seqs.append([[(4, 0, bpm)], [(0x1D, 0, 0)], [(0x1C, 0, 0)],
                     [(2, 60, 0)]])
    # Multiple events in one batch, which the dispatcher loops over.
    seqs.append([[(0x1D, 0, 0), (2, 60, 0)], [(1, 0, 0), (2, 64, 0)]])
    # Descending notes, to exercise the portamento direction flag both ways.
    seqs.append([[(2, 96, 0)], [(2, 24, 0)], [(2, 96, 0)]])

    cases = []
    for sync in (0, 1):
        cases += capture_events(init, seqs, bpm_sync=sync)

    b = open("out/arm9.bin", "rb").read()
    bb = 0x02004000
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - bb + 4 * i)[0])
                         for i in range(n)]
    nf = len(EV_FIELDS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"EVT1" + struct.pack("<II", len(cases), nf))
        f.write(struct.pack("<16i", *u32t(0x0207FF80, 16)))
        f.write(struct.pack("<128i", *u32t(0x0207FFC4, 128)))
        for steps in cases:
            f.write(struct.pack("<i", len(steps)))
            for batch, before, after in steps:
                f.write(struct.pack("<i", len(batch)))
                for op, note, arg in batch:
                    f.write(struct.pack("<iii", op, note, arg))
                f.write(struct.pack(f"<{nf}i", *before))
                f.write(struct.pack(f"<{nf}i", *after))
    nsteps = sum(len(c) for c in cases)
    print(f"{path}: {len(cases)} sequences, {nsteps} dispatch calls")


# --- drum voice capture -----------------------------------------------------
DRUM_NEW = 0x02076014
DRUM_RENDER = 0x020722B8
SAMPLE_LEN = 0x8000        # what the engine pre-renders a drum patch into


def capture_drum(speed, stage, vol, length, frames=6, seed=7):
    """Drive the real drum voice over a known sample buffer."""
    o = Oracle()
    dr = o.call(DRUM_NEW)
    vb = o.u32(dr + 0x30)
    samp = o.call(NEW_THUNK, length * 2 + 4)
    outb = o.call(NEW_THUNK, FRAME * 2)
    slot = o.call(NEW_THUNK, 4)
    o.w32(slot, outb)

    rng = _lcg(seed)
    wave = [next(rng) >> 1 for _ in range(length + 1)]
    o.uc.mem_write(samp, struct.pack(f"<{length + 1}h", *wave))

    o.uc.mem_write(dr + 0x08, b"\0")
    o.w32(dr + 0x10, FRAME)
    o.w32(dr + 0x1C, slot)
    o.w32(dr + 0x34, speed)
    o.w32(dr + 0x38, 0)
    o.w32(dr + 0x40, length)
    o.w32(dr + 0x44, samp)
    o.uc.mem_write(vb + 0x10, bytes([stage]))
    o.w32(vb + 0x14, vol)

    state = [_s32(o.u32(vb + 0x14)), o.uc.mem_read(vb + 0x10, 1)[0],
             _s32(o.u32(dr + 0x34)), _s32(o.u32(dr + 0x38)),
             _s32(o.u32(dr + 0x40))]
    outs = []
    for _ in range(frames):
        o.call(DRUM_RENDER, dr)
        outs.append(list(struct.unpack(f"<{FRAME}h",
                                       o.uc.mem_read(outb, FRAME * 2))))
    return state, wave, outs


def cmd_drum_vectors(path="tests/vectors/drum.bin"):
    """Sweep playback rate, envelope stage and starting level."""
    cases = []
    # 0x8000 is unity rate; the trigger formula is 0x8000 * 2^((note-60)/12).
    for speed in (0x800, 0x4000, 0x8000, 0xC000, 0x10000, 0x40000):
        for stage in (0, 1, 2):
            cases.append(capture_drum(speed, stage, 0x400000, 4096,
                                      seed=speed + stage))
    # Run past the end of the buffer so the one-shot clamp engages.
    cases.append(capture_drum(0x80000, 0, 0x800000, 512, frames=12, seed=99))
    # Envelope from zero, and from the ceiling.
    for vol in (0, 0x00B33333):
        cases.append(capture_drum(0x8000, 0, vol, 2048, frames=10,
                                  seed=vol + 1))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"DRM1" + struct.pack("<I", len(cases)))
        for state, wave, outs in cases:
            f.write(struct.pack("<5i", *state))
            f.write(struct.pack("<II", len(wave), len(outs)))
            f.write(struct.pack(f"<{len(wave)}h", *wave))
            for o_ in outs:
                f.write(struct.pack(f"<{FRAME}h", *o_))
    tot = sum(len(c[2]) for c in cases) * FRAME
    print(f"{path}: {len(cases)} cases, {tot} samples")


# --- mixer + effect DSP capture ---------------------------------------------
MIXER_RENDER = 0x02072E8C

# ds10_mixer compare order.
MX_FIELDS = ([(0x30, 4)] + [(0x38 + 4 * c, 4) for c in range(6)] +
             [(0x50 + 4 * c, 4) for c in range(6)] +
             [(0x68 + 4 * c, 4) for c in range(6)] +
             [(0x84, 4), (0x88, 2)])
# ds10_fx compare order (scalars only; the two ring buffers start zeroed on
# both sides and stay in lockstep).
FXS_FIELDS = ([(o, 2) for o in (0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18,
                                0x1A, 0x1C)] +
              [(o, 4) for o in (0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40,
                                0x44, 0x48, 0x4C, 0x50, 0x54, 0x58,
                                0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70,
                                0x74, 0x78, 0x7C, 0x80, 0x84, 0x88,
                                0x8C, 0x90, 0x94, 0x98, 0x9C, 0xA0)])


def _lcg(seed):
    x = seed & 0xFFFFFFFF
    while True:
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        yield ((x >> 16) & 0xFFFF) - 0x8000


def capture_mixer(setup, frames=8, seed=1):
    """Drive the real mixer with known voice buffers and capture stereo out.

    `setup` is a list of (kind, a, b): ("fx", param, value),
    ("pan", ch, v), ("vol", ch, v), ("route", v, _), ("mute", v, _),
    ("tempo", v, _).
    """
    o = Oracle()
    mx = o.call(MIXER_NEW)
    fx = o.u32(mx + FX_PTR)

    # Six voice buffers, an array of pointers at 8-byte stride, and the output
    # pointer pair the mixer writes through.
    bufs = [o.call(NEW_THUNK, FRAME * 2) for _ in range(6)]
    arr = o.call(NEW_THUNK, 6 * 8)
    for i, b in enumerate(bufs):
        o.w32(arr + 8 * i, b)
    outl = o.call(NEW_THUNK, FRAME * 2)
    outr = o.call(NEW_THUNK, FRAME * 2)
    pair = o.call(NEW_THUNK, 8)
    o.w32(pair, outl)
    o.w32(pair + 4, outr)
    o.w32(mx + 0x10, FRAME)
    o.w32(mx + 0x18, arr)
    o.w32(mx + 0x1C, pair)
    o.uc.mem_write(mx + 0x08, b"\0")

    for kind, a, b in setup:
        if kind == "fx":
            o.call(FX_SET, fx, a, b)
        elif kind == "pan":
            o.w32(mx + 0x38 + 4 * a, b)
        elif kind == "vol":
            o.w32(mx + 0x50 + 4 * a, b)
        elif kind == "route":
            o.w32(mx + 0x84, a)
        elif kind == "mute":
            o.w32(mx + 0x88, a)
        elif kind == "tempo":
            o.w32(fx + 0xA0, a)

    def snap(base, fields, size):
        raw = bytes(o.uc.mem_read(base, size))
        return [_rdf(raw, off, sz) for off, sz in fields]

    mstate = snap(mx, MX_FIELDS, 0x8C)
    fstate = snap(fx, FXS_FIELDS, 0xA8)

    rng = _lcg(seed)
    steps = []
    for _ in range(frames):
        ins = []
        for bi in bufs:
            vals = [next(rng) >> 2 for _ in range(FRAME)]
            o.uc.mem_write(bi, struct.pack(f"<{FRAME}h", *vals))
            ins.append(vals)
        o.call(MIXER_RENDER, mx)
        l = list(struct.unpack(f"<{FRAME}h", o.uc.mem_read(outl, FRAME * 2)))
        r = list(struct.unpack(f"<{FRAME}h", o.uc.mem_read(outr, FRAME * 2)))
        steps.append((ins, l, r))
    return mstate, fstate, steps


def cmd_mixer_vectors(path="tests/vectors/mixer.bin"):
    """Sweep effect type, routing, pan/vol, mute and tempo sync."""
    cases = []
    base = [("vol", c, 0x6000) for c in range(6)] + \
           [("pan", c, 0x4000) for c in range(6)]
    for ftype in (0, 1, 2):
        for route in range(6):
            setup = base + [("fx", 1, ftype), ("fx", 2, 60), ("fx", 5, 90),
                            ("fx", 7, 40), ("fx", 8, 70), ("route", route, 0)]
            cases.append(capture_mixer(setup, seed=1 + ftype * 8 + route))
    # Pan and volume extremes.
    for pan in (0, 0x2000, 0x7FFF):
        for vol in (0, 0x3000, 0x7FFF):
            setup = [("vol", c, vol) for c in range(6)] + \
                    [("pan", c, pan) for c in range(6)] + \
                    [("fx", 1, 0), ("fx", 8, 40), ("route", 0, 0)]
            cases.append(capture_mixer(setup, seed=100 + pan + vol))
    # Mute ramp, both directions.
    for mute in (0, 1):
        cases.append(capture_mixer(base + [("mute", mute, 0)], frames=12,
                                   seed=200 + mute))
    # Tempo-synced delay - the divider path, end to end through the DSP.
    for tempo in (60, 120, 400):
        setup = base + [("tempo", tempo, 0), ("fx", 1, 0), ("fx", 6, 1),
                        ("fx", 2, 40), ("fx", 7, 50), ("fx", 8, 90),
                        ("route", 4, 0)]
        cases.append(capture_mixer(setup, frames=12, seed=300 + tempo))
    # Heavy feedback, to make the ring buffer and its ramp matter.
    for fb in (0, 30, 63):
        setup = base + [("fx", 1, 0), ("fx", 2, 12), ("fx", 7, fb),
                        ("fx", 8, 127), ("route", 4, 0)]
        cases.append(capture_mixer(setup, frames=16, seed=400 + fb))

    b = open("out/arm9.bin", "rb").read()
    bb = 0x02004000
    s16 = lambda a, n: [struct.unpack_from("<h", b, a - bb + 2 * i)[0]
                        for i in range(n)]
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - bb + 4 * i)[0])
                         for i in range(n)]
    nm, nf = len(MX_FIELDS), len(FXS_FIELDS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"MIX1" + struct.pack("<III", len(cases), nm, nf))
        f.write(struct.pack("<3i", *u32t(0x0207EF38, 3)))
        f.write(struct.pack("<128h", *s16(0x0207EF44, 128)))
        f.write(struct.pack("<128h", *s16(0x0207F044, 128)))
        f.write(struct.pack("<12i", *u32t(0x0207F144, 12)))
        f.write(struct.pack("<128i", *u32t(0x0207FFC4, 128)))
        for mstate, fstate, steps in cases:
            f.write(struct.pack(f"<{nm}i", *mstate))
            f.write(struct.pack(f"<{nf}i", *fstate))
            f.write(struct.pack("<i", len(steps)))
            for ins, l, r in steps:
                for ch in ins:
                    f.write(struct.pack(f"<{FRAME}h", *ch))
                f.write(struct.pack(f"<{FRAME}h", *l))
                f.write(struct.pack(f"<{FRAME}h", *r))
    tot = sum(len(c[2]) for c in cases) * FRAME
    print(f"{path}: {len(cases)} cases, {tot} stereo samples")


# --- effect parameter mapping capture --------------------------------------
MIXER_NEW = 0x02076030     # alloc(0x8C) + ctor; the fx processor is at +0x80
FX_SET = 0x020726D4        # (fx, param, value)
FX_PTR = 0x80

# Fields the parameter mappings write, in ds10_fx compare order.
FX_FIELDS = [(0x0C, 2), (0x0E, 2), (0x10, 2), (0x12, 2), (0x14, 2),
             (0x16, 2), (0x18, 2), (0x1A, 2), (0x1C, 2),
             (0x44, 4), (0x48, 4), (0x4C, 4), (0x50, 4),
             (0x8C, 4), (0x90, 4), (0x98, 4), (0x9C, 4)]


def capture_fx_params():
    """Set each effect parameter through the original and read the state back.

    A type change reapplies all six other parameters through the new mapping,
    so the sequence matters and each type gets its own sweep.
    """
    cases = []
    seqs = []
    for ftype in (0, 1, 2):
        # (param, value) ranges per the knob ranges in recon.md
        seq = [(1, ftype)]
        for v in range(0, 128, 7):
            seq.append((2, v))
        for v in range(-63, 64, 9):
            seq.append((3, v))
        for v in range(0, 128, 11):
            seq.append((4, v))
        for v in range(0, 128, 9):
            seq.append((5, v))
        for v in range(-63, 64, 7):
            seq.append((7, v))
        for v in range(0, 128, 5):
            seq.append((8, v))
        seqs.append(seq)
    # Delay with tempo sync. The divisor is the tempo at +0xA0, which a fresh
    # mixer leaves at zero, so the tempo has to be poked in or the synced path
    # never runs and the hardware-divider code goes untested.
    tempos = [0] * len(seqs)
    for tempo in (1, 60, 120, 200, 1000, 70000):
        seqs.append([(1, 0), (6, 1)] + [(2, v) for v in range(0, 128, 5)])
        tempos.append(tempo)

    for seq, tempo0 in zip(seqs, tempos):
        o = Oracle()
        mx = o.call(MIXER_NEW)
        fx = o.u32(mx + FX_PTR)
        if tempo0:
            o.w32(fx + 0xA0, tempo0)

        def snap():
            raw = bytes(o.uc.mem_read(fx, 0xA8))
            return [_rdf(raw, off, sz) for off, sz in FX_FIELDS]

        init = snap()
        tempo = _s32(o.u32(fx + 0xA0))
        steps = []
        for param, value in seq:
            o.call(FX_SET, fx, param, value)
            steps.append((param, value, snap()))
        cases.append((init, tempo, steps))
    return cases


def cmd_fx_vectors(path="tests/vectors/fx.bin"):
    cases = capture_fx_params()
    b = open("out/arm9.bin", "rb").read()
    bb = 0x02004000
    s16 = lambda a, n: [struct.unpack_from("<h", b, a - bb + 2 * i)[0]
                        for i in range(n)]
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - bb + 4 * i)[0])
                         for i in range(n)]
    nf = len(FX_FIELDS)
    nsteps = sum(len(c[2]) for c in cases)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"FXP1" + struct.pack("<III", len(cases), nf, nsteps))
        f.write(struct.pack("<3i", *u32t(0x0207EF38, 3)))     # lfo phase off
        f.write(struct.pack("<128h", *s16(0x0207EF44, 128)))  # flanger fb
        f.write(struct.pack("<128h", *s16(0x0207F044, 128)))  # depth
        f.write(struct.pack("<12i", *u32t(0x0207F144, 12)))   # delay divisions
        f.write(struct.pack("<128i", *u32t(0x0207FFC4, 128))) # lfo rate
        for init, tempo, steps in cases:
            f.write(struct.pack(f"<{nf}i", *init))
            f.write(struct.pack("<ii", tempo, len(steps)))
            for param, value, st in steps:
                f.write(struct.pack("<ii", param, value))
                f.write(struct.pack(f"<{nf}i", *st))
    print(f"{path}: {len(cases)} sequences, {nsteps} parameter sets")


# --- patch loading capture --------------------------------------------------
# (object key, offset, size) for every field the 31 knobs write, in the order
# tests/test_patch.c reads them.
P_FIELDS = [
    ("pc", 0x30, 4), ("pc", 0x20, 4), ("pc", 0x24, 4), ("pc", 0x28, 4),
    ("pc", 0x2C, 4), ("pc", 0x34, 2),
    ("pc", 0x40, 2), ("pc", 0x44, 2), ("pc", 0x48, 2),
    ("pc", 0x4C, 4), ("pc", 0x50, 4), ("pc", 0x54, 4),
    ("nc", 0x0C, 4),
    ("vco", 0x54, 4), ("vco", 0x58, 4), ("vco", 0x5C, 4),
    ("vco", 0x2C, 4), ("vco", 0x30, 4), ("vco", 0x34, 4),
    ("vco", 0x38, 4), ("vco", 0x3C, 4), ("vco", 0x48, 4),
    ("vcf", 0x1C, 4), ("vcf", 0x20, 4), ("vcf", 0x24, 4), ("vcf", 0x3C, 4),
    ("vcf", 0x44, 4), ("vcf", 0x48, 4), ("vcf", 0x04, 4), ("vcf", 0x08, 4),
    ("amp", 0x24, 4), ("amp", 0x1C, 4), ("amp", 0x20, 4), ("amp", 0x38, 4),
    ("amp", 0x3C, 4), ("amp", 0x18, 4), ("amp", 0x04, 4), ("amp", 0x08, 4),
    ("envp", 0x00, 4), ("envp", 0x04, 4), ("envp", 0x08, 4), ("envp", 0x0C, 4),
    ("lfo", 0x18, 4), ("lfo", 0x30, 2), ("lfo", 0x28, 1),
]


def capture_patch(knob_sets, bpm=None):
    """For each knob vector: load it through the original and read back state.

    `bpm` overrides the LFO tempo at +0x2C. The constructor leaves it at 120, so
    the BPM-synced rate branch is exercised by default - but only at one tempo,
    hence the override.
    """
    cases = []
    for knobs in knob_sets:
        o = Oracle()
        base = {k: o.u32(o.syn + off) for k, off in
                (("lfo", 0x34), ("env", 0x30), ("nc", 0x38), ("pc", 0x3C),
                 ("vco", 0x40), ("vcf", 0x44), ("amp", 0x48))}
        base["envp"] = o.u32(base["env"])
        if bpm is not None:
            o.w32(base["lfo"] + 0x2C, bpm)
        cur_bpm = _s32(o.u32(base["lfo"] + 0x2C))
        o.load_patch(knobs)
        out = []
        for key, off, size in P_FIELDS:
            raw = bytes(o.uc.mem_read(base[key] + off, 4))
            out.append(_rdf(raw, 0, size))
        cases.append((list(knobs), cur_bpm, out))
    return cases


def cmd_patch_vectors(path="tests/vectors/patch.bin"):
    _n, init = factory_patch(0)
    sets = []
    for idx in range(24):
        _nm, p = factory_patch(idx)
        sets.append(p)
    # Sweep every knob across its full range with the rest at INIT, which
    # exercises far more of the space than the factory set does.
    RANGES = [(-2, 2), (-63, 63), (0, 127), (-63, 63), (-63, 63), (-63, 63),
              (0, 3), (0, 3), (-63, 63), (0, 127), (0, 1), (0, 127),
              (0, 127), (0, 2), (-63, 63), (-63, 63), (0, 1), (-63, 63),
              (0, 127), (0, 127), (0, 127), (0, 127), (0, 127), (0, 127),
              (0, 127), (0, 1), (0, 6), (0, 6), (0, 6), (0, 6), (0, 6)]
    for k, (lo, hi) in enumerate(RANGES):
        for val in range(lo, hi + 1):
            p = list(init)
            p[k] = val
            sets.append(p)
    cases = capture_patch(sets)

    # The BPM-synced LFO rate branch depends on the tempo, so sweep it too.
    # 3000 pushes the result past 2^31, which is a distinct arithmetic path.
    for bpm in (0, 1, 60, 240, 3000):
        tset = []
        for freq in range(0, 128, 3):
            p = list(init)
            p[24], p[25] = freq, 1
            tset.append(p)
        cases += capture_patch(tset, bpm=bpm)

    b = open("out/arm9.bin", "rb").read()
    bb = 0x02004000
    s16 = lambda a, n: [struct.unpack_from("<h", b, a - bb + 2 * i)[0]
                        for i in range(n)]
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - bb + 4 * i)[0])
                         for i in range(n)]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"PAT1" + struct.pack("<II", len(cases), len(P_FIELDS)))
        f.write(struct.pack("<128i", *u32t(0x0207FD80, 128)))   # env rate
        f.write(struct.pack("<16i", *u32t(0x0207FF80, 16)))     # lfo bpm
        f.write(struct.pack("<128i", *u32t(0x0207FFC4, 128)))   # lfo free
        f.write(struct.pack("<128i", *u32t(0x020801C4, 128)))   # porta
        f.write(struct.pack("<128i", *u32t(0x020805C4, 128)))   # peak
        for knobs, bpm, out in cases:
            f.write(struct.pack("<31b", *knobs))
            f.write(struct.pack("<i", bpm))
            f.write(struct.pack(f"<{len(P_FIELDS)}i", *out))
    print(f"{path}: {len(cases)} patches, {len(P_FIELDS)} fields each")


# --- end-to-end voice capture -----------------------------------------------
# Field lists must match the C struct member order exactly. (offset, size);
# size 1 = byte, 2 = signed halfword, 4 = word.
V_LFO = [(0x00, 4), (0x04, 4), (0x08, 4), (0x0C, 4), (0x10, 4),
         (0x14, 4), (0x18, 4), (0x1C, 4), (0x24, 4)]
V_ENV = [(0x04, 1), (0x08, 4), (0x0C, 4)]
V_ENVP = [(0x00, 4), (0x04, 4), (0x08, 4), (0x0C, 4)]
V_NC = [(0x00, 4), (0x04, 4), (0x0C, 4), (0x10, 1)]
V_PC = [(0x20, 4), (0x24, 4), (0x28, 4), (0x2C, 4), (0x30, 4), (0x34, 2),
        (0x38, 4), (0x3C, 4), (0x40, 2), (0x44, 2), (0x48, 2),
        (0x4C, 4), (0x50, 4), (0x54, 4)]
V_VCO = [(0x0C, 4), (0x14, 4), (0x18, 4), (0x1C, 4), (0x20, 4), (0x24, 4),
         (0x28, 4), (0x2C, 4), (0x30, 4), (0x38, 4), (0x3C, 4), (0x40, 4),
         (0x44, 4), (0x48, 4), (0x4C, 4), (0x50, 4), (0x60, 4), (0x64, 4),
         (0x68, 4), (0x6C, 4), (0x54, 4), (0x58, 4), (0x5C, 4)]
V_VCF = [(0x24, 4), (0x34, 4), (0x38, 4), (0x40, 4), (0x50, 4), (0x54, 4),
         (0x5C, 4), (0x60, 4), (0x04, 4), (0x08, 4), (0x0C, 4), (0x20, 4),
         (0x1C, 4), (0x28, 4), (0x3C, 4), (0x44, 4), (0x48, 4), (0x4C, 4),
         (0x58, 4)]
V_AMP = [(0x0C, 4), (0x20, 4), (0x2C, 4), (0x30, 4), (0x34, 4), (0x38, 4),
         (0x3C, 4), (0x04, 4), (0x08, 4), (0x18, 4), (0x1C, 4), (0x24, 4),
         (0x28, 4)]


def capture_voice(params, note, frames=4):
    """Whole-voice state before a frame, and the 64 samples the original wrote."""
    o = Oracle()
    ptr = {k: o.u32(o.syn + off) for k, off in
           (("lfo", 0x34), ("env", 0x30), ("nc", 0x38), ("pc", 0x3C),
            ("vco", 0x40), ("vcf", 0x44), ("amp", 0x48))}
    o.load_patch(params)

    def read(base, fields, size=0x100):
        raw = bytes(o.uc.mem_read(base, size))
        return [_rdf(raw, off, sz) for off, sz in fields]

    def snap():
        envp = o.u32(ptr["env"])
        return (read(ptr["lfo"], V_LFO, 0x34) + read(ptr["env"], V_ENV, 0x14) +
                read(envp, V_ENVP, 0x10) + read(ptr["nc"], V_NC, 0x14) +
                read(ptr["pc"], V_PC, 0x58) + read(ptr["vco"], V_VCO, 0xA0) +
                read(ptr["vcf"], V_VCF, 0x64) + read(ptr["amp"], V_AMP, 0x40))

    o.note_on(note)
    o.call(RENDER, o.syn)              # event frame, not measured
    cases = []
    for _ in range(frames):
        before = snap()
        o.call(RENDER, o.syn)
        out = list(struct.unpack(f"<{FRAME}h",
                                o.uc.mem_read(OUTBUF, FRAME * 2)))
        cases.append((before, out))
    return cases


NFIELD_TOTAL = None


def cmd_voice_vectors(path="tests/vectors/voice.bin"):
    """End-to-end: every factory patch plus parameter sweeps, whole frames."""
    _n, init = factory_patch(0)
    cases = []
    for idx in range(24):
        _nm, p = factory_patch(idx)
        for note in (36, 60, 84):
            cases += capture_voice(p, note)
    for w1 in range(4):
        for w2 in range(4):
            p = list(init)
            p[6], p[7] = w1, w2
            cases += capture_voice(p, 60)
    for cutoff in (0, 40, 80, 127):
        for peak in (0, 100, 127):
            for ftype in (0, 1, 2):
                p = list(init)
                p[11], p[12], p[13] = cutoff, peak, ftype
                cases += capture_voice(p, 60)
    for drive in (0, 64, 127):
        for sync in (0, 1):
            p = list(init)
            p[18], p[10] = drive, sync
            cases += capture_voice(p, 60)

    b = open("out/arm9.bin", "rb").read()
    base = 0x02004000
    s16 = lambda a, n: [struct.unpack_from("<h", b, a - base + 2 * i)[0]
                        for i in range(n)]
    u32t = lambda a, n: [_s32(struct.unpack_from("<I", b, a - base + 4 * i)[0])
                         for i in range(n)]
    nf = len(cases[0][0])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VOI1" + struct.pack("<II", len(cases), nf))
        f.write(struct.pack("<128i", *u32t(DEPTH_TABLE, 128)))
        f.write(struct.pack("<130i", *u32t(PITCH_TABLE, 130)))
        f.write(struct.pack("<128h", *s16(0x020803C4, 128)))
        f.write(struct.pack("<128h", *s16(0x020804C4, 128)))
        f.write(struct.pack("<152i", *u32t(0x020807C4, 152)))
        for state, out in cases:
            f.write(struct.pack(f"<{nf}i", *state))
            f.write(struct.pack(f"<{FRAME}h", *out))
    print(f"{path}: {len(cases)} frames, {nf} state fields, "
          f"{len(cases) * FRAME} samples")


AMP_PTR = 0x48             # synth + 0x48
# ds10_amp field order.
AMP_FIELDS = [0x0C, 0x20, 0x2C, 0x30, 0x34, 0x38, 0x3C]


def capture_amp(params, note, frames=4):
    """Filter output + VCO2 output + AMP state per frame, and the samples out.

    The expected output is the voice's own render buffer, so this closes the
    per-sample chain: oscillator -> filter -> amp -> buffer.
    """
    o = Oracle()
    aobj = o.u32(o.syn + AMP_PTR)
    o.add_probe("in", *VCF_OUT)
    o.add_probe("vco2", *VCO2_OUT)

    snap = {"armed": False, "state": None}

    def on_loop_head(u, _a, _s, _x):
        if snap["armed"]:
            raw = bytes(u.mem_read(aobj, 0x40))
            snap["state"] = [_s32(struct.unpack_from("<I", raw, off)[0])
                             for off in AMP_FIELDS]
            snap["armed"] = False
    o.uc.hook_add(UC_HOOK_CODE, on_loop_head, begin=LOOP_HEAD, end=LOOP_HEAD)

    o.load_patch(params)
    o.note_on(note)

    cases = []
    for _ in range(frames):
        o.probed["in"].clear()
        o.probed["vco2"].clear()
        snap["armed"] = True
        snap["state"] = None
        o.call(RENDER, o.syn)
        if snap["state"] is None:
            continue
        out = list(struct.unpack(f"<{FRAME}h",
                                o.uc.mem_read(OUTBUF, FRAME * 2)))
        cases.append((snap["state"], list(o.probed["in"]),
                      list(o.probed["vco2"]), out))
    return cases


def cmd_amp_vectors(path="tests/vectors/amp.bin"):
    """Sweep drive, level, the distortion switch and the factory patches."""
    _n, init = factory_patch(0)
    cases = []
    for drive in (0, 20, 60, 100, 127):
        for level in (0, 40, 80, 127):
            p = list(init)
            p[18], p[19] = drive, level
            cases += capture_amp(p, 60)
    for w2 in range(4):
        for drive in (0, 90, 127):
            p = list(init)
            p[7], p[18] = w2, drive
            cases += capture_amp(p, 60)
    for note in (36, 60, 84):
        cases += capture_amp(list(init), note)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        cases += capture_amp(p, 60)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"AMP1" + struct.pack("<I", len(cases)))
        for state, ins, vco2, out in cases:
            f.write(struct.pack(f"<{len(AMP_FIELDS)}i", *state))
            f.write(struct.pack("<I", len(ins)))
            f.write(struct.pack(f"<{len(ins)}i", *ins))
            f.write(struct.pack(f"<{len(vco2)}i", *vco2))
            f.write(struct.pack(f"<{len(out)}h", *out[:len(ins)]))
    total = sum(len(c[1]) for c in cases)
    print(f"{path}: {len(cases)} frames, {total} samples")


NC_PTR = 0x38              # synth + 0x38  NOTECTRL
PC_PTR = 0x3C              # synth + 0x3C  PITCHCTRL
DEPTH_TABLE = 0x0207EB38
PITCH_TABLE = 0x0207E950

# NOTECTRL: target, current, rate, dir(byte)
NC_FIELDS = [(0x00, 4), (0x04, 4), (0x0C, 4), (0x10, 1)]
# PITCHCTRL fields the preamble reads/writes, in ds10_pitchctrl order.
PC_FIELDS = [(0x20, 4), (0x24, 4), (0x28, 4), (0x2C, 4), (0x30, 4),
             (0x34, 2), (0x38, 4), (0x3C, 4),
             (0x40, 2), (0x44, 2), (0x48, 2),
             (0x4C, 4), (0x50, 4), (0x54, 4)]


def _rdf(raw, off, size):
    if size == 1:
        return raw[off]
    if size == 2:
        return struct.unpack_from("<h", raw, off)[0]
    return _s32(struct.unpack_from("<I", raw, off)[0])


def capture_pitch(params, note, frames):
    """NOTECTRL + PITCHCTRL transitions, plus the ENV/LFO values they read."""
    o = Oracle()
    nc = o.u32(o.syn + NC_PTR)
    pc = o.u32(o.syn + PC_PTR)
    env = o.u32(o.syn + ENV_PTR)
    lfo = o.u32(o.syn + LFO_PTR)
    o.load_patch(params)

    def snap():
        nraw = bytes(o.uc.mem_read(nc, 0x14))
        praw = bytes(o.uc.mem_read(pc, 0x58))
        eraw = bytes(o.uc.mem_read(env, 0x14))
        lraw = bytes(o.uc.mem_read(lfo, 0x34))
        return (
            [_rdf(nraw, off, sz) for off, sz in NC_FIELDS],
            [_rdf(praw, off, sz) for off, sz in PC_FIELDS],
            [_s32(struct.unpack_from("<I", eraw, 8 + 4 * i)[0]) for i in range(3)],
            [_s32(struct.unpack_from("<I", lraw, 4 * i)[0]) for i in range(5)],
        )

    o.note_on(note)
    o.call(RENDER, o.syn)              # event frame, not measured
    cases = []
    for _ in range(frames):
        before = snap()
        o.call(RENDER, o.syn)
        after = snap()
        # Preamble order is LFO, ENV, NOTECTRL, PITCHCTRL - so by the time the
        # pitch block runs, ENV and LFO have already advanced this frame. Their
        # post-call values are what it actually read; the pre-call ones are a
        # frame stale.
        cases.append(((before[0], before[1], after[2], after[3]),
                      after[0][1], after[1][6], after[1][7]))
    return cases


def cmd_pitch_vectors(path="tests/vectors/pitch.bin"):
    """Sweep octave, portamento, detune, EG/LFO routing and factory patches."""
    _n, init = factory_patch(0)
    cases = []
    for octave in (-2, -1, 0, 1, 2):
        for porta in (0, 30, 90, 127):
            p = list(init)
            p[0], p[2] = octave, porta
            cases += capture_pitch(p, 60, 20)
    for vco2p in (-63, -20, 0, 20, 63):
        for egint in (-63, 0, 63):
            p = list(init)
            p[8], p[1] = vco2p, egint
            cases += capture_pitch(p, 60, 12)
    for src in range(7):
        for depth in (-63, 0, 63):
            p = list(init)
            p[26], p[3] = src, depth
            p[27], p[4] = src, depth
            cases += capture_pitch(p, 60, 12)
    for note in (24, 48, 60, 84, 108):
        cases += capture_pitch(list(init), note, 12)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        cases += capture_pitch(p, 60, 12)

    b = open("out/arm9.bin", "rb").read()
    base = 0x02004000
    tbl = lambda a, n: [_s32(struct.unpack_from("<I", b, a - base + 4 * i)[0])
                        for i in range(n)]
    depth = tbl(DEPTH_TABLE, 128)
    pitch = tbl(PITCH_TABLE, 130)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"PIT1" + struct.pack("<I", len(cases)))
        f.write(struct.pack("<128i", *depth))
        f.write(struct.pack("<130i", *pitch))
        for (ncv, pcv, envv, lfov), want_cur, want_i1, want_i2 in cases:
            f.write(struct.pack(f"<{len(NC_FIELDS)}i", *ncv))
            f.write(struct.pack(f"<{len(PC_FIELDS)}i", *pcv))
            f.write(struct.pack("<3i", *envv))
            f.write(struct.pack("<5i", *lfov))
            f.write(struct.pack("<3i", want_cur, want_i1, want_i2))
    print(f"{path}: {len(cases)} frame transitions")


LFO_PTR = 0x34             # synth + 0x34
# Must match the ds10_lfo struct layout, including the never-written +0x10
# slot the modulation routing uses to mean "no contribution".
LFO_FIELDS = [("out0", 0x00), ("out1", 0x04), ("out2", 0x08), ("out3", 0x0C),
              ("out4", 0x10), ("phase_down", 0x14), ("inc", 0x18),
              ("phase_up", 0x1C), ("lfsr", 0x24)]


def capture_lfo(params, note, frames):
    """LFO state transitions, one per frame. Same event-frame caveat as ENV."""
    o = Oracle()
    lobj = o.u32(o.syn + LFO_PTR)
    o.load_patch(params)

    def state():
        raw = bytes(o.uc.mem_read(lobj, 0x34))
        return [_s32(struct.unpack_from("<I", raw, off)[0])
                for _n, off in LFO_FIELDS]

    o.note_on(note)
    o.call(RENDER, o.syn)              # event frame, not measured
    cases = []
    for _ in range(frames):
        before = state()
        o.call(RENDER, o.syn)
        cases.append((before, state()))
    return cases


def cmd_lfo_vectors(path="tests/vectors/lfo.bin"):
    """Sweep LFO rate and BPM sync across notes and the factory patches."""
    _n, init = factory_patch(0)
    cases = []
    for freq in (0, 8, 24, 48, 72, 100, 127):
        for bpm in (0, 1):
            p = list(init)
            p[24], p[25] = freq, bpm
            cases += capture_lfo(p, 60, 40)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        cases += capture_lfo(p, 60, 24)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"LFO1" + struct.pack("<I", len(cases)))
        for before, after in cases:
            f.write(struct.pack(f"<{len(LFO_FIELDS)}i", *before))
            f.write(struct.pack(f"<{len(LFO_FIELDS)}i", *after))
    print(f"{path}: {len(cases)} frame transitions")


ENV_PTR = 0x30             # synth + 0x30


def capture_env(params, note, frames, release_at=None):
    """Envelope state transitions, one per frame.

    The envelope advances once per frame in the render preamble, so this is a
    state-transition test rather than a sample stream.

    Events are dispatched INSIDE render, before the envelope block, so a
    snapshot taken before the call would miss a note-on/off changing the stage.
    Each event therefore gets one discarded frame, after which no events are
    pending and the pre-call state is exactly what the envelope block sees.
    """
    o = Oracle()
    eobj = o.u32(o.syn + ENV_PTR)
    o.load_patch(params)

    def state():
        raw = bytes(o.uc.mem_read(eobj, 0x14))
        pblk = struct.unpack_from("<I", raw, 0)[0]
        praw = bytes(o.uc.mem_read(pblk, 0x10))
        return ([_s32(struct.unpack_from("<I", praw, 4 * i)[0]) for i in range(4)],
                raw[4],
                _s32(struct.unpack_from("<I", raw, 8)[0]),
                _s32(struct.unpack_from("<I", raw, 12)[0]))

    cases = []

    def run(n):
        for _ in range(n):
            pblk, stage, level, aux = state()
            o.call(RENDER, o.syn)
            _p2, stage2, level2, aux2 = state()
            cases.append((pblk, stage, level, aux, stage2, level2, aux2))

    o.note_on(note)
    o.call(RENDER, o.syn)              # event frame, not measured
    if release_at is None:
        run(frames)
    else:
        run(release_at)
        o.note_off()
        o.call(RENDER, o.syn)          # event frame, not measured
        run(frames - release_at)
    return cases


def cmd_env_vectors(path="tests/vectors/env.bin"):
    """Sweep A/D/S/R across the factory patches, with and without note-off."""
    _n, init = factory_patch(0)
    cases = []
    for a in (0, 20, 60, 127):
        for d in (0, 40, 127):
            for s in (0, 64, 127):
                for r in (0, 50, 127):
                    p = list(init)
                    p[20], p[21], p[22], p[23] = a, d, s, r
                    cases += capture_env(p, 60, 24, release_at=12)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        cases += capture_env(p, 60, 16, release_at=8)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"ENV1" + struct.pack("<I", len(cases)))
        for pblk, st, lv, ax, st2, lv2, ax2 in cases:
            f.write(struct.pack("<4i", *pblk))
            f.write(struct.pack("<iii", st, lv, ax))
            f.write(struct.pack("<iii", st2, lv2, ax2))
    print(f"{path}: {len(cases)} frame transitions")


def cmd_vco_vectors(path="tests/vectors/vco.bin"):
    """Sweep both waveforms, sync, cross-mod, balance and the factory patches."""
    _n, init = factory_patch(0)
    cases = []

    def add(params, note):
        cases.extend(capture_vco(params, note))

    for w1 in range(4):
        for w2 in range(4):
            p = list(init)
            p[6], p[7] = w1, w2
            add(p, 60)
    for sync in (0, 1):
        for vco2pitch in (-63, -20, 0, 20, 63):
            p = list(init)
            p[10], p[8] = sync, vco2pitch
            add(p, 60)
    for bal in (0, 32, 64, 96, 127):
        p = list(init)
        p[9] = bal
        add(p, 60)
    for note in (24, 36, 48, 72, 84, 96):
        add(list(init), note)
    for idx in range(24):
        _nm, p = factory_patch(idx)
        add(p, 60)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"VCO1" + struct.pack("<I", len(cases)))
        for state, mix in cases:
            f.write(struct.pack(f"<{len(VCO_ORDER)}i", *state))
            f.write(struct.pack("<I", len(mix)))
            f.write(struct.pack(f"<{len(mix)}i", *mix))
    total = sum(len(c[1]) for c in cases)
    print(f"{path}: {len(cases)} frames, {total} samples")


def cmd_vectors(path="tests/vectors/vcf.bin"):
    """Sweep cutoff, peak, type and the factory patches; write test vectors."""
    _n, init = factory_patch(0)
    cases = []

    def add(params, note, tag):
        got = capture_vcf(params, note)
        n = sum(len(c[1]) for c in got)
        if n == 0:
            print(f"  {tag}: no samples captured - skipped")
            return
        cases.extend(got)

    for cutoff in (0, 16, 32, 48, 64, 80, 96, 112, 127):
        for peak in (0, 40, 80, 110, 127):
            p = list(init)
            p[11], p[12] = cutoff, peak
            add(p, 60, f"cutoff={cutoff} peak={peak}")
    for ftype in (0, 1, 2):
        for note in (36, 60, 84):
            p = list(init)
            p[11], p[12], p[13] = 70, 90, ftype
            add(p, note, f"type={ftype} note={note}")
    for idx in range(24):
        _nm, p = factory_patch(idx)
        add(p, 60, f"patch{idx}")

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(VEC_MAGIC + struct.pack("<I", len(cases)))
        for state, ins, outs in cases:
            f.write(struct.pack("<8i", *state))
            f.write(struct.pack("<I", len(ins)))
            f.write(struct.pack(f"<{len(ins)}i", *ins))
            f.write(struct.pack(f"<{len(outs)}i", *outs))
    total = sum(len(c[1]) for c in cases)
    print(f"{path}: {len(cases)} frames, {total} samples")
    print("state field order: " + ", ".join(VCF_ORDER))


def cmd_waves(note="60"):
    """Sweep the vco1 waveform selector with everything else neutral."""
    _n, base = factory_patch(0)                    # INIT
    for wf in range(4):
        p = list(base)
        p[6] = wf                                  # knob 6 = vco1
        p[9] = 0                                   # balance -> vco1 only
        o = Oracle()
        o.load_patch(p)
        o.note_on(int(note))
        s = o.render(8)
        seg = s[320:320 + 40]
        print(f"vco1={wf}: min {min(s):6} max {max(s):6}  samples 320..360:")
        print("   " + " ".join(f"{v:6d}" for v in seg[:20]))
        print("   " + " ".join(f"{v:6d}" for v in seg[20:]))


def main():
    cmds = {"probe": cmd_probe, "render": cmd_render, "wav": cmd_wav,
            "waves": cmd_waves, "vectors": cmd_vectors,
            "vco-vectors": cmd_vco_vectors,
            "env-vectors": cmd_env_vectors,
            "lfo-vectors": cmd_lfo_vectors,
            "pitch-vectors": cmd_pitch_vectors,
            "amp-vectors": cmd_amp_vectors,
            "vcfupd-vectors": cmd_vcfupd_vectors,
            "voice-vectors": cmd_voice_vectors,
            "patch-vectors": cmd_patch_vectors,
            "fx-vectors": cmd_fx_vectors,
            "mixer-vectors": cmd_mixer_vectors,
            "drum-vectors": cmd_drum_vectors,
            "event-vectors": cmd_event_vectors,
            "seq-vectors": cmd_seq_vectors,
            "song-vectors": cmd_song_vectors,
            "replay-vectors": cmd_replay_vectors}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    cmds[sys.argv[1]](*sys.argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
