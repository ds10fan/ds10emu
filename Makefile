ROM ?= ds10plus.nds
PY  ?= python3
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
export DS10_ROM = $(ROM)

CORE_HDRS = src/core/fixed.h

.PHONY: recon extract oracle ghidra-image test test-vcf test-vco test-env test-lfo test-pitch test-amp test-vcfupd test-voice test-patch test-fx test-mixer test-drum clean

# Differential tests: our C against the original ARM code, bit-exact.
test: test-voice test-patch test-fx test-mixer test-drum test-vcf test-vcfupd test-vco test-env test-lfo test-pitch test-amp

test-vcf: out/test_vcf tests/vectors/vcf.bin
	@./out/test_vcf

out/test_vcf: tests/test_vcf.c src/core/vcf.c src/core/vcf.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vcf.c src/core/vcf.c

test-vco: out/test_vco tests/vectors/vco.bin
	@./out/test_vco

out/test_vco: tests/test_vco.c src/core/vco.c src/core/vco.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vco.c src/core/vco.c

test-env: out/test_env tests/vectors/env.bin
	@./out/test_env

out/test_env: tests/test_env.c src/core/env.c src/core/env.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_env.c src/core/env.c

test-lfo: out/test_lfo tests/vectors/lfo.bin
	@./out/test_lfo

out/test_lfo: tests/test_lfo.c src/core/lfo.c src/core/lfo.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_lfo.c src/core/lfo.c

test-pitch: out/test_pitch tests/vectors/pitch.bin
	@./out/test_pitch

out/test_pitch: tests/test_pitch.c src/core/pitch.c src/core/pitch.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_pitch.c src/core/pitch.c

test-amp: out/test_amp tests/vectors/amp.bin
	@./out/test_amp

out/test_amp: tests/test_amp.c src/core/amp.c src/core/amp.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_amp.c src/core/amp.c

test-vcfupd: out/test_vcfupd tests/vectors/vcfupd.bin
	@./out/test_vcfupd

out/test_vcfupd: tests/test_vcf_update.c src/core/vcf.c src/core/vcf.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vcf_update.c src/core/vcf.c

test-patch: out/test_patch tests/vectors/patch.bin
	@./out/test_patch

out/test_patch: tests/test_patch.c src/core/patch.c src/core/vcf.c \
                src/core/patch.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_patch.c src/core/patch.c src/core/vcf.c

test-fx: out/test_fx tests/vectors/fx.bin
	@./out/test_fx

out/test_fx: tests/test_fx.c src/core/fx.c src/core/fx.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_fx.c src/core/fx.c

test-mixer: out/test_mixer tests/vectors/mixer.bin
	@./out/test_mixer

out/test_mixer: tests/test_mixer.c src/core/mixer.c src/core/fx.c \
                src/core/mixer.h src/core/fx.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_mixer.c src/core/mixer.c src/core/fx.c

test-drum: out/test_drum tests/vectors/drum.bin
	@./out/test_drum

out/test_drum: tests/test_drum.c src/core/drum.c src/core/drum.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_drum.c src/core/drum.c

# End-to-end: a whole voice, which subsumes the per-stage tests above. Those
# stay because they localise a failure that this one only reports.
CORE_SRC = src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
           src/core/lfo.c src/core/pitch.c src/core/amp.c

test-voice: out/test_voice tests/vectors/voice.bin
	@./out/test_voice

out/test_voice: tests/test_voice.c $(CORE_SRC) src/core/voice.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_voice.c $(CORE_SRC)

# Vectors are ROM-derived and never committed; regenerate them locally.
tests/vectors/vcf.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py vectors

tests/vectors/vco.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py vco-vectors

tests/vectors/env.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py env-vectors

tests/vectors/lfo.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py lfo-vectors

tests/vectors/pitch.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py pitch-vectors

tests/vectors/amp.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py amp-vectors

tests/vectors/vcfupd.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py vcfupd-vectors

tests/vectors/voice.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py voice-vectors

tests/vectors/patch.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py patch-vectors

tests/vectors/fx.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py fx-vectors

tests/vectors/mixer.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py mixer-vectors

tests/vectors/drum.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py drum-vectors

# Reproduce every artifact recon.md refers to, from the ROM alone.
recon: extract
	@echo '=== autoload memory map ==='
	@$(PY) tools/ndsx.py autoload
	@echo
	@echo '=== hardware register classification ==='
	@$(PY) tools/ndsx.py regs
	@echo
	@echo '=== factory patches ==='
	@$(PY) tools/ndsx.py patches
	@echo
	@echo '=== memory map validation (branch targets) ==='
	@$(PY) tools/probe.py branchcheck
	@echo
	@echo '=== memory map validation (both CPUs boot) ==='
	@$(PY) tools/emu.py smoke

# Execute the original ARM9 synth code and render audio (PLAN.md tier 1 oracle).
oracle: extract
	@$(PY) tools/oracle.py probe
	@echo
	@$(PY) tools/oracle.py waves

# Single sparse ARM9 image (ITCM + static) to import into Ghidra at 0x01FF8000.
ghidra-image: out/arm9_ghidra.bin

out/arm9_ghidra.bin: out/arm9.bin tools/mkimage.py
	@$(PY) tools/mkimage.py

extract: out/arm9.bin

out/arm9.bin: $(ROM) tools/ndsx.py
	@$(PY) tools/ndsx.py header
	@$(PY) tools/ndsx.py extract out
	@$(PY) tools/ndsx.py autoload >/dev/null

clean:
	rm -rf out
