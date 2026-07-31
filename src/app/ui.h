/* The DS-10 interface, on top of the 2D hardware model.
 *
 * A page is one screen of the interface: a tile image and a tilemap out of
 * /data/mainbg, drawn as one background layer, over the shared S_bg backdrop.
 * Which layer, which palette and which VRAM addresses are not choices made
 * here - they are what the game does, read out of the VRAM and registers that
 * ndsref captured, and checked against them by `make test-page`.
 */
#ifndef DS10_APP_UI_H
#define DS10_APP_UI_H

#include <stdint.h>

#include "../gfx/nds2d.h"

typedef struct ds10_ui ds10_ui;

/* `root` is the directory holding mainbg/, mainobj/, data0/ - normally
 * "data/nitrofs". Returns NULL if the shared assets are missing. */
ds10_ui *ds10_ui_new(const char *root);
void ds10_ui_free(ds10_ui *u);

/* Choose what each screen shows, by asset name ("61_SYN_EDIT", "90_MAP").
 * Either may be NULL to leave that screen as it was. Returns 0 on success. */
int ds10_ui_page(ds10_ui *u, const char *bottom, const char *top);

/* Render one frame. Both buffers are 256x192 of 0xAARRGGBB. */
void ds10_ui_frame(ds10_ui *u, uint32_t *top, uint32_t *bottom);

/* The GPU state, for tests that compare it against a captured frame. */
ds10_gpu *ds10_ui_gpu(ds10_ui *u);

/* The last error, for the caller to report. */
const char *ds10_ui_error(const ds10_ui *u);

#endif /* DS10_APP_UI_H */
