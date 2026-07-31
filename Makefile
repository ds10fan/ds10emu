ROM ?= ds10plus.nds
PY  ?= python3
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
export DS10_ROM = $(ROM)

CORE_HDRS = src/core/fixed.h

.PHONY: test-obj test-page web serve ds10view sweep test-2d ndsref recon extract oracle ghidra-image test test-vcf test-vco test-env test-lfo test-pitch test-amp test-vcfupd test-voice test-patch test-fx test-mixer test-drum test-event test-seq test-song test-replay test-songrender songplay tables clean

# Differential tests: our C against the original ARM code, bit-exact.
test: test-obj test-voice test-patch test-event test-seq test-replay test-songrender test-song test-fx test-mixer test-drum test-vcf test-vcfupd test-vco test-env test-lfo test-pitch test-amp

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
                src/core/mixer_state.c src/core/mixer.h src/core/fx.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_mixer.c src/core/mixer.c src/core/fx.c \
	    src/core/mixer_state.c

test-drum: out/test_drum tests/vectors/drum.bin
	@./out/test_drum

out/test_drum: tests/test_drum.c src/core/drum.c src/core/drum.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_drum.c src/core/drum.c

test-event: out/test_event tests/vectors/event.bin
	@./out/test_event

out/test_event: tests/test_event.c src/core/event.c src/core/patch.c \
                src/core/vcf.c src/core/event.h $(CORE_HDRS)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_event.c src/core/event.c \
	    src/core/patch.c src/core/vcf.c

test-seq: out/test_seq tests/vectors/seq.bin
	@./out/test_seq

# The whole chain: a demo song's patches and steps through the sequencer into two
# synth voices, bit-compared against the original doing the same.
SONG_SRC = src/core/seq.c src/core/song.c src/core/md5.c src/core/tables.c \
           src/core/patch.c src/core/event.c src/core/voice.c src/core/vco.c \
           src/core/vcf.c src/core/env.c src/core/lfo.c src/core/pitch.c \
           src/core/amp.c src/core/fx.c src/core/drum.c src/core/voice_state.c \
           src/core/mixer.c src/core/mixer_state.c

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

# data/tables.bin is committed, so the engine and its tests build and run with
# no ROM present. This target re-extracts it and reports any difference, which
# is the check that the committed copy is still the ROM's.
tables:
	@$(PY) tools/oracle.py tables out/tables.bin
	@cmp out/tables.bin data/tables.bin && echo "data/tables.bin: unchanged"

test-replay: out/test_replay tests/vectors/replay.bin
	@./out/test_replay

out/test_replay: tests/test_replay.c src/core/seq.c src/core/seq.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_replay.c src/core/seq.c

test-song: out/test_song tests/vectors/song.bin
	@./out/test_song

out/test_song: tests/test_song.c src/core/song.c src/core/md5.c \
               src/core/song.h src/core/md5.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_song.c src/core/song.c src/core/md5.c

out/test_seq: tests/test_seq.c src/core/seq.c src/core/seq.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_seq.c src/core/seq.c

# End-to-end: a whole voice, which subsumes the per-stage tests above. Those
# stay because they localise a failure that this one only reports.
CORE_SRC = src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
           src/core/lfo.c src/core/pitch.c src/core/amp.c

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
# GUI
# ---------------------------------------------------------------------------

# ndsref runs the ROM in melonDS and dumps, per frame, the whole 2D state plus
# the framebuffers melonDS composited from it. That is the pixel ground truth
# for the renderer; Unicorn stops at the CPU and cannot provide it.
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

# The 2D hardware model, against melonDS. `make sweep` walks the whole
# interface and snapshots each screen; test-2d renders every snapshot and
# compares pixel for pixel.
sweep: out/ndsref
	@rm -rf out/sweep
	@$(PY) tools/sweep.py out/sweep

test-2d: out/test_2d
	@test -n "$$(ls out/sweep/*.snap 2>/dev/null)" || { \
	  echo "no snapshots: run 'make sweep' first"; exit 1; }
	@./out/test_2d out/sweep/*.snap

out/test_2d: tests/test_2d.c src/gfx/nds2d.c src/gfx/nds2d.h src/gfx/color.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_2d.c src/gfx/nds2d.c

# ---------------------------------------------------------------------------
# Browser build
# ---------------------------------------------------------------------------

# Needs the Emscripten SDK; source its emsdk_env.sh, or point EMCC at emcc.
EMCC ?= emcc
WEB_SRC = src/web/main.c src/app/ui.c src/app/obj.c \
          src/gfx/nds2d.c src/gfx/nitro.c \
          src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
          src/core/lfo.c src/core/pitch.c src/core/amp.c src/core/fx.c \
          src/core/mixer.c src/core/patch.c src/core/event.c \
          src/core/tables.c src/core/voice_state.c src/core/mixer_state.c
WEB_FLAGS = -O2 -std=c99 -Wall \
            --preload-file data/nitrofs@/data/nitrofs \
            --preload-file data/pages.bin@/data/pages.bin \
            --preload-file data/tables.bin@/data/tables.bin \
            -s EXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8"]' \
            -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=64MB -s EXIT_RUNTIME=0

web: web/build/ds10.js
web/build/ds10.js: $(WEB_SRC) src/app/ui.h src/gfx/nds2d.h src/gfx/nitro.h \
                   data/pages.bin data/tables.bin web/index.html \
                   web/app.js
	@mkdir -p web/build
	$(EMCC) $(WEB_FLAGS) -o web/build/ds10.js $(WEB_SRC)
	@cp web/index.html web/app.js web/build/

# Serve it. The page is static, so anything that serves files will do.
serve: web
	@echo "http://0.0.0.0:$(PORT)/"
	@cd web/build && $(PY) -m http.server $(PORT) --bind 0.0.0.0
PORT ?= 8537

# Render a page natively, as a check that the browser build is not the odd one
# out. ds10view writes PPMs; the wasm build produces the same pixels.
ds10view: out/ds10view
out/ds10view: src/app/ds10view.c $(WEB_SRC:src/web/main.c=) src/app/ui.c \
              src/gfx/nds2d.c src/gfx/nitro.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/app/ds10view.c src/app/ui.c src/gfx/nds2d.c \
	    src/gfx/nitro.c

# Render each page with the reimplementation and diff it against the captured
# frame for the same page. Needs `make sweep` first.
# The sprite widget: our OAM builder against the game's own setter, run under
# Unicorn. This is the piece that turns widget state into what the screen
# shows, so it is the seam between the UI and the hardware model.
test-obj: out/test_obj tests/vectors/obj.bin
	@./out/test_obj

out/test_obj: tests/test_obj.c src/app/obj.c src/app/obj.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_obj.c src/app/obj.c

tests/vectors/obj.bin: extract tools/oracle.py
	@$(PY) tools/oracle.py obj-vectors

test-page: out/ds10view
	@$(PY) tools/pagecheck.py out/sweep

# The captured sprite layer per page, until the widget constructors are read.
data/pages.bin: tools/pages.py
	@$(PY) tools/pages.py out/sweep $@
