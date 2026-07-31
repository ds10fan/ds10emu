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


LFO_PTR = 0x34             # synth + 0x34
LFO_FIELDS = [("out0", 0x00), ("out1", 0x04), ("out2", 0x08), ("out3", 0x0C),
              ("phase_down", 0x14), ("inc", 0x18), ("phase_up", 0x1C),
              ("lfsr", 0x24)]


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
            "lfo-vectors": cmd_lfo_vectors}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        print(__doc__)
        return 1
    cmds[sys.argv[1]](*sys.argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
