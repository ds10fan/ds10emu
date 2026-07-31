/* fonttest - render a string with one of the ROM's NFTR fonts, as text.
 *
 *   out/fonttest data/nitrofs/../fonts.NFTR "SWING"
 *
 * A quick eye check on src/gfx/nftr.c: if the letters read, the glyph
 * packing, the width table and the character map are all being walked right.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gfx/nftr.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "out/nitrofs/data/fonts.NFTR";
    const char *text = argc > 2 ? argv[2] : "SWING 120";
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *blob;
    unsigned char *img;
    ds10_nftr ft;
    int w, h, x, y;

    if (!f) {
        perror(path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    if (ds10_nftr_parse(&ft, blob, (size_t)n) < 0) {
        fprintf(stderr, "%s: not an NFTR\n", path);
        return 2;
    }
    printf("%s: cell %dx%d, %d bpp, %d bytes each, %d glyphs, line feed %d\n",
           path, ft.cell_w, ft.cell_h, ft.bpp, ft.cell_size, ft.nglyphs,
           ft.line_feed);
    w = ds10_nftr_width(&ft, text) + ft.cell_w;
    h = ft.cell_h;
    if (w <= ft.cell_w) {
        printf("no glyph in this font maps to any of that text\n");
        return 1;
    }
    img = calloc((size_t)w * h, 1);
    ds10_nftr_text(&ft, text, img, w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) putchar(img[y * w + x] ? '#' : '.');
        putchar('\n');
    }
    return 0;
}
