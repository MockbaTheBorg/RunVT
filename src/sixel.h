// sixel.h - minimal DEC Sixel graphics decoder.
//
// Decodes a sixel data string (the payload of a DCS q ... ST sequence)
// straight into an RGBA pixel overlay the same size as the terminal's
// pixel area. vtparser.h buffers the DCS payload and calls
// sixel_decode() once it sees the ST terminator.
//
// Scope is deliberately narrow: RGB color register definitions ('#'),
// repeat runs ('!'), carriage return / next line ('$' / '-'), and the
// sixel data bytes themselves. Raster attribute strings ('"') are
// parsed just enough to skip over them. HLS color mode is approximated
// via lightness only rather than fully converted - in practice almost
// every sixel emitter out there uses RGB mode anyway, so this was not
// worth the extra code.

#ifndef RUNVT_SIXEL_H
#define RUNVT_SIXEL_H

#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *pixels; // RGBA, pw*ph*4 bytes
    int pw;
    int ph;
} SixelLayer;

static void sixel_init(SixelLayer *sl, int pw, int ph) {
    sl->pw = pw;
    sl->ph = ph;
    sl->pixels = (unsigned char *)malloc((size_t)pw * (size_t)ph * 4);
    memset(sl->pixels, 0, (size_t)pw * (size_t)ph * 4);
}

static void sixel_free(SixelLayer *sl) {
    free(sl->pixels);
    sl->pixels = NULL;
}

// Shifts pixel rows [top_row, bottom_row] (in text-row units, inclusive)
// up by one text row's worth of pixels, same idea as scr_scroll_up but
// for the sixel overlay instead of the Cell grid. screen.h calls this
// alongside its own scroll so an image sitting in the scroll region
// moves with the text instead of staying pinned in place while
// everything scrolls past it.
static void sixel_scroll_up(SixelLayer *sl, int top_row, int bottom_row, int cell_h) {
    int rowbytes = sl->pw * 4;
    int py_top = top_row * cell_h;
    int py_bottom_excl = (bottom_row + 1) * cell_h;
    int total_px_rows = py_bottom_excl - py_top;

    if (cell_h <= 0 || py_bottom_excl > sl->ph) return;

    if (total_px_rows > cell_h) {
        memmove(&sl->pixels[py_top * rowbytes], &sl->pixels[(py_top + cell_h) * rowbytes],
                (size_t)(total_px_rows - cell_h) * (size_t)rowbytes);
    }
    memset(&sl->pixels[(py_bottom_excl - cell_h) * rowbytes], 0, (size_t)cell_h * (size_t)rowbytes);
}

// Same as above, opposite direction - for scroll-down / reverse index.
static void sixel_scroll_down(SixelLayer *sl, int top_row, int bottom_row, int cell_h) {
    int rowbytes = sl->pw * 4;
    int py_top = top_row * cell_h;
    int py_bottom_excl = (bottom_row + 1) * cell_h;
    int total_px_rows = py_bottom_excl - py_top;

    if (cell_h <= 0 || py_bottom_excl > sl->ph) return;

    if (total_px_rows > cell_h) {
        memmove(&sl->pixels[(py_top + cell_h) * rowbytes], &sl->pixels[py_top * rowbytes],
                (size_t)(total_px_rows - cell_h) * (size_t)rowbytes);
    }
    memset(&sl->pixels[py_top * rowbytes], 0, (size_t)cell_h * (size_t)rowbytes);
}

// Blanks the pixels under text columns [col0, col1] (inclusive) on one
// text row - the sixel-layer half of EL/ED/ECH. Without this, erasing
// a line or the screen only blanked the Cell grid; any sixel pixels
// sitting there stayed rendered on top of the now-empty text.
static void sixel_clear_region(SixelLayer *sl, int row, int col0, int col1,
                                int cell_w, int cell_h) {
    if (cell_w <= 0 || cell_h <= 0) return;

    int px0 = col0 * cell_w;
    int px1 = (col1 + 1) * cell_w;
    int py0 = row * cell_h;
    int py1 = (row + 1) * cell_h;
    if (px1 > sl->pw) px1 = sl->pw;
    if (py1 > sl->ph) py1 = sl->ph;
    if (px0 >= px1 || py0 >= py1) return;

    for (int y = py0; y < py1; y++) {
        memset(&sl->pixels[(y * sl->pw + px0) * 4], 0, (size_t)(px1 - px0) * 4);
    }
}

// Shifts pixels right within one text row's band, columns [col0, cols)
// - the sixel-layer half of ICH. Pixels pushed past the right edge of
// the row are gone; the vacated columns on the left are cleared.
static void sixel_shift_right(SixelLayer *sl, int row, int col0, int cols,
                               int shift_cols, int cell_w, int cell_h) {
    if (cell_w <= 0 || cell_h <= 0) return;

    int px0 = col0 * cell_w;
    int px_end = cols * cell_w;
    if (px_end > sl->pw) px_end = sl->pw;
    int shift_px = shift_cols * cell_w;
    int avail_px = px_end - px0;
    if (avail_px <= 0) return;

    int py0 = row * cell_h;
    int py1 = (row + 1) * cell_h;
    if (py1 > sl->ph) py1 = sl->ph;

    for (int y = py0; y < py1; y++) {
        unsigned char *rowpx = &sl->pixels[y * sl->pw * 4];
        if (shift_px < avail_px) {
            memmove(&rowpx[(px0 + shift_px) * 4], &rowpx[px0 * 4],
                    (size_t)(avail_px - shift_px) * 4);
            memset(&rowpx[px0 * 4], 0, (size_t)shift_px * 4);
        } else {
            memset(&rowpx[px0 * 4], 0, (size_t)avail_px * 4);
        }
    }
}

// Same idea, opposite direction - the sixel-layer half of DCH. Pixels
// shift left, vacated columns on the right are cleared.
static void sixel_shift_left(SixelLayer *sl, int row, int col0, int cols,
                              int shift_cols, int cell_w, int cell_h) {
    if (cell_w <= 0 || cell_h <= 0) return;

    int px0 = col0 * cell_w;
    int px_end = cols * cell_w;
    if (px_end > sl->pw) px_end = sl->pw;
    int shift_px = shift_cols * cell_w;
    int avail_px = px_end - px0;
    if (avail_px <= 0) return;

    int py0 = row * cell_h;
    int py1 = (row + 1) * cell_h;
    if (py1 > sl->ph) py1 = sl->ph;

    for (int y = py0; y < py1; y++) {
        unsigned char *rowpx = &sl->pixels[y * sl->pw * 4];
        if (shift_px < avail_px) {
            memmove(&rowpx[px0 * 4], &rowpx[(px0 + shift_px) * 4],
                    (size_t)(avail_px - shift_px) * 4);
            memset(&rowpx[(px_end - shift_px) * 4], 0, (size_t)shift_px * 4);
        } else {
            memset(&rowpx[px0 * 4], 0, (size_t)avail_px * 4);
        }
    }
}

// Default 16 sixel color registers. Real terminals differ slightly on
// these - it barely matters since any well-behaved sixel stream
// redefines the registers it actually uses with '#' anyway.
static const unsigned char sixel_default_palette[16][3] = {
    {0, 0, 0}, {51, 51, 204}, {204, 33, 33}, {51, 204, 51},
    {204, 51, 204}, {51, 204, 204}, {204, 204, 51}, {204, 204, 204},
    {102, 102, 102}, {102, 102, 255}, {255, 102, 102}, {102, 255, 102},
    {255, 102, 255}, {102, 255, 255}, {255, 255, 102}, {255, 255, 255}
};

static void sixel_paint(SixelLayer *sl, int x, int y, const unsigned char rgb[3]) {
    if (x < 0 || y < 0 || x >= sl->pw || y >= sl->ph) return;
    unsigned char *p = &sl->pixels[(y * sl->pw + x) * 4];
    p[0] = rgb[0];
    p[1] = rgb[1];
    p[2] = rgb[2];
    p[3] = 255;
}

// Parses a decimal number starting at data[*i], advances *i past it.
// Returns 0 (and leaves *i alone) if there were no digits to read.
static int sixel_parse_int(const unsigned char *data, int len, int *i, int *out) {
    int start = *i;
    int val = 0;
    int any = 0;
    while (*i < len && data[*i] >= '0' && data[*i] <= '9') {
        val = val * 10 + (data[*i] - '0');
        (*i)++;
        any = 1;
    }
    if (!any) {
        *i = start;
        return 0;
    }
    *out = val;
    return 1;
}

// Decodes sixel data, painting it into sl starting at pixel
// (origin_x, origin_y) - that's wherever the cursor was when the DCS
// sequence arrived, so the image lands right where the app put it.
static void sixel_decode(SixelLayer *sl, int origin_x, int origin_y,
                          const unsigned char *data, int len) {
    unsigned char reg[256][3];
    int reg_defined[256];
    int x = 0, y = 0;
    int cur_reg = 0;
    int repeat = 1;

    memset(reg_defined, 0, sizeof(reg_defined));
    for (int i = 0; i < 16; i++) {
        reg[i][0] = sixel_default_palette[i][0];
        reg[i][1] = sixel_default_palette[i][1];
        reg[i][2] = sixel_default_palette[i][2];
        reg_defined[i] = 1;
    }

    int i = 0;
    while (i < len) {
        unsigned char b = data[i];

        if (b >= 0x3F && b <= 0x7E) {
            // one sixel char = a 6-pixel-tall vertical strip, bit 0 on top
            int value = b - 0x3F;
            for (int row = 0; row < 6; row++) {
                if (value & (1 << row)) {
                    for (int r = 0; r < repeat; r++) {
                        sixel_paint(sl, origin_x + x + r, origin_y + y + row,
                                    reg[cur_reg]);
                    }
                }
            }
            x += repeat;
            repeat = 1;
            i++;
        } else if (b == '!') {
            int n = 1;
            i++;
            sixel_parse_int(data, len, &i, &n);
            repeat = n > 0 ? n : 1;
        } else if (b == '$') {
            x = 0;
            i++;
        } else if (b == '-') {
            x = 0;
            y += 6;
            i++;
        } else if (b == '"') {
            // raster attrs: Pan;Pad;Ph;Pv - we don't need any of this,
            // just have to eat it so it doesn't get mistaken for data
            i++;
            for (int k = 0; k < 4; k++) {
                int n;
                sixel_parse_int(data, len, &i, &n);
                if (i < len && data[i] == ';') i++;
                else break;
            }
        } else if (b == '#') {
            int params[5];
            int nparams = 0;
            i++;
            while (nparams < 5) {
                int n = 0;
                if (!sixel_parse_int(data, len, &i, &n)) break;
                params[nparams++] = n;
                if (i < len && data[i] == ';') { i++; continue; }
                break;
            }
            if (nparams >= 1) {
                cur_reg = params[0] & 0xFF;
                // Pu=1 is HLS, Pu=2 is RGB - per the actual DEC sixel
                // spec, not the order you'd guess. Got this backwards
                // on the first pass; fixed once real test content
                // exposed it.
                if (nparams >= 5 && params[1] == 2) {
                    // RGB mode, each component given as a 0-100 percentage
                    reg[cur_reg][0] = (unsigned char)(params[2] * 255 / 100);
                    reg[cur_reg][1] = (unsigned char)(params[3] * 255 / 100);
                    reg[cur_reg][2] = (unsigned char)(params[4] * 255 / 100);
                    reg_defined[cur_reg] = 1;
                } else if (nparams >= 5 && params[1] == 1) {
                    // HLS mode - not doing a real conversion, lightness
                    // only gets us a reasonable gray and that's enough
                    unsigned char l = (unsigned char)(params[3] * 255 / 100);
                    reg[cur_reg][0] = l;
                    reg[cur_reg][1] = l;
                    reg[cur_reg][2] = l;
                    reg_defined[cur_reg] = 1;
                } else if (!reg_defined[cur_reg]) {
                    reg[cur_reg][0] = reg[cur_reg][1] = reg[cur_reg][2] = 128;
                }
            }
        } else {
            i++;
        }
    }
}

#endif // RUNVT_SIXEL_H
