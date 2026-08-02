ROM ?= ds10plus.nds
PY  ?= python3
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra
export DS10_ROM = $(ROM)

CORE_HDRS = src/core/fixed.h src/core/tables_data.h
# The ROM tables the engine indexes by name; every binary that
# links any of the engine needs them.
TBL_SRC = src/core/tables_data.c

.PHONY: fonttest test-uiknob test-uiaffine test-uilive test-tables test-webedit test-websong test-cell test-obj test-page web serve ds10view sweep test-2d ndsref recon extract oracle ghidra-image test test-vcf test-vco test-env test-lfo test-pitch test-amp test-vcfupd test-voice test-patch test-fx test-mixer test-drum test-event test-seq test-song test-replay test-songrender songplay tables clean test-editor

# Differential tests: our C against the original ARM code, bit-exact.
test: test-obj test-cell test-uiknob test-uiaffine test-tables test-editor test-voice test-patch test-event test-seq test-replay test-songrender test-song test-fx test-mixer test-drum test-vcf test-vcfupd test-vco test-env test-lfo test-pitch test-amp

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
SONG_SRC = src/core/seq.c src/core/song.c src/core/md5.c \
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

# The knob widget's angle map, against the original's own setter. The first
# UI function verified against the ARM rather than against a screenshot.
test-uiknob: out/test_uiknob tests/vectors/uiknob.bin
	@./out/test_uiknob

out/test_uiknob: tests/test_uiknob.c src/app/widget.c src/app/widget.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_uiknob.c src/app/widget.c $(TBL_SRC)

tests/vectors/uiknob.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py ui-vectors

# The same function against a running machine's own widgets, at their real
# addresses, with Unicorn seeded from a melonDS RAM dump. Needs the ROM.
test-uilive: out/test_uiknob tests/vectors/uilive.bin
	@./out/test_uiknob tests/vectors/uilive.bin

out/snapui/synedit/script.txt: tools/snapui.py out/ndsref
	@$(PY) tools/snapui.py synedit

tests/vectors/uilive.bin: out/snapui/synedit/script.txt tools/oracle.py \
                          tools/emu.py tools/snap.py
	@$(PY) tools/oracle.py ui-live $$(ls out/snapui/synedit/*.snap | tail -1)

# The sprite affine matrix, against the original's own builder.
test-uiaffine: out/test_uiaffine tests/vectors/uiaffine.bin
	@./out/test_uiaffine

out/test_uiaffine: tests/test_uiaffine.c src/app/widget.c src/app/widget.h \
                   $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_uiaffine.c src/app/widget.c $(TBL_SRC)

tests/vectors/uiaffine.bin: extract tools/oracle.py tools/emu.py
	@$(PY) tools/oracle.py ui-affine

test-tables: out/test_tables
	@./out/test_tables

out/test_tables: tests/test_tables.c src/core/tables.c src/core/tables_data.c \
                 src/core/tables.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_tables.c src/core/tables.c  \
	    $(TBL_SRC)

test-editor: out/test_editor
	@./out/test_editor

out/test_editor: tests/test_editor.c src/app/editor.c src/core/song.c \
                 src/core/md5.c src/app/editor.h src/core/song.h $(TBL_SRC)
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_editor.c src/app/editor.c \
	    src/core/song.c src/core/md5.c \
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
WEB_SRC = src/web/main.c src/app/ui.c src/app/obj.c src/app/cell.c \
          src/app/widget.c \
          src/app/editor.c \
          src/gfx/nds2d.c src/gfx/nitro.c \
          src/core/voice.c src/core/vco.c src/core/vcf.c src/core/env.c \
          src/core/lfo.c src/core/pitch.c src/core/amp.c src/core/fx.c \
          src/core/mixer.c src/core/patch.c src/core/event.c \
          src/core/voice_state.c src/core/mixer_state.c \
          src/core/seq.c src/core/song.c src/core/md5.c src/core/drum.c \
          $(TBL_SRC)
WEB_FLAGS = -O2 -std=c99 -Wall -lm \
            --preload-file data/nitrofs@/data/nitrofs \
            --preload-file data/saves@/data/saves \
            --preload-file data/pages.bin@/data/pages.bin \
            -s EXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8","FS"]' \
            -s INITIAL_MEMORY=64MB -s EXIT_RUNTIME=0

# The engine again, for the AudioWorklet: no preloaded files at all - the
# tables are compiled in and a song arrives over the port - and one file,
# because a worklet has no fetch. DSP then runs on the audio thread, where a
# slow frame cannot stall it.
AUDIO_FLAGS = -O2 -std=c99 -Wall -lm \
              -s EXPORTED_RUNTIME_METHODS='["cwrap","HEAPU8"]' \
              -s INITIAL_MEMORY=32MB -s EXIT_RUNTIME=0 \
              -s SINGLE_FILE=1 -s MODULARIZE=1 -s EXPORT_ES6=1 \
              -s EXPORT_NAME=DS10Audio -s ENVIRONMENT=worker

web/build/ds10audio.js: $(WEB_SRC)
	@mkdir -p web/build
	$(EMCC) $(AUDIO_FLAGS) -o web/build/ds10audio.js $(WEB_SRC)

# The songs in songs/*.SAV, so your own saves are in the picker too.
data/saves: tools/savex.py $(wildcard songs/*.SAV)
	@rm -rf data/saves && $(PY) tools/savex.py data/saves songs/*.SAV \
	    | tail -1
	@$(PY) tools/mksaveindex.py data/saves web/saves.js

# Both files come out of one run; the header is what the engine includes.
src/core/tables_data.c src/core/tables_data.h: data/tables.bin tools/mktables.py
	@$(PY) tools/mktables.py data/tables.bin src/core/tables_data.c

web: web/build/ds10.js web/build/ds10audio.js
web/build/ds10.js: $(WEB_SRC) src/app/ui.h src/gfx/nds2d.h src/gfx/nitro.h \
                   data/pages.bin data/tables.bin web/index.html \
                   web/app.js web/panel.js web/worklet.js web/saves.js
	@mkdir -p web/build
	$(EMCC) $(WEB_FLAGS) -o web/build/ds10.js $(WEB_SRC)
	@cp web/index.html web/app.js web/worklet.js web/panel.js \
	    web/saves.js web/build/

# Serve it. The page is static, so anything that serves files will do.
serve: web
	@echo "http://0.0.0.0:$(PORT)/"
	@cd web/build && $(PY) -m http.server $(PORT) --bind 0.0.0.0
PORT ?= 8537

# Render a page natively, as a check that the browser build is not the odd one
# out. ds10view writes PPMs; the wasm build produces the same pixels.
ds10view: out/ds10view
out/ds10view: src/app/ds10view.c src/app/ui.c src/app/obj.c src/app/cell.c \
              src/app/widget.c src/app/widget.h $(TBL_SRC) \
              src/gfx/nds2d.c src/gfx/nitro.c
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ src/app/ds10view.c src/app/ui.c src/app/obj.c \
	    src/app/widget.c $(TBL_SRC) \
	    src/app/cell.c src/gfx/nds2d.c src/gfx/nitro.c -lm

# The browser build against songplay, sample for sample. Needs node and the
# wasm build; both run the same engine, so this catches setup mistakes rather
# than DSP ones.
test-webedit: web out/songplay
	@node tools/webedit.js
	@node tools/webboot.js
	@node tools/webaudio.js

test-websong: web out/songplay
	@./out/songplay -b 3 -o out/ref_demo.wav \
	    data/nitrofs/demo/ds10demo01.dat >/dev/null
	@node tools/websong.js

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

# The cell expander: the function the whole interface draws through, against
# the original under Unicorn.
test-cell: out/test_cell tests/vectors/cell.bin
	@./out/test_cell

out/test_cell: tests/test_cell.c src/app/cell.c src/app/cell.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tests/test_cell.c src/app/cell.c

tests/vectors/cell.bin: extract tools/oracle.py
	@$(PY) tools/oracle.py cell-vectors

test-page: out/ds10view
	@$(PY) tools/pagecheck.py out/sweep out/sweep2 out/sweep3

# The captured sprite layer per page, until the widget constructors are read.
data/pages.bin: tools/pages.py
	@$(PY) tools/pages.py out/sweep out/sweep2 out/sweep3 $@

# Render a string with one of the ROM's fonts, as an eye check on the reader.
fonttest: out/fonttest
out/fonttest: tools/fonttest.c src/gfx/nftr.c src/gfx/nitro.c src/gfx/nftr.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ tools/fonttest.c src/gfx/nftr.c src/gfx/nitro.c
