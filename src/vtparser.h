// vtparser.h - VT100/ANSI control sequence state machine.
//
// Takes raw bytes from the child, drives screen.h, and hands sixel DCS
// payloads off to sixel.h. A few queries (DSR, DA) need a reply sent
// back to the child or well-behaved apps will just sit there waiting
// for an answer that never comes - that goes out through a
// caller-supplied callback rather than a file descriptor, so this file
// still doesn't need to know it's talking to a pty.

#ifndef RUNVT_VTPARSER_H
#define RUNVT_VTPARSER_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "screen.h"
#include "sixel.h"

#define VT_ST_NORMAL  0
#define VT_ST_ESC     1
#define VT_ST_CSI     2
#define VT_ST_OSC     3
#define VT_ST_DCS     4
#define VT_ST_SCS_G0  5
#define VT_ST_SCS_G1  6

#define VT_MAX_PARAMS 16

typedef struct {
    int state;

    int params[VT_MAX_PARAMS];
    int nparams;
    int building;
    int private_marker; // '?' seen right after CSI, else 0

    unsigned char *dcs_buf;
    size_t dcs_len;
    size_t dcs_cap;

    int cell_w;
    int cell_h;

    int (*reply)(void *ctx, const unsigned char *buf, int len);
    void *reply_ctx;

    // OSC (window title, mostly) - buffered until its terminator shows
    // up, which is either BEL or the two-byte ST (ESC \).
    char osc_buf[256];
    size_t osc_len;
    int osc_pending_st;
    void (*set_title)(void *ctx, const char *title);
    void *title_ctx;

    int bell; // set on BEL, cleared by whoever's watching for it
} VTParser;

static void vt_init(VTParser *p, int cell_w, int cell_h,
                     int (*reply)(void *, const unsigned char *, int),
                     void *reply_ctx,
                     void (*set_title)(void *, const char *),
                     void *title_ctx) {
    memset(p, 0, sizeof(*p));
    p->state = VT_ST_NORMAL;
    p->cell_w = cell_w;
    p->cell_h = cell_h;
    p->reply = reply;
    p->reply_ctx = reply_ctx;
    p->set_title = set_title;
    p->title_ctx = title_ctx;
}

static void vt_free(VTParser *p) {
    free(p->dcs_buf);
    p->dcs_buf = NULL;
}

static void vt_dcs_append(VTParser *p, unsigned char b) {
    if (p->dcs_len + 1 > p->dcs_cap) {
        size_t newcap = p->dcs_cap ? p->dcs_cap * 2 : 256;
        p->dcs_buf = (unsigned char *)realloc(p->dcs_buf, newcap);
        p->dcs_cap = newcap;
    }
    p->dcs_buf[p->dcs_len++] = b;
}

// Sixel is the only DCS payload we care about. Its intro is Pa;Pb;Pc
// then a literal 'q', so a quick scan near the front of the buffer
// tells us whether this was a sixel image or something we should just
// drop on the floor (DECRQSS and friends - not implementing those).
static void vt_finish_dcs(VTParser *p, Screen *s, SixelLayer *sl) {
    size_t qi = (size_t)-1;
    size_t scan_limit = p->dcs_len < 32 ? p->dcs_len : 32;

    for (size_t i = 0; i < scan_limit; i++) {
        if (p->dcs_buf[i] == 'q') { qi = i; break; }
    }
    if (qi != (size_t)-1 && sl != NULL) {
        int origin_x = s->cur_x * p->cell_w;
        int origin_y = s->cur_y * p->cell_h;
        sixel_decode(sl, origin_x, origin_y, p->dcs_buf + qi + 1,
                     (int)(p->dcs_len - qi - 1));
        // sixel paints straight into the pixel overlay, not a Cell, so
        // it needs its own dirty poke - nothing else in this path
        // touches the screen buffer to trigger one otherwise.
        s->dirty = 1;
    }
    p->dcs_len = 0;
}

// Two flavors of param lookup, because CSI params aren't all created
// equal. Movement counts (CUU/CUD/CUP row+col, etc) treat a missing OR
// explicit-zero param the same way - "CSI 0A" means move up 1, not 0 -
// so vt_p() folds both cases to the default. Things like ED/EL modes
// and SGR codes need 0 to mean an actual 0, so they go through
// vt_praw() instead, which only substitutes the default when the param
// was truly absent.
static int vt_p(VTParser *p, int idx, int def) {
    if (idx >= p->nparams || p->params[idx] == 0) return def;
    return p->params[idx];
}

static int vt_praw(VTParser *p, int idx, int def) {
    if (idx >= p->nparams) return def;
    return p->params[idx];
}

static void vt_reply_str(VTParser *p, const char *s) {
    if (p->reply) p->reply(p->reply_ctx, (const unsigned char *)s, (int)strlen(s));
}

// OSC payload is "Ps;Pt" - Ps=0 (icon+title) or 2 (title only) are the
// ones every shell/editor actually uses, so that's all we bother with.
static void vt_finish_osc(VTParser *p) {
    p->osc_buf[p->osc_len] = '\0';
    char *semi = strchr(p->osc_buf, ';');
    if (!semi) return;
    int ps = atoi(p->osc_buf);
    if ((ps == 0 || ps == 2) && p->set_title) {
        p->set_title(p->title_ctx, semi + 1);
    }
}

// SGR params chain: "CSI 1;31m" means bold AND red, so we walk the
// whole list applying each one instead of just looking at params[0].
// A bare "CSI m" with no params at all means reset, same as "CSI 0m".
static void vt_apply_sgr(Screen *s, VTParser *p) {
    int n = p->nparams == 0 ? 1 : p->nparams;

    for (int i = 0; i < n; i++) {
        int v = vt_praw(p, i, 0);

        if (v == 0) {
            s->cur_attr = 0;
            s->cur_fg = 7;
            s->cur_bg = 0;
        } else if (v == 1) {
            s->cur_attr |= ATTR_BOLD;
        } else if (v == 4) {
            s->cur_attr |= ATTR_UNDERLINE;
        } else if (v == 7) {
            s->cur_attr |= ATTR_REVERSE;
        } else if (v == 22) {
            s->cur_attr &= ~ATTR_BOLD;
        } else if (v == 24) {
            s->cur_attr &= ~ATTR_UNDERLINE;
        } else if (v == 27) {
            s->cur_attr &= ~ATTR_REVERSE;
        } else if (v >= 30 && v <= 37) {
            s->cur_fg = (unsigned char)(v - 30);
        } else if (v == 39) {
            s->cur_fg = 7;
        } else if (v >= 40 && v <= 47) {
            s->cur_bg = (unsigned char)(v - 40);
        } else if (v == 49) {
            s->cur_bg = 0;
        } else if (v >= 90 && v <= 97) {
            s->cur_fg = (unsigned char)(v - 90 + 8);
        } else if (v >= 100 && v <= 107) {
            s->cur_bg = (unsigned char)(v - 100 + 8);
        }
    }
}

static void vt_dispatch_csi(Screen *s, VTParser *p, SixelLayer *sl, unsigned char final) {
    switch (final) {
        case 'A': scr_cursor_move(s, 0, -vt_p(p, 0, 1)); break;
        case 'B': scr_cursor_move(s, 0,  vt_p(p, 0, 1)); break;
        case 'C': scr_cursor_move(s,  vt_p(p, 0, 1), 0); break;
        case 'D': scr_cursor_move(s, -vt_p(p, 0, 1), 0); break;
        case 'H':
        case 'f': scr_cursor_set(s, vt_p(p, 0, 1), vt_p(p, 1, 1)); break;
        case 'J': scr_erase_screen(s, vt_praw(p, 0, 0), sl); break;
        case 'K': scr_erase_line(s, vt_praw(p, 0, 0), sl); break;
        case 'm': vt_apply_sgr(s, p); break;
        case 'r': {
            int top = vt_p(p, 0, 1);
            int bottom = vt_p(p, 1, s->rows);
            scr_set_scroll_region(s, top, bottom);
            break;
        }
        // ANSI.SYS-style save/restore (as opposed to the real VT100 ESC
        // 7 / ESC 8 above) - lots of DOS/CP-M-heritage software uses
        // this form, cheap to support both.
        case 's':
            if (!p->private_marker) scr_save_cursor(s);
            break;
        case 'u':
            if (!p->private_marker) scr_restore_cursor(s);
            break;
        case 'h':
            if (p->private_marker == '?' && vt_praw(p, 0, 0) == 25) {
                s->cursor_visible = 1;
                s->dirty = 1;
            }
            break;
        case 'l':
            if (p->private_marker == '?' && vt_praw(p, 0, 0) == 25) {
                s->cursor_visible = 0;
                s->dirty = 1;
            }
            break;
        case '@': scr_insert_chars(s, vt_p(p, 0, 1), sl); break;
        case 'P': scr_delete_chars(s, vt_p(p, 0, 1), sl); break;
        case 'X': scr_erase_chars(s, vt_p(p, 0, 1), sl); break;
        case 'L': scr_insert_lines(s, vt_p(p, 0, 1), sl); break;
        case 'M': scr_delete_lines(s, vt_p(p, 0, 1), sl); break;
        // DSR - device status report. A handful of full-screen apps
        // (status-line editors especially) send "CSI 6n" to ask where
        // the cursor is and just block until they get an answer. Worth
        // the few lines to answer it rather than have those apps hang.
        case 'n': {
            int code = vt_praw(p, 0, 0);
            if (code == 6) {
                char buf[32];
                sprintf(buf, "\x1B[%d;%dR", s->cur_y + 1, s->cur_x + 1);
                vt_reply_str(p, buf);
            } else if (code == 5) {
                vt_reply_str(p, "\x1B[0n");
            }
            break;
        }
        // DA - device attributes. Same story as DSR: some apps probe
        // "what kind of terminal is this" before doing anything else.
        // Claiming plain VT100 (no options) is the safest answer.
        case 'c':
            if (!p->private_marker) vt_reply_str(p, "\x1B[?1;0c");
            break;
        default:
            break;
    }
}

static void vt_feed_byte(Screen *s, VTParser *p, SixelLayer *sl, unsigned char b) {
    // ESC always means "abandon whatever we were doing and start a new
    // escape sequence" - handling it once up here instead of in every
    // state means a malformed CSI/OSC/DCS can't wedge the parser.
    if (b == 0x1B) {
        if (p->state == VT_ST_DCS) vt_finish_dcs(p, s, sl);
        else if (p->state == VT_ST_OSC) p->osc_pending_st = 1; // might be ST (ESC \)
        p->state = VT_ST_ESC;
        return;
    }

    switch (p->state) {
        case VT_ST_NORMAL:
            if (b < 0x20) {
                switch (b) {
                    case 0x08: if (s->cur_x > 0) { s->cur_x--; s->dirty = 1; } break;
                    case 0x09: {
                        int nx = ((s->cur_x / 8) + 1) * 8;
                        s->cur_x = nx > s->cols - 1 ? s->cols - 1 : nx;
                        s->dirty = 1;
                        break;
                    }
                    case 0x0A: case 0x0B: case 0x0C: scr_newline(s, sl); break;
                    case 0x0D: scr_cr(s); break;
                    case 0x0E: s->gl_is_g1 = 1; break;
                    case 0x0F: s->gl_is_g1 = 0; break;
                    case 0x07: p->bell = 1; break;
                    default: break;
                }
            } else if (b != 0x7F) {
                scr_putc(s, b, sl);
            }
            break;

        case VT_ST_ESC:
            switch (b) {
                case '[': p->state = VT_ST_CSI; p->nparams = 0; p->building = 0;
                          p->private_marker = 0; break;
                case ']': p->state = VT_ST_OSC; p->osc_len = 0; break;
                case 'P': p->state = VT_ST_DCS; p->dcs_len = 0; break;
                case '(': p->state = VT_ST_SCS_G0; break;
                case ')': p->state = VT_ST_SCS_G1; break;
                case '7': scr_save_cursor(s); p->state = VT_ST_NORMAL; break;
                case '8': scr_restore_cursor(s); p->state = VT_ST_NORMAL; break;
                case 'D': scr_newline(s, sl); p->state = VT_ST_NORMAL; break;
                case 'M': scr_reverse_index(s, sl); p->state = VT_ST_NORMAL; break;
                case 'E': scr_cr(s); scr_newline(s, sl); p->state = VT_ST_NORMAL; break;
                // RIS - full reset. Clear screen, home cursor, back to
                // default colors/attrs/charsets. Some apps fire this on
                // startup just to be safe about terminal state.
                // scr_erase_screen(mode 2) already wipes sixel pixels
                // row by row now, so a plain full-screen erase is
                // enough - no separate sixel_clear() needed here.
                case 'c':
                    scr_erase_screen(s, 2, sl);
                    s->cur_x = 0; s->cur_y = 0;
                    s->cur_fg = 7; s->cur_bg = 0; s->cur_attr = 0;
                    s->g0_charset = CHARSET_NORMAL; s->g1_charset = CHARSET_NORMAL;
                    s->gl_is_g1 = 0;
                    p->state = VT_ST_NORMAL;
                    break;
                case '\\':
                    if (p->osc_pending_st) { vt_finish_osc(p); p->osc_pending_st = 0; }
                    p->state = VT_ST_NORMAL;
                    break;
                default: p->state = VT_ST_NORMAL; break;
            }
            break;

        case VT_ST_CSI:
            if (b == '?' || b == '>' || b == '=') {
                p->private_marker = b;
            } else if (b >= '0' && b <= '9') {
                if (!p->building) {
                    if (p->nparams < VT_MAX_PARAMS) p->params[p->nparams] = 0;
                    p->building = 1;
                }
                if (p->nparams < VT_MAX_PARAMS) {
                    p->params[p->nparams] = p->params[p->nparams] * 10 + (b - '0');
                }
            } else if (b == ';') {
                if (p->nparams < VT_MAX_PARAMS) p->nparams++;
                p->building = 0;
            } else if (b >= 0x40 && b <= 0x7E) {
                if (p->building && p->nparams < VT_MAX_PARAMS) p->nparams++;
                vt_dispatch_csi(s, p, sl, b);
                p->state = VT_ST_NORMAL;
            }
            break;

        case VT_ST_OSC:
            if (b == 0x07) {
                vt_finish_osc(p);
                p->state = VT_ST_NORMAL;
            } else if (p->osc_len + 1 < sizeof(p->osc_buf)) {
                p->osc_buf[p->osc_len++] = (char)b;
            }
            break;

        case VT_ST_DCS:
            vt_dcs_append(p, b);
            break;

        case VT_ST_SCS_G0:
            s->g0_charset = (b == '0') ? CHARSET_SPECIAL_GRAPHICS : CHARSET_NORMAL;
            p->state = VT_ST_NORMAL;
            break;

        case VT_ST_SCS_G1:
            s->g1_charset = (b == '0') ? CHARSET_SPECIAL_GRAPHICS : CHARSET_NORMAL;
            p->state = VT_ST_NORMAL;
            break;

        default:
            p->state = VT_ST_NORMAL;
            break;
    }
}

static void vt_process(Screen *s, VTParser *p, SixelLayer *sl,
                        const unsigned char *buf, int len) {
    for (int i = 0; i < len; i++) {
        vt_feed_byte(s, p, sl, buf[i]);
    }
}

#endif // RUNVT_VTPARSER_H
