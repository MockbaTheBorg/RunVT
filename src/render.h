// render.h - blits the cell grid + sixel overlay to an SDL2 window.
//
// SDL2 already abstracts window/pixel access across platforms, so this
// file has no #ifdef OS branches - that's the whole point of picking it.

#ifndef RUNVT_RENDER_H
#define RUNVT_RENDER_H

#include <SDL2/SDL.h>
#include <stdlib.h>
#include "screen.h"
#include "font.h"
#include "sixel.h"

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    unsigned char *fb; // RGBA scratch buffer, pw*ph*4
    int pw;
    int ph;
} Renderer;

// Standard 16-color ANSI palette (the usual xterm-ish values). VT100
// hardware itself was monochrome green/amber - this is for the SGR
// color support we added on top for real-world compatibility.
static const unsigned char render_palette[16][3] = {
    {0, 0, 0},       {170, 0, 0},     {0, 170, 0},     {170, 85, 0},
    {0, 0, 170},     {170, 0, 170},   {0, 170, 170},   {170, 170, 170},
    {85, 85, 85},    {255, 85, 85},   {85, 255, 85},   {255, 255, 85},
    {85, 85, 255},   {255, 85, 255},  {85, 255, 255},  {255, 255, 255}
};

static int render_init(Renderer *r, int cols, int total_rows, const char *title) {
    r->pw = cols * FONT_W;
    r->ph = total_rows * FONT_H;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;

    r->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, r->pw, r->ph, SDL_WINDOW_SHOWN);
    if (!r->win) return -1;

    r->ren = SDL_CreateRenderer(r->win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r->ren) r->ren = SDL_CreateRenderer(r->win, -1, 0);
    if (!r->ren) return -1;

    r->tex = SDL_CreateTexture(r->ren, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, r->pw, r->ph);
    if (!r->tex) return -1;

    r->fb = (unsigned char *)malloc((size_t)r->pw * (size_t)r->ph * 4);
    return 0;
}

// BEL used to just ring a bell on real hardware - closest sane
// equivalent on a modern desktop is flashing the window/taskbar entry.
static void render_flash(Renderer *r) {
    SDL_FlashWindow(r->win, SDL_FLASH_BRIEFLY);
}

static void render_destroy(Renderer *r) {
    free(r->fb);
    if (r->tex) SDL_DestroyTexture(r->tex);
    if (r->ren) SDL_DestroyRenderer(r->ren);
    if (r->win) SDL_DestroyWindow(r->win);
}

static void render_blit_cell(Renderer *r, int col, int row, const Cell *c, int invert) {
    const unsigned char *glyph = font_glyph(c->charset, c->ch);
    // Bold doesn't get its own glyph weight (single bitmap font), so we
    // fake it the way most terminal emulators do: bump the color to its
    // bright variant instead.
    const unsigned char *fgc = render_palette[(c->attr & ATTR_BOLD) ? (c->fg | 8) & 15 : c->fg & 15];
    const unsigned char *bgc = render_palette[c->bg & 15];
    int px0 = col * FONT_W;
    int py0 = row * FONT_H;

    if (invert) {
        const unsigned char *t = fgc; fgc = bgc; bgc = t;
    }

    for (int gy = 0; gy < FONT_H; gy++) {
        unsigned char rowbits = glyph[gy];
        int py = py0 + gy;
        unsigned char *dst = &r->fb[(py * r->pw + px0) * 4];
        int underline = (c->attr & ATTR_UNDERLINE) && gy == FONT_H - 1;

        for (int gx = 0; gx < FONT_W; gx++) {
            int on = underline || ((rowbits >> (7 - gx)) & 1);
            const unsigned char *rgb = on ? fgc : bgc;
            dst[gx * 4 + 0] = rgb[0];
            dst[gx * 4 + 1] = rgb[1];
            dst[gx * 4 + 2] = rgb[2];
            dst[gx * 4 + 3] = 255;
        }
    }
}

static void render_frame(Renderer *r, Screen *s, SixelLayer *sl) {
    for (int y = 0; y < s->total_rows; y++) {
        for (int x = 0; x < s->cols; x++) {
            // cursor never lands on the status row - it's not part of
            // the addressable screen
            int is_cursor = s->cursor_visible && x == s->cur_x && y == s->cur_y;
            render_blit_cell(r, x, y, scr_cell(s, x, y), is_cursor);
        }
    }

    if (sl != NULL) {
        int n = r->pw * r->ph;
        for (int i = 0; i < n; i++) {
            const unsigned char *sp = &sl->pixels[i * 4];
            if (sp[3] != 0) {
                unsigned char *dp = &r->fb[i * 4];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
            }
        }
    }

    SDL_UpdateTexture(r->tex, NULL, r->fb, r->pw * 4);
    SDL_RenderClear(r->ren);
    SDL_RenderCopy(r->ren, r->tex, NULL, NULL);
    SDL_RenderPresent(r->ren);
}

#endif // RUNVT_RENDER_H
