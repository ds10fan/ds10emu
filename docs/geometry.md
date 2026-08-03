# The interface's geometry, as read from the ROM

`src/app/ui.c` was deleted when the interface was restarted as a decompilation
(see `UIPLAN.md`). Its *structure* was wrong - real widgets carry their own
rectangles, and a table per page is not how the original works - but by the end
its *numbers* were read out of the ROM's constructors and tables rather than
measured. This file is those numbers, so the rewrite does not have to read six
constructors again.

Provenance, and it varies:

- **read from a table the constructor walks**: 90_MAP's boxes (0x02087018 and
  0x02086f08), 52_FX's buttons (0x020869ac) and knobs (0x02086874),
  54_DRUM_FX's (0x02086808), 70_PATCH's knobs (0x02085918),
  33_DRUM_SELECTER's panels (0x02086300), 30_MIXER's knobs and faders
  (0x02087610, 0x020875b0), 32_PATTERN's bar (0x020845dc), 81_KEY's bar
  (0x0204bff8's three calls).
- **read from a constructor's immediates**: 32_PATTERN's button grid and mute
  and solo strip (0x0202e01c), 71_KAOSS's bar (0x02049150), 62_DRUM_SEQ's row
  buttons and both sequencers' scrollbar (0x020594d4, 0x02050108), 30_MIXER's
  strip (0x02065ed8), the shared top bar (around 0x02053900), and every
  statically-sized widget in the ROM by scanning the three placement functions.
- **still measured, and owed**: the keyboard's key rectangles on 81_KEY (which
  F the lowest key is remains an assumption), the piano roll's cursor row, and
  which page fills the shared bar's x 131 slot with which button - the ROM
  gives the set and the slots, the captions on the rendered pages gave the
  assignment.

## Constants

```
  PTR_TILE 0x21A
  PTR_PAL 3
  PTR_PRIO 2
  PTR_W 8
  PTR_H 16
  PTR_SWEEP 270   /* the sweep every screen passes 0x02055b98 */
  PTR_ORBIT_X 3.0
  PTR_ORBIT_Y (-7.0)
  PTR_PI 3.14159265358979323846
  MIX_PAN_X 21
  MIX_PAN_Y 47
  MIX_PAN_R 14
  MIX_VOL_X 10
  MIX_VOL_Y 85
  MIX_VOL_W 32
  MIX_VOL_H 0x61        /* the whole widget, which is what it catches */
  MIX_VOL_TRAVEL 0x51
  MIX_STEP 43
  KEY_TOP 28
  KEY_BOTTOM 170
  BLACK_BOTTOM 116
  WHITE_X0 3
  WHITE_W 18
  KEY_BASE 53 /* F3 */
  FX_KNOB_Y 161
  FX_KNOB_R 20
  SEQ_CELL 13
  SEQ_X0 45
  SEQ_Y0 35
  SEQ_COLS 16
  SEQ_ROWS 12
  SEQ_RANGE 24    /* the scrollable height, in rows: two octaves */
  SEQ_TOP_DEFAULT 69      /* A4 on the top row, as the capture had it */
  SEQ_CURSOR_CELL 4
  SEQ_CURSOR_Y 20
  KAOSS_X 0
  KAOSS_Y 20
  KAOSS_W 256
  KAOSS_H 172
  MIX_FADER_CELL 15
  MIX_FADER_ORIGIN 85
  DSEQ_ROWS 4
  PAT_X0 6
  PAT_Y0 24
  PAT_W 55
  PAT_H 27
  PAT_XSTEP 63
  PAT_YSTEP 33
  PAT_COLS 4
  PAT_ROWS 4
  PAT_MARK_CELL 20
  PAT_MARK_DX 0
  PAT_MARK_DY 0
  PAT_MARK_BASE 0x200   /* CHARACTER_c's tile base, as everywhere */
  MAPBTN_X 222
  MAPBTN_Y 2
  MAPBTN_W 34
  MAPBTN_H 16
  BAR_Y0 2
  BAR_Y1 18
  BAR_W 26
  BAR_PLAY_X 193
  BAR_SWAP_X 224
  SCROLL_X0 3
  SCROLL_X1 20
  SCROLL_Y0 31              /* 0x02050108's +0x41C, in 20.12 */
  SCROLL_H 11
  SCROLL_SPAN 144           /* the 0x90 it passes the slider setter */
  SCROLL_MAX 84             /* and the 0x54; the value counts upward */
  SCROLL_POSITIONS (SEQ_RANGE - SEQ_ROWS)
  SCROLL_TRAVEL SCROLL_SPAN   /* the handle's top travels the span */
  SCROLL_BOTTOM (SCROLL_Y0 + SCROLL_SPAN + SCROLL_H)
  SCROLL_DEFAULT 5
  HANDLE_W 12
  HANDLE_X 5
  SEQ_KEY_X0 22
  SEQ_KEY_X1 39
  FX_DRUM_DX 64
  FX_DRUM_TYPES 3
  KAOSS_COLOR 0x1F2F3F1Fu   /* r 0x1F, g 0x3F, b 0x2F - see LED_COLOR */
  SEQ_MODE_X 42
  SEQ_MODE_Y 2
  SEQ_MODE_W 17
  SEQ_MODE_H 14
  SEQ_MARK_CELL 7
  SEQ_MARK_BASE 0x200
```

## Tables

```c
MAP_BOX[] = {
    {  7,  25, 46, 16,  0, "60_SYN_SEQ_PIANO"},  /* SYN1 SEQ */
    {  7,  87, 46, 16,  1, "60_SYN_SEQ_PIANO"},  /* SYN2 SEQ */
    {  7, 149, 54, 16, 12, "51_ALL_DRUMS"},      /* DRUMS SEQ */
    { 39,  45, 54, 16,  8, "81_KEY_i"},          /* SYN1 KBD */
    { 39,  65, 54, 16,  6, "71_KAOSS"},          /* SYN1 KAOSS */
    { 39, 107, 54, 16,  9, "81_KEY_i"},          /* SYN2 KBD */
    { 39, 127, 54, 16,  7, "71_KAOSS"},          /* SYN2 KAOSS */
    {117,  25, 46, 16,  2, "61_SYN_EDIT"},       /* SYN1 EDIT */
    {117,  87, 46, 16,  3, "61_SYN_EDIT"},       /* SYN2 EDIT */
    {131,  54, 53, 16,  4, "70_PATCH"},          /* SYN1 PATCH */
    {131, 116, 53, 16,  5, "70_PATCH"},          /* SYN2 PATCH */
    {203,  25, 36, 16, 11, "30_MIXER"},
    {207,  55, 30, 16, 10, "52_FX"},
    {  4,   2, 76, 14, 13, "32_PATTERN"},
    {206, 142, 46, 16, 18, "33_DRUM_SELECTER"},  /* DRUMS */
    /* the DRUMS submenu, which is the same table with four more in front */
    { 85,  64, 88, 16, 17, "53_DRUMEDIT"},
    { 85,  96, 88, 16, 19, "63_DRUM_EDIT_PATCH"},
    { 85, 128, 88, 16, 16, "54_DRUM_FX"},
};

WHITE_SEMI[14] = {
    0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23,
};

BLACK_KEY[10] = {
    { 14,  24,  1}, { 33,  43,  3}, { 52,  62,  5}, { 86,  96,  8},
    {106, 116, 10}, {140, 150, 13}, {159, 169, 15}, {178, 188, 17},
    {212, 222, 20}, {232, 242, 22},
};

KEY_BUTTON[] = {
    {0x04, 2, 0x1C, 0x0E, 0},
    {0x23, 2, 0x1C, 0x0E, 1},
    {0x42, 4, 0x46, 0x0C, 2},
};

FX_PARAM[3][3] = {
    {2, 3, 7},   /* delay */
    {4, 5, -1},  /* flanger */
    {4, 5, 7},   /* chorus */
};

FX_KNOB_X[4] = {64, 118, 172, 226};

PATCH_KNOB[] = {
    { 23, 93, 24}, { 66, 93,  3}, {109, 93,  4},
    {152, 93,  5}, {195, 93, 15}, {238, 93, 17},
};

DRUM_PANEL[4] = {
    {  7,  25, 117, 76}, {132,  25, 117, 76},
    {  7, 110, 117, 76}, {132, 110, 117, 76},
};

SEQ_MODE_PAGE[] = {
    "60_SYN_SEQ_PIANO", "60_SYN_SEQ_GATE", "60_SYN_SEQ_VOL",
    "60_SYN_SEQ_PAN", "60_SYN_SEQ_KAOSS", "60_SYN_SEQ_KAOSS",
};

SEQ_MODE[] = {
    {SEQ_ROWS}, {5}, {12}, {5}, {12}, {12},
};

BAR_BUTTON[] = {
    /* the sequencer, both variants of it */
    {"60_SYN_SEQ_",  42, 13, DS10_BAR_MODE},
    {"60_SYN_SEQ_", 131, 28, DS10_BAR_CLR},
    {"60_SYN_SEQ_", 162, 28, DS10_BAR_SOLO},
    {"62_DRUM_SEQ",  42, 13, DS10_BAR_MODE},
    {"62_DRUM_SEQ", 131, 28, DS10_BAR_CLR},
    {"62_DRUM_SEQ", 162, 28, DS10_BAR_SOLO},
    /* the pad. Its constructor, 0x02049150, places six of these and names
     * them, so these are read rather than guessed from the icon: three
     * captioned %d at x = 4 + 21*n, 18 by 14, then `setting` at 67 captioned
     * SET, `playmode` at 98 whose caption the page rewrites - the ROM has
     * OFF/TRIG/SMOOTH and OFF/HLD/SMT as two spellings of it - and `rec` at
     * 162, all 28 by 14. The one at 131 is not in this constructor; the
     * editor screens share a prologue and it comes from there. */
    {"71_KAOSS",      4, 18, DS10_BAR_SLOT1},
    {"71_KAOSS",     25, 18, DS10_BAR_SLOT2},
    {"71_KAOSS",     46, 18, DS10_BAR_SLOT3},
    {"71_KAOSS",     67, 28, DS10_BAR_SET},
    {"71_KAOSS",     98, 28, DS10_BAR_RATE},
    {"71_KAOSS",    131, 28, DS10_BAR_SOLO},
    {"71_KAOSS",    162, 28, DS10_BAR_REC},
    /* the keyboard, from its constructor 0x0204bff8: octDown and octUp 28 by
     * 14, `synth` 70 by 12 at y 4, and `rec` 32 by 16 - the same name the pad
     * gives the button in that slot. As on the pad, the one at 131 is not in
     * the constructor and comes from the shared editor prologue. */
    {"81_KEY",        4, 28, DS10_BAR_OCT_DOWN},
    {"81_KEY",       35, 28, DS10_BAR_OCT_UP},
    {"81_KEY",       66, 70, DS10_BAR_SYNTH},
    {"81_KEY",      131, 28, DS10_BAR_SOLO},
    {"81_KEY",      162, 32, DS10_BAR_REC},
    /* The two synth editors. The wide button on the left of both was
     * unidentified; the size scan finds a `synth` widget placed at 70 by 12
     * on each (0x02047770 and 0x0204e568), the same name and size 81_KEY's
     * selector has. Its x is computed rather than immediate, so the 4 is the
     * drawn frame's origin - 81_KEY's draws 60 wide from a 70-wide box, and
     * these draw 60 wide from x 4. */
    {"61_SYN_EDIT",   4, 70, DS10_BAR_SYNTH},
    {"61_SYN_EDIT", 162, 28, DS10_BAR_SOLO},
    {"70_PATCH",      4, 70, DS10_BAR_SYNTH},
    {"70_PATCH",    162, 28, DS10_BAR_SOLO},
    /* the drums */
    {"51_ALL_DRUMS",    162, 28, DS10_BAR_CLR},
    {"33_DRUM_SELECTER",  4, 27, DS10_BAR_EDIT},
    {"33_DRUM_SELECTER", 162, 28, DS10_BAR_REC},
    /* the pattern picker, and the song */
    /* COPY and LOCK are in 32_PATTERN's constructor table at 0x020845dc,
     * 28 by 14 each. */
    {"32_PATTERN",    4, 28, DS10_BAR_COPY},
    {"32_PATTERN",   35, 28, DS10_BAR_LOCK},
    {"31_SONG",     162, 28, DS10_BAR_CLR},
};

FX_BUTTON[] = {
    { 11,  47, 66, 14, 0, 0},   /* DELAY */
    { 51,  78, 66, 14, 0, 1},   /* FLANGER */
    { 11, 109, 66, 14, 0, 2},   /* CHORUS */
    {139,  38, 66, 14, 1, 0},   /* SYN 1 */
    {179,  54, 66, 14, 1, 1},   /* SYN 2 */
    {139,  70, 66, 14, 1, 2},   /* SYN 1 + 2 */
    {179,  86, 66, 14, 1, 3},   /* DRUMS */
    {139, 102, 66, 14, 1, 4},   /* ALL */
    {179, 118, 66, 14, 1, 5},   /* OFF */
};

MS_STRIP[] = {
    /* The mixer's strip is placed in a loop at 0x02065ed8, x = n*43 + 3 with
     * the size as immediates - 40 by 10, the same as 32_PATTERN's and the
     * same widget name, tm%d. Measured off the frames it looked 34 by 9,
     * which is the drawn box rather than the hit box. */
    {"30_MIXER",       3,  62, 40, 10, 43, 6, 0, 11},
    /* 32_PATTERN's is its constructor's, not a measurement: twelve widgets
     * per row named tm%d and ts%d at x = 3 + 43*n, y 160 and 171, 40 by 10.
     * Twelve because the PLUS has a second deck; six are on screen. Which is
     * also why the mute and solo pass at 0x0206724c splits on bit 4 of the
     * widget index - the two rows are numbered n and n + 16. */
    {"32_PATTERN",     3, 160, 40, 10, 43, 6, 0, 11},
    /* From the drum sequencer's constructor 0x020594d4, which places the four
     * pairs in a loop: x 3 and x 22, y = 33 + 13*row, each 19 by 13. The
     * 13 is the grid's own row pitch, and the rows sit two pixels above the
     * grid cells they belong to. */
    {"51_ALL_DRUMS",   3,  33, 19, 13, 0, 4, 19, 0},
};

SYN_KNOB[] = {
    { 27,  46,  0}, { 27,  87,  2}, { 27, 128,  1}, { 27, 169, 19},
    { 80,  46,  6}, { 80,  87,  9}, { 80, 169, 18},
    {127,  46,  7}, {127,  87,  8},
    {179,  46, 11}, {179,  87, 12}, {179, 128, 14},
    {230,  46, 20}, {230,  87, 21}, {230, 128, 22}, {230, 169, 23},
};
```
