// font.h - embedded 8x16 bitmap fonts and glyph lookup.
//
// The actual glyph data (font_latin1.h, font_cp850.h,
// font_special_graphics.h) isn't hand-typed - tools/gen_fonts.py pulls
// it out of GNU Unifont plus glibc's IBM850 charmap. Rerun that script
// if these tables ever need regenerating, don't hand-edit them.

#ifndef RUNVT_FONT_H
#define RUNVT_FONT_H

#include "screen.h"
#include "font_latin1.h"
#include "font_cp850.h"
#include "font_special_graphics.h"

#define FONT_W 8
#define FONT_H 16

typedef enum { CODEPAGE_LATIN1 = 0, CODEPAGE_CP850 = 1 } CodePage;

static CodePage g_active_codepage = CODEPAGE_LATIN1;

// Returns a 16-byte glyph bitmap (1 byte per row, MSB = leftmost pixel)
// for a cell's charset tag + byte value, honoring whichever upper-128
// codepage is active. Falls back to the normal font for a few DEC
// Special Graphics symbols Unifont just doesn't have a narrow glyph for
// (some obscure control-picture ones - box drawing itself is fine).
static const unsigned char *font_glyph(unsigned char charset, unsigned char ch) {
    if (charset == CHARSET_SPECIAL_GRAPHICS && ch >= 0x60 && ch <= 0x7E) {
        const unsigned char *g = font_special_graphics[ch];
        int all_blank = 1;
        for (int i = 0; i < 16; i++) {
            if (g[i] != 0) { all_blank = 0; break; }
        }
        if (!all_blank) return g;
    }
    return (g_active_codepage == CODEPAGE_CP850) ? font_cp850[ch] : font_latin1[ch];
}

#endif // RUNVT_FONT_H
