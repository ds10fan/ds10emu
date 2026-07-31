/* The browser build's entry points.
 *
 * Nothing here is DS-10; it is the thin edge between the reimplementation and
 * JavaScript. The C side owns two 256x192 framebuffers and hands their
 * addresses over; the page copies them into canvases.
 *
 * Assets come from the Emscripten virtual filesystem, preloaded at build time
 * from data/nitrofs, so the same ds10_ui code runs here and natively.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "../app/ui.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

static ds10_ui *ui;
static uint32_t fb_top[FB], fb_bottom[FB];
static char last_error[256];

/* Canvas ImageData wants RGBA bytes in memory order; the renderer produces
 * 0xAARRGGBB words. On a little-endian target that is B,G,R,A, so the red and
 * blue lanes swap on the way out. */
static void to_rgba(uint32_t *p)
{
    int i;

    for (i = 0; i < FB; i++) {
        uint32_t c = p[i];

        p[i] = (c & 0xFF00FF00u) | ((c & 0xFF0000u) >> 16) |
               ((c & 0xFFu) << 16);
    }
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_init(void)
{
    if (ui) return 0;
    ui = ds10_ui_new("/data/nitrofs");
    if (!ui) {
        snprintf(last_error, sizeof last_error,
                 "could not load assets from /data/nitrofs");
        return -1;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int ds10_web_page(const char *bottom, const char *top)
{
    if (!ui && ds10_web_init() < 0) return -1;
    if (ds10_ui_page(ui, (bottom && *bottom) ? bottom : NULL,
                     (top && *top) ? top : NULL) < 0) {
        snprintf(last_error, sizeof last_error, "%s", ds10_ui_error(ui));
        return -1;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void ds10_web_render(void)
{
    if (!ui) return;
    ds10_ui_frame(ui, fb_top, fb_bottom);
    to_rgba(fb_top);
    to_rgba(fb_bottom);
}

EMSCRIPTEN_KEEPALIVE
uint32_t *ds10_web_top(void) { return fb_top; }

EMSCRIPTEN_KEEPALIVE
uint32_t *ds10_web_bottom(void) { return fb_bottom; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_width(void) { return DS10_SCR_W; }

EMSCRIPTEN_KEEPALIVE
int ds10_web_height(void) { return DS10_SCR_H; }

EMSCRIPTEN_KEEPALIVE
const char *ds10_web_error(void) { return last_error; }

int main(void)
{
    /* Nothing to do at load time: the page drives everything. */
    return 0;
}
