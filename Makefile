ROM ?= ds10plus.nds
PY  ?= python3
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
export DS10_ROM = $(ROM)

CORE_HDRS = src/core/fixed.h src/core/tables_data.h
# The ROM tables the engine indexes by name; every binary that
# links any of the engine needs them.
TBL_SRC = src/core/tables_data.c

.PHONY: test-songwalk test-songhw test-songtext test-drumfx test-drumbuf test-tables ndsref recon extract oracle ghidra-image test test-vcf test-vco test-env test-lfo test-pitch test-amp test-vcfupd test-voice test-patch test-fx test-mixer test-drum test-event test-seq test-song test-replay test-songrender songplay tables clean

# Differential tests: our C against the original ARM code, bit-exact.
test: test-drumfx test-drumbuf test-tables test-voice test-patch test-event test-seq test-replay test-songrender test-song test-fx test-mixer test-drum test-vcf test-vcfupd test-vco test-env test-lfo test-pitch test-amp

test-vcf: out/test_vcf tests/vectors/vcf.bin
	@./out/test_vcf

out/test_vcf: tests/test_vcf.c src/core/vcf.c src/core/vcf.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vcf.c src/core/vcf.c \
	    $(TBL_SRC)

test-vco: out/test_vco tests/vectors/vco.bin
	@./out/test_vco

out/test_vco: tests/test_vco.c src/core/vco.c src/core/vco.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vco.c src/core/vco.c \
	    $(TBL_SRC)

test-env: out/test_env tests/vectors/env.bin
	@./out/test_env

out/test_env: tests/test_env.c src/core/env.c src/core/env.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_env.c src/core/env.c \
	    $(TBL_SRC)

test-lfo: out/test_lfo tests/vectors/lfo.bin
	@./out/test_lfo

out/test_lfo: tests/test_lfo.c src/core/lfo.c src/core/lfo.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_lfo.c src/core/lfo.c \
	    $(TBL_SRC)

test-pitch: out/test_pitch tests/vectors/pitch.bin
	@./out/test_pitch

out/test_pitch: tests/test_pitch.c src/core/pitch.c src/core/pitch.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_pitch.c src/core/pitch.c \
	    $(TBL_SRC)

test-amp: out/test_amp tests/vectors/amp.bin
	@./out/test_amp

out/test_amp: tests/test_amp.c src/core/amp.c src/core/amp.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_amp.c src/core/amp.c \
	    $(TBL_SRC)

test-vcfupd: out/test_vcfupd tests/vectors/vcfupd.bin
	@./out/test_vcfupd

out/test_vcfupd: tests/test_vcf_update.c src/core/vcf.c src/core/vcf.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_vcf_update.c src/core/vcf.c \
	    $(TBL_SRC)

test-patch: out/test_patch tests/vectors/patch.bin
	@./out/test_patch

out/test_patch: tests/test_patch.c src/core/patch.c src/core/vcf.c \
                src/core/patch.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_patch.c src/core/patch.c src/core/vcf.c \
	    $(TBL_SRC)

test-fx: out/test_fx tests/vectors/fx.bin
	@./out/test_fx

out/test_fx: tests/test_fx.c src/core/fx.c src/core/fx.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_fx.c src/core/fx.c \
	    $(TBL_SRC)

test-mixer: out/test_mixer tests/vectors/mixer.bin
	@./out/test_mixer

# A drum's own effect settings: the nine bytes at payload +0x2BA8 are a route and
# the eight effect parameters, pushed as mixer knobs 0..8 before that drum's
# one-second buffer is rendered. This is that stage against the original.
test-drumfx: out/test_drumfx tests/vectors/drumfx.bin
	@./out/test_drumfx

out/test_drumfx: tests/test_drumfx.c src/core/mixer.c src/core/fx.c \
                 src/core/mixer_state.c src/core/mixer.h src/core/fx.h \
                 $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_drumfx.c src/core/mixer.c src/core/fx.c \
	    src/core/mixer_state.c \
	    $(TBL_SRC)

tests/vectors/drumfx.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py drumfx-vectors

# A drum buffer against one a running DS rendered. The vectors are committed
# because making them needs melonDS as well as the ROM: `tools/drumref.py
# vectors` packs each song into a save, boots it and reads the buffers out of
# main RAM.
test-drumbuf: out/test_drumbuf tests/vectors/drumbuf.bin
	@./out/test_drumbuf

out/test_drumbuf: tests/test_drumbuf.c src/core/drumbuf.c src/core/fx.c \
                  src/core/mixer_state.c src/core/patch.c src/core/event.c \
                  src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
                  src/core/lfo.c src/core/pitch.c src/core/amp.c \
                  src/core/voice_state.c src/core/drumbuf.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_drumbuf.c src/core/drumbuf.c \
	    src/core/fx.c src/core/mixer_state.c src/core/patch.c src/core/event.c \
	    src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
	    src/core/lfo.c src/core/pitch.c src/core/amp.c src/core/voice_state.c \
	    $(TBL_SRC)

out/test_mixer: tests/test_mixer.c src/core/mixer.c src/core/fx.c \
                src/core/mixer_state.c src/core/mixer.h src/core/fx.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_mixer.c src/core/mixer.c src/core/fx.c  \
	    src/core/mixer_state.c \
	    $(TBL_SRC)

test-drum: out/test_drum tests/vectors/drum.bin
	@./out/test_drum

out/test_drum: tests/test_drum.c src/core/drum.c src/core/drum.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_drum.c src/core/drum.c \
	    $(TBL_SRC)

test-event: out/test_event tests/vectors/event.bin
	@./out/test_event

out/test_event: tests/test_event.c src/core/event.c src/core/patch.c \
                src/core/vcf.c src/core/event.h $(CORE_HDRS) $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_event.c src/core/event.c  \
	    src/core/patch.c src/core/vcf.c \
	    $(TBL_SRC)

test-seq: out/test_seq tests/vectors/seq.bin
	@./out/test_seq

# The whole chain: a demo song's patches and steps through the sequencer into two
# synth voices, bit-compared against the original doing the same.
SONG_SRC = src/core/seq.c src/core/song.c src/core/md5.c src/core/drumbuf.c \
           src/core/patch.c src/core/event.c src/core/voice.c src/core/vco.c \
           src/core/vcf.c src/core/env.c src/core/lfo.c src/core/pitch.c \
           src/core/amp.c src/core/fx.c src/core/drum.c src/core/voice_state.c \
           src/core/mixer.c src/core/mixer_state.c $(TBL_SRC)

test-songrender: out/test_songrender tests/vectors/songrender.bin
	@./out/test_songrender

out/test_songrender: tests/test_songrender.c $(SONG_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_songrender.c $(SONG_SRC)

# Render a song file to a WAV with nothing but the reimplementation.
songplay: out/songplay
out/songplay: src/app/songplay.c $(SONG_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/app/songplay.c $(SONG_SRC)

# data/tables.bin is committed, and src/core/tables_data.c is generated from
# it, which is what the engine reads - so everything builds and runs with no
# ROM present. This target re-extracts the binary and reports any difference,
# which is the check that the committed copy is still the ROM's; test-tables
# is the check that the generated C still matches the binary.
tables:
	@$(PY) tools/oracle.py tables out/tables.bin
	@cmp out/tables.bin data/tables.bin && echo "data/tables.bin: unchanged"

test-replay: out/test_replay tests/vectors/replay.bin
	@./out/test_replay

out/test_replay: tests/test_replay.c src/core/seq.c src/core/seq.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_replay.c src/core/seq.c \
	    $(TBL_SRC)

test-song: out/test_song tests/vectors/song.bin
	@./out/test_song

out/test_song: tests/test_song.c src/core/song.c src/core/md5.c \
               src/core/song.h src/core/md5.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_song.c src/core/song.c src/core/md5.c \
	    $(TBL_SRC)

test-tables: out/test_tables
	@./out/test_tables

out/test_tables: tests/test_tables.c src/core/tables.c src/core/tables_data.c \
                 src/core/tables.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_tables.c src/core/tables.c  \
	    $(TBL_SRC)

out/test_seq: tests/test_seq.c src/core/seq.c src/core/seq.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_seq.c src/core/seq.c \
	    $(TBL_SRC)

# End-to-end: a whole voice, which subsumes the per-stage tests above. Those
# stay because they localise a failure that this one only reports.
# tables_data.c is in here because the engine indexes the ROM tables by name
# rather than being handed them; see src/core/tables_data.h.
CORE_SRC = src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
           src/core/lfo.c src/core/pitch.c src/core/amp.c $(TBL_SRC)

test-voice: out/test_voice tests/vectors/voice.bin
	@./out/test_voice

out/test_voice: tests/test_voice.c $(CORE_SRC) src/core/voice_state.c \
                src/core/voice.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_voice.c $(CORE_SRC) src/core/voice_state.c

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

tests/vectors/event.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py event-vectors

tests/vectors/seq.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py seq-vectors

tests/vectors/song.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py song-vectors

tests/vectors/replay.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py replay-vectors

tests/vectors/songrender.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py songrender-vectors

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

# ---------------------------------------------------------------------------
# melonDS
# ---------------------------------------------------------------------------

# ndsref runs the ROM in melonDS: whole-machine state Unicorn cannot provide,
# for the drum buffers (tools/drumref.py) and whole-song captures (songhw).
MELONDS ?= $(HOME)/p/melonDS
MELON_BUILD ?= $(MELONDS)/build-nojit
# The core's class layout depends on these, so the driver must be compiled
# with the same set the library was.
MELON_DEFS = -DARCHITECTURE_x86_64 -DGDBSTUB_ENABLED -DNDEBUG
MELON_LIBS = $(MELON_BUILD)/src/libcore.a \
             $(MELON_BUILD)/src/teakra/src/libteakra.a -lzstd -lpthread

ndsref: out/ndsref
out/ndsref: tools/ndsref/main.cpp tools/ndsref/platform.cpp
	@mkdir -p out
	@test -f $(MELON_BUILD)/src/libcore.a || { \
	  echo "no melonDS core at $(MELON_BUILD)."; \
	  echo "  git clone https://github.com/melonDS-emu/melonDS $(MELONDS)"; \
	  echo "  cmake -B $(MELON_BUILD) -G Ninja -DBUILD_QT_SDL=OFF \\"; \
	  echo "        -DENABLE_OGLRENDERER=OFF -DENABLE_JIT=OFF \\"; \
	  echo "        -DCMAKE_BUILD_TYPE=Release $(MELONDS)"; \
	  echo "  cmake --build $(MELON_BUILD)"; exit 1; }
	$(CXX) -std=c++17 -O2 $(MELON_DEFS) -o $@ $^ \
	  -I$(MELONDS)/src -I$(MELON_BUILD) $(MELON_LIBS)

# ---------------------------------------------------------------------------
# songtext - the text format
# ---------------------------------------------------------------------------

# Everything committed that is a song: the save slots and the ROM's own demos.
TEXT_SONGS = $(wildcard data/saves/*.ds10song) \
    $(wildcard data/nitrofs/demo/ds10*demo*.dat) data/nitrofs/demo/ds10init.dat

# The round trip: every song dumps to text and reads back byte for byte, and
# clean mode only ever loses the dead bytes of a step with no note. Python only -
# the format is still moving, and a second implementation to keep in step cost
# more than it caught. `tools/songtext.py` is the format.
test-songtext:
	@$(PY) tools/songtext.py check $(TEXT_SONGS)

# songwalk - a whole arrangement, the ROM's own mix bus against out/songplay,
# sample for sample. The ROM side is the oracle, so this needs the ROM and
# unicorn; a capture is a few seconds and is cached under out/songwalk.
SONGWALK ?= data/saves/new_00.ds10song
test-songwalk: songplay
	@$(PY) tools/songwalk.py check $(SONGWALK) 8

# songhw - a whole song played by the ROM under melonDS, against our own render.
# Needs the ROM and a melonDS build (see ndsref). One song is about two minutes of
# emulation, so this is not part of `make test`.
SONGHW ?= data/saves/xfer2_10.ds10song
test-songhw: out/ndsref songplay
	@$(PY) tools/songhw.py compare $(SONGHW) 24

# ---------------------------------------------------------------------------
# Assets the engine and its tests read
# ---------------------------------------------------------------------------

# The songs in songs/*.ds10save, extracted so they can be loaded by name.
# tools/sav.py pack puts one back into a slot.
data/saves: tools/sav.py $(wildcard songs/*.ds10save)
	@rm -rf data/saves && $(PY) tools/sav.py extract data/saves songs/*.ds10save \
	    | tail -1

# Both files come out of one run; the header is what the engine includes.
src/core/tables_data.c src/core/tables_data.h: data/tables.bin tools/mktables.py
	@$(PY) tools/mktables.py data/tables.bin src/core/tables_data.c
