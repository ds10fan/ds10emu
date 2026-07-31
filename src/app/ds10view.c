/* ds10view - render a page of the interface to a PPM, with nothing but the
 * reimplementation.
 *
 *     out/ds10view 61_SYN_EDIT 90_MAP out/page
 *
 * writes out/page_bot.ppm and out/page_top.ppm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define FB (DS10_SCR_W * DS10_SCR_H)

static uint32_t top[FB], bottom[FB];

static int write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", DS10_SCR_W, DS10_SCR_H);
    for (i = 0; i < FB; i++) {
        fputc((fb[i] >> 16) & 0xFF, f);
        fputc((fb[i] >> 8) & 0xFF, f);
        fputc(fb[i] & 0xFF, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *bot = argc > 1 ? argv[1] : "61_SYN_EDIT";
    const char *tp = argc > 2 ? argv[2] : "90_MAP";
    const char *stem = argc > 3 ? argv[3] : "out/page";
    const char *root = getenv("DS10_ASSETS");
    char path[512];
    ds10_ui *u;

    u = ds10_ui_new(root ? root : "data/nitrofs");
    if (!u) return 2;
    if (ds10_ui_page(u, bot, tp) < 0) {
        fprintf(stderr, "%s\n", ds10_ui_error(u));
        return 2;
    }
    ds10_ui_frame(u, top, bottom);

    snprintf(path, sizeof path, "%s_bot.ppm", stem);
    if (write_ppm(path, bottom) < 0) return 2;
    snprintf(path, sizeof path, "%s_top.ppm", stem);
    if (write_ppm(path, top) < 0) return 2;
    printf("%s: %s (bottom), %s (top)\n", stem, bot, tp);
    ds10_ui_free(u);
    return 0;
}
