// runvt_core.h - the reusable heart of RunVT: SDL window, screen grid,
// sixel overlay, VT parser, and the keyboard/render pump. Everything in
// main.c that isn't "how do we talk to a child process" lives here, so
// an embedding host (RunCPM linked directly against RunVT, no PTY of
// any kind) can drive the same terminal core standalone RunVT does -
// just swapping out how bytes get in and out.
//
// Standalone usage (see main.c): rt_core_init(), then per frame:
// rt_core_feed() with bytes read from the child, rt_core_pump(),
// rt_core_maybe_render().
//
// Embedded usage: same shape, minus a child process - the "input_cb"
// in RunVTConfig is where keystrokes and DSR/DA query replies go
// instead of a pty write, and rt_core_feed() is called directly by
// whatever's generating output (e.g. RunCPM's console primitives)
// instead of being read off a pipe.

#ifndef RUNVT_CORE_H
#define RUNVT_CORE_H

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "version.h"
#include "screen.h"
#include "font.h"
#include "cp850_encode.h"
#include "sixel.h"
#include "vtparser.h"
#include "render.h"

// Bytes RunVT needs to hand back to whatever it's a terminal for -
// keystrokes, clipboard paste, DSR/DA query replies. Same signature as
// vtparser.h's own reply callback (and RT_Process's rt_write) so it can
// be passed straight through with no adapter in the standalone case.
typedef int (*rt_input_cb)(void *ctx, const unsigned char *buf, int len);

typedef struct {
    int cols, rows;
    double zoom;
    const char *title;
    int codepage;          // CODEPAGE_LATIN1 or CODEPAGE_CP850, see cp850_encode.h
    rt_input_cb input_cb;
    void *input_ctx;
} RunVTConfig;

typedef struct RunVT {
    Screen scr;
    SixelLayer sixel;
    VTParser vp;
    Renderer ren;

    rt_input_cb input_cb;
    void *input_ctx;
    int input_enabled;

    char status_left[264];
    int last_caps;

    // No cross-platform "mouse released" event exists for a window
    // resize, so we fake it: wait for a gap in resize events instead.
    int resize_pending;
    Uint32 last_resize_tick;
} RunVT;

#define RUNVT_RESIZE_SETTLE_MS 150

static void rt_core_set_title(void *ctx, const char *title) {
    SDL_SetWindowTitle(((Renderer *)ctx)->win, title);
}

// Minimal UTF-8 decoder for SDL_TEXTINPUT text. SDL hands us composed
// keyboard input as UTF-8 regardless of what codepage we're emulating,
// so this has to happen before we can encode down to Latin-1/CP850.
static int rt_core_utf8_decode(const char *s, unsigned int *out_cp) {
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { *out_cp = c0; return 1; }
    if ((c0 & 0xE0) == 0xC0 && s[1]) {
        *out_cp = ((unsigned int)(c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
        *out_cp = ((unsigned int)(c0 & 0x0F) << 12)
                | ((unsigned int)((unsigned char)s[1] & 0x3F) << 6)
                | ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *out_cp = ((unsigned int)(c0 & 0x07) << 18)
                | ((unsigned int)((unsigned char)s[1] & 0x3F) << 12)
                | ((unsigned int)((unsigned char)s[2] & 0x3F) << 6)
                | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *out_cp = '?';
    return 1;
}

static unsigned char rt_core_encode_char(unsigned int cp) {
    if (cp < 0x80) return (unsigned char)cp;
    if (g_active_codepage == CODEPAGE_CP850) return cp850_encode(cp);
    return cp < 0x100 ? (unsigned char)cp : '?';
}

static void rt_core_send(RunVT *vt, const unsigned char *buf, int len) {
    if (vt->input_cb) vt->input_cb(vt->input_ctx, buf, len);
}

static void rt_core_send_str(RunVT *vt, const char *s) {
    rt_core_send(vt, (const unsigned char *)s, (int)strlen(s));
}

static void rt_core_handle_textinput(RunVT *vt, const char *text) {
    const char *p = text;
    while (*p) {
        unsigned int cp;
        int n = rt_core_utf8_decode(p, &cp);
        unsigned char b = rt_core_encode_char(cp);
        rt_core_send(vt, &b, 1);
        p += n;
    }
}

// Only the keys that don't show up as SDL_TEXTINPUT: control keys,
// arrows, function keys, ctrl-combos. Printable characters (including
// accented ones) go through rt_core_handle_textinput() instead - doing
// both here would double-send every normal keystroke.
static void rt_core_handle_keydown(RunVT *vt, SDL_Keysym *ks) {
    SDL_Keycode sym = ks->sym;
    Uint16 mod = ks->mod;

    // Ctrl+Shift+V pastes - checked ahead of the plain ctrl-letter case
    // below, or this would just send a literal Ctrl+V (0x16) instead.
    if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) && sym == SDLK_v) {
        if (SDL_HasClipboardText()) {
            char *text = SDL_GetClipboardText();
            rt_core_handle_textinput(vt, text);
            SDL_free(text);
        }
        return;
    }

    if ((mod & KMOD_CTRL) && sym >= SDLK_a && sym <= SDLK_z) {
        unsigned char b = (unsigned char)(sym - SDLK_a + 1);
        rt_core_send(vt, &b, 1);
        return;
    }

    // Shift+Backspace sends DEL, for keyboards where reaching Delete is awkward.
    if ((mod & KMOD_SHIFT) && sym == SDLK_BACKSPACE) {
        rt_core_send_str(vt, "\x7F");
        return;
    }

    switch (sym) {
        // CP/M-era apps (WordStar and friends) expect BS here, not DEL.
        // SDL gives the numpad Enter its own keycode, separate from
        // the main one - both should just mean "Enter" to the app.
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  rt_core_send_str(vt, "\r"); break;
        case SDLK_BACKSPACE: rt_core_send_str(vt, "\x08"); break;
        case SDLK_TAB:       rt_core_send_str(vt, "\x09"); break;
        case SDLK_ESCAPE:    rt_core_send_str(vt, "\x1B"); break;
        case SDLK_DELETE:    rt_core_send_str(vt, "\x7F"); break;
        case SDLK_UP:        rt_core_send_str(vt, "\x1B[A"); break;
        case SDLK_DOWN:      rt_core_send_str(vt, "\x1B[B"); break;
        case SDLK_RIGHT:     rt_core_send_str(vt, "\x1B[C"); break;
        case SDLK_LEFT:      rt_core_send_str(vt, "\x1B[D"); break;
        case SDLK_HOME:      rt_core_send_str(vt, "\x1B[H"); break;
        case SDLK_END:       rt_core_send_str(vt, "\x1B[F"); break;
        // real VT100 PF1-PF4 codes, in case anything still cares
        case SDLK_F1:        rt_core_send_str(vt, "\x1BOP"); break;
        case SDLK_F2:        rt_core_send_str(vt, "\x1BOQ"); break;
        case SDLK_F3:        rt_core_send_str(vt, "\x1BOR"); break;
        case SDLK_F4:        rt_core_send_str(vt, "\x1BOS"); break;
        default: break;
    }
}

static int rt_core_init(RunVT *vt, const RunVTConfig *cfg) {
    memset(vt, 0, sizeof(*vt));

    vt->input_cb = cfg->input_cb;
    vt->input_ctx = cfg->input_ctx;
    vt->input_enabled = 1;
    vt->last_caps = -1;
    g_active_codepage = cfg->codepage;

    // The status bar is always present, on top of whatever size the
    // caller asks for - not an optional extra row. The app itself only
    // ever gets `rows` addressable rows; the bar is RunVT's own extra
    // row underneath, same idea as a VT220's status line row.
    scr_init(&vt->scr, cfg->cols, cfg->rows, 1);
    scr_set_cell_px(&vt->scr, FONT_W, FONT_H);
    sixel_init(&vt->sixel, cfg->cols * FONT_W, vt->scr.total_rows * FONT_H);
    sixel_set_paint_limit(&vt->sixel, cfg->rows * FONT_H);

    vt_init(&vt->vp, FONT_W, FONT_H, cfg->input_cb, cfg->input_ctx, rt_core_set_title, &vt->ren);

    if (render_init(&vt->ren, cfg->cols, vt->scr.total_rows, cfg->title, cfg->zoom) != 0) {
        return -1;
    }

    snprintf(vt->status_left, sizeof(vt->status_left), " %s", cfg->title);
    scr_set_status(&vt->scr, vt->status_left, "");

    SDL_StartTextInput();
    return 0;
}

static void rt_core_feed(RunVT *vt, const unsigned char *buf, int len) {
    vt_process(&vt->scr, &vt->vp, &vt->sixel, buf, len);
}

static void rt_core_set_input_enabled(RunVT *vt, int enabled) {
    vt->input_enabled = enabled;
}

// Pumps pending SDL events (quit, resize, keyboard) and settles any
// in-progress window resize. Returns 1 if the window should close -
// either a real SDL_QUIT, or (once the caller has disabled input via
// rt_core_set_input_enabled(), e.g. because its child process died) any
// keypress, matching the "press any key to close" convention rather
// than trying to forward keystrokes nowhere.
static int rt_core_pump(RunVT *vt) {
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            return 1;
        }
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
            // Let it sit wherever the drag put it - linear filter so a
            // non-integer scale blurs instead of looking jagged. The
            // settle check below cleans it up once the drag stops.
            render_set_scale_mode(&vt->ren, SDL_ScaleModeLinear);
            vt->resize_pending = 1;
            vt->last_resize_tick = SDL_GetTicks();
            vt->scr.dirty = 1; // redraw at the new size right away, not on the next unrelated change
            continue;
        }
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_TEXTINPUT) {
            if (!vt->input_enabled) return 1;
            if (ev.type == SDL_KEYDOWN) {
                rt_core_handle_keydown(vt, &ev.key.keysym);
            } else {
                rt_core_handle_textinput(vt, ev.text.text);
            }
        }
    }

    // Drag's over - square up both dimensions to one scale factor
    // (kills the letterbox either way). Close to a whole number (within
    // 0.1)? Snap to it and go crisp. Otherwise leave it fractional and
    // blurry - beats a jagged in-between scale.
    if (vt->resize_pending && SDL_GetTicks() - vt->last_resize_tick >= RUNVT_RESIZE_SETTLE_MS) {
        int w, h;
        SDL_GetWindowSize(vt->ren.win, &w, &h);
        double kx = (double)w / vt->ren.pw;
        double ky = (double)h / vt->ren.ph;
        double k = kx > ky ? kx : ky;
        if (k < 1.0) k = 1.0;

        int nearest = (int)(k + 0.5);
        if (nearest < 1) nearest = 1;
        double diff = k - nearest;
        if (diff < 0) diff = -diff;

        if (diff <= 0.1) {
            k = nearest;
            render_set_scale_mode(&vt->ren, SDL_ScaleModeNearest);
        } else {
            render_set_scale_mode(&vt->ren, SDL_ScaleModeLinear);
        }

        int nw = (int)(vt->ren.pw * k + 0.5);
        int nh = (int)(vt->ren.ph * k + 0.5);
        if (nw != w || nh != h) SDL_SetWindowSize(vt->ren.win, nw, nh);
        vt->scr.dirty = 1;
        vt->resize_pending = 0;
    }

    // SDL tracks capslock as toggle state, not a press/release event we
    // can just forward - poll it and force a status refresh on change
    // so it doesn't wait for some unrelated redraw to catch up.
    int caps = (SDL_GetModState() & KMOD_CAPS) ? 1 : 0;
    if (caps != vt->last_caps) {
        vt->last_caps = caps;
        vt->scr.dirty = 1;
    }

    return 0;
}

// Redraws only if something's actually changed since the last frame -
// a bell with nothing else to redraw still needs a fresh frame, since
// render_flash inverts whatever's currently in the framebuffer, so it
// has to be this tick's frame, not a stale one.
static void rt_core_maybe_render(RunVT *vt) {
    if (vt->vp.bell) vt->scr.dirty = 1;

    if (vt->scr.dirty) {
        char status_right[64];
        int caps = (SDL_GetModState() & KMOD_CAPS) ? 1 : 0;
        sprintf(status_right, "Row:%02d Col:%02d %s ", vt->scr.cur_y + 1, vt->scr.cur_x + 1,
                caps ? "CAPS" : "    ");
        scr_set_status(&vt->scr, vt->status_left, status_right);
        render_frame(&vt->ren, &vt->scr, &vt->sixel);
        vt->scr.dirty = 0;
    }

    if (vt->vp.bell) {
        render_flash(&vt->ren);
        vt->vp.bell = 0;
    }
}

static void rt_core_shutdown(RunVT *vt) {
    SDL_StopTextInput();
    render_destroy(&vt->ren);
    vt_free(&vt->vp);
    sixel_free(&vt->sixel);
    scr_free(&vt->scr);
}

#endif // RUNVT_CORE_H
