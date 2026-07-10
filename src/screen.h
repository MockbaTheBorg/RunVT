/*
 * screen.h - character cell grid, cursor state, scrolling.
 *
 * No OS or rendering knowledge in here on purpose - vtparser.h drives
 * this, render.h just reads it.
 */

#ifndef RUNVT_SCREEN_H
#define RUNVT_SCREEN_H

#include <stdlib.h>
#include <string.h>
#include "sixel.h"

#define ATTR_BOLD       0x01
#define ATTR_UNDERLINE  0x02
#define ATTR_REVERSE    0x04

#define CHARSET_NORMAL            0  // ASCII / active upper-128 codepage
#define CHARSET_SPECIAL_GRAPHICS  1  // DEC Special Graphics line-drawing

typedef struct {
    unsigned char ch;
    unsigned char charset;
    unsigned char fg;
    unsigned char bg;
    unsigned char attr;
} Cell;

typedef struct {
    int cols;
    int rows;       // addressable rows only - status line (if any) is extra
    Cell *cells;

    int status_row;  // row index of the status line, or -1 if none
    int total_rows;  // rows, plus 1 if status_row is in use

    int cur_x;
    int cur_y;
    int cursor_visible;

    unsigned char cur_fg;
    unsigned char cur_bg;
    unsigned char cur_attr;

    unsigned char g0_charset;
    unsigned char g1_charset;
    int gl_is_g1;              // SI/SO: which of G0/G1 is currently invoked

    int saved_x;
    int saved_y;
    unsigned char saved_fg;
    unsigned char saved_bg;
    unsigned char saved_attr;

    int scroll_top;    // DECSTBM region, 0-based, inclusive
    int scroll_bottom;

    int cell_w;         // glyph size in pixels, needed to keep a sixel
    int cell_h;          // image in sync when its rows scroll - 0 if unset

    int dirty;          // whole-screen dirty flag for renderer
} Screen;

static void scr_init(Screen *s, int cols, int rows, int with_status_line) {
    s->cols = cols;
    s->rows = rows;
    s->status_row = with_status_line ? rows : -1;
    s->total_rows = rows + (with_status_line ? 1 : 0);
    s->cells = (Cell *)malloc(sizeof(Cell) * (size_t)cols * (size_t)s->total_rows);

    s->cur_x = 0;
    s->cur_y = 0;
    s->cursor_visible = 1;

    s->cur_fg = 7;
    s->cur_bg = 0;
    s->cur_attr = 0;

    s->g0_charset = CHARSET_NORMAL;
    s->g1_charset = CHARSET_NORMAL;
    s->gl_is_g1 = 0;

    s->saved_x = 0;
    s->saved_y = 0;
    s->saved_fg = 7;
    s->saved_bg = 0;
    s->saved_attr = 0;

    s->scroll_top = 0;
    s->scroll_bottom = rows - 1;

    s->cell_w = 0;
    s->cell_h = 0;

    s->dirty = 1;

    for (int i = 0; i < cols * s->total_rows; i++) {
        s->cells[i].ch = ' ';
        s->cells[i].charset = CHARSET_NORMAL;
        s->cells[i].fg = 7;
        s->cells[i].bg = 0;
        s->cells[i].attr = 0;
    }
}

// Called once after scr_init with the font's pixel dimensions, so
// scrolling can shift the sixel overlay in lockstep with the text
// grid. Not part of scr_init itself since plenty of callers (offline
// tests, mainly) don't care about sixel/scroll interaction at all.
static void scr_set_cell_px(Screen *s, int cell_w, int cell_h) {
    s->cell_w = cell_w;
    s->cell_h = cell_h;
}

static void scr_free(Screen *s) {
    free(s->cells);
    s->cells = NULL;
}

static Cell *scr_cell(Screen *s, int x, int y) {
    return &s->cells[y * s->cols + x];
}

// Text-only, no sixel side effect - used internally by the scroll
// functions below, which already handle their own sixel shifting via
// sixel_scroll_up/down. Clearing sixel pixels here too, on top of
// that, would race the shift: this blanks row `bottom` (say) before
// sixel_scroll_up gets a chance to read it as the source for the
// pixels moving into row `bottom-1`, corrupting the scroll. The
// public, sixel-aware version right below this is what EL/ED/ECH use,
// where there's no separate shift to conflict with.
static void scr_clear_cells(Screen *s, int row, int x0, int x1) {
    for (int x = x0; x <= x1; x++) {
        Cell *c = scr_cell(s, x, row);
        c->ch = ' ';
        c->charset = CHARSET_NORMAL;
        c->fg = s->cur_fg;
        c->bg = s->cur_bg;
        c->attr = 0;
    }
    s->dirty = 1;
}

// sl may be NULL (offline tests, or a caller that never touches sixel)
// - the text grid clears fine either way, there's just no image layer
// to punch a hole in.
static void scr_clear_row_range(Screen *s, int row, int x0, int x1, SixelLayer *sl) {
    scr_clear_cells(s, row, x0, x1);
    if (sl != NULL) sixel_clear_region(sl, row, x0, x1, s->cell_w, s->cell_h);
}

static void scr_scroll_up(Screen *s, int top, int bottom, SixelLayer *sl) {
    int rowbytes = (int)sizeof(Cell) * s->cols;

    if (bottom > top) {
        memmove(scr_cell(s, 0, top), scr_cell(s, 0, top + 1),
                (size_t)rowbytes * (size_t)(bottom - top));
    }
    scr_clear_cells(s, bottom, 0, s->cols - 1);
    if (sl != NULL && s->cell_h > 0) sixel_scroll_up(sl, top, bottom, s->cell_h);
    s->dirty = 1;
}

static void scr_newline(Screen *s, SixelLayer *sl) {
    if (s->cur_y == s->scroll_bottom) {
        scr_scroll_up(s, s->scroll_top, s->scroll_bottom, sl);
    } else if (s->cur_y < s->rows - 1) {
        s->cur_y++;
        s->dirty = 1;
    }
}

static void scr_cr(Screen *s) {
    s->cur_x = 0;
    s->dirty = 1;
}

static void scr_scroll_down(Screen *s, int top, int bottom, SixelLayer *sl) {
    int rowbytes = (int)sizeof(Cell) * s->cols;

    if (bottom > top) {
        memmove(scr_cell(s, 0, top + 1), scr_cell(s, 0, top),
                (size_t)rowbytes * (size_t)(bottom - top));
    }
    scr_clear_cells(s, top, 0, s->cols - 1);
    if (sl != NULL && s->cell_h > 0) sixel_scroll_down(sl, top, bottom, s->cell_h);
    s->dirty = 1;
}

// RI - Reverse Index: cursor up one line, scrolling the region down if
// we're already sitting on the top margin.
static void scr_reverse_index(Screen *s, SixelLayer *sl) {
    if (s->cur_y == s->scroll_top) {
        scr_scroll_down(s, s->scroll_top, s->scroll_bottom, sl);
    } else if (s->cur_y > 0) {
        s->cur_y--;
        s->dirty = 1;
    }
}

// DECSTBM - set top/bottom scroll margins. Rows come in 1-based off the
// wire like everything else in CSI land. A degenerate range just resets
// to the full screen instead of erroring out - simplest thing that
// won't wedge a misbehaving app.
static void scr_set_scroll_region(Screen *s, int top, int bottom) {
    if (top < 1) top = 1;
    if (bottom > s->rows) bottom = s->rows;
    if (top >= bottom) {
        top = 1;
        bottom = s->rows;
    }
    s->scroll_top = top - 1;
    s->scroll_bottom = bottom - 1;
    s->cur_x = 0;
    s->cur_y = s->scroll_top;
    s->dirty = 1;
}

// Writes one glyph at the cursor with the current attrs/charset, then
// advances the cursor with VT100-style autowrap.
static void scr_putc(Screen *s, unsigned char ch, SixelLayer *sl) {
    if (s->cur_x >= s->cols) {
        scr_cr(s);
        scr_newline(s, sl);
    }

    Cell *c = scr_cell(s, s->cur_x, s->cur_y);
    c->ch = ch;
    c->charset = s->gl_is_g1 ? s->g1_charset : s->g0_charset;
    if (s->cur_attr & ATTR_REVERSE) {
        c->fg = s->cur_bg;
        c->bg = s->cur_fg;
    } else {
        c->fg = s->cur_fg;
        c->bg = s->cur_bg;
    }
    c->attr = s->cur_attr;

    s->cur_x++;
    s->dirty = 1;
}

// Every cursor-repositioning path (relative move, absolute CUP, DECRC
// restore) funnels through here, which is also why this is the one
// place that marks the screen dirty for a plain move. Moving the
// cursor with no accompanying cell write - the common case for pure
// navigation with no character typed - was previously not triggering
// a redraw at all: the internal position updated fine, but the
// visible cursor block just sat wherever it was last drawn until some
// unrelated write finally set dirty. Looked exactly like "cursor gets
// stuck" for anything that moves without also printing.
static void scr_cursor_clip(Screen *s) {
    if (s->cur_x < 0) s->cur_x = 0;
    if (s->cur_y < 0) s->cur_y = 0;
    if (s->cur_x > s->cols - 1) s->cur_x = s->cols - 1;
    if (s->cur_y > s->rows - 1) s->cur_y = s->rows - 1;
    s->dirty = 1;
}

static void scr_cursor_move(Screen *s, int dx, int dy) {
    s->cur_x += dx;
    s->cur_y += dy;
    scr_cursor_clip(s);
}

// 1-based row/col, VT100 CUP style.
static void scr_cursor_set(Screen *s, int row, int col) {
    s->cur_x = col - 1;
    s->cur_y = row - 1;
    scr_cursor_clip(s);
}

// ED - erase in display. 0=cursor..end, 1=start..cursor, 2=whole screen.
// Deliberately never touches the status row - that's not part of the
// addressable screen, same as on real hardware.
static void scr_erase_screen(Screen *s, int mode, SixelLayer *sl) {
    if (mode == 2) {
        for (int y = 0; y < s->rows; y++) scr_clear_row_range(s, y, 0, s->cols - 1, sl);
    } else if (mode == 0) {
        scr_clear_row_range(s, s->cur_y, s->cur_x, s->cols - 1, sl);
        for (int y = s->cur_y + 1; y < s->rows; y++) scr_clear_row_range(s, y, 0, s->cols - 1, sl);
    } else if (mode == 1) {
        for (int y = 0; y < s->cur_y; y++) scr_clear_row_range(s, y, 0, s->cols - 1, sl);
        scr_clear_row_range(s, s->cur_y, 0, s->cur_x, sl);
    }
}

// EL - erase in line. 0=cursor..end, 1=start..cursor, 2=whole line.
static void scr_erase_line(Screen *s, int mode, SixelLayer *sl) {
    if (mode == 0) {
        scr_clear_row_range(s, s->cur_y, s->cur_x, s->cols - 1, sl);
    } else if (mode == 1) {
        scr_clear_row_range(s, s->cur_y, 0, s->cur_x, sl);
    } else if (mode == 2) {
        scr_clear_row_range(s, s->cur_y, 0, s->cols - 1, sl);
    }
}

// ICH - insert Pn blank chars at the cursor, shifting the rest of the
// line right (chars pushed off the right edge are gone). Shifts any
// sixel pixels sitting in that row right along with the text.
static void scr_insert_chars(Screen *s, int n, SixelLayer *sl) {
    int avail = s->cols - s->cur_x;
    if (n > avail) n = avail;
    if (n < 1) return;

    if (avail - n > 0) {
        memmove(scr_cell(s, s->cur_x + n, s->cur_y), scr_cell(s, s->cur_x, s->cur_y),
                (size_t)(avail - n) * sizeof(Cell));
    }
    if (sl != NULL) {
        sixel_shift_right(sl, s->cur_y, s->cur_x, s->cols, n, s->cell_w, s->cell_h);
    }
    scr_clear_row_range(s, s->cur_y, s->cur_x, s->cur_x + n - 1, sl);
}

// DCH - delete Pn chars at the cursor, shifting the rest of the line
// left and blanking the vacated cells at the end. Same sixel handling
// as ICH, just leftward.
static void scr_delete_chars(Screen *s, int n, SixelLayer *sl) {
    int avail = s->cols - s->cur_x;
    if (n > avail) n = avail;
    if (n < 1) return;

    if (avail - n > 0) {
        memmove(scr_cell(s, s->cur_x, s->cur_y), scr_cell(s, s->cur_x + n, s->cur_y),
                (size_t)(avail - n) * sizeof(Cell));
    }
    if (sl != NULL) {
        sixel_shift_left(sl, s->cur_y, s->cur_x, s->cols, n, s->cell_w, s->cell_h);
    }
    scr_clear_row_range(s, s->cur_y, s->cols - n, s->cols - 1, sl);
}

// ECH - blank Pn chars at the cursor in place, no shifting.
static void scr_erase_chars(Screen *s, int n, SixelLayer *sl) {
    int end = s->cur_x + n - 1;
    if (end > s->cols - 1) end = s->cols - 1;
    if (end < s->cur_x) return;
    scr_clear_row_range(s, s->cur_y, s->cur_x, end, sl);
}

// IL - insert Pn blank lines at the cursor row, scrolling the rest of
// the scroll region down (lines pushed past the bottom margin are
// gone). One scroll_down per line is simplest and n is always small
// in practice, so there's no need for a fancier bulk version.
static void scr_insert_lines(Screen *s, int n, SixelLayer *sl) {
    for (int i = 0; i < n && s->cur_y <= s->scroll_bottom; i++) {
        scr_scroll_down(s, s->cur_y, s->scroll_bottom, sl);
    }
}

// DL - delete Pn lines at the cursor row, scrolling the rest of the
// scroll region up.
static void scr_delete_lines(Screen *s, int n, SixelLayer *sl) {
    for (int i = 0; i < n && s->cur_y <= s->scroll_bottom; i++) {
        scr_scroll_up(s, s->cur_y, s->scroll_bottom, sl);
    }
}

static void scr_save_cursor(Screen *s) {
    s->saved_x = s->cur_x;
    s->saved_y = s->cur_y;
    s->saved_fg = s->cur_fg;
    s->saved_bg = s->cur_bg;
    s->saved_attr = s->cur_attr;
}

static void scr_restore_cursor(Screen *s) {
    s->cur_x = s->saved_x;
    s->cur_y = s->saved_y;
    s->cur_fg = s->saved_fg;
    s->cur_bg = s->saved_bg;
    s->cur_attr = s->saved_attr;
    scr_cursor_clip(s);
}

// Stamps the status bar: left text starting at col 0, right text
// ending at the last column, everything between filled with blanks.
// If the two would overlap, right just gets pushed flush against left
// rather than truncated - status text is short enough in practice
// (app name / row,col / CAPS) that this never actually happens at 80
// columns or wider. Not host-writable like a real VT220 status line
// (DECSASD) - that would need its own bit of parser plumbing for a
// feature nobody running CP/M software is going to hit. This is
// RunVT's own bar, redrawn from scratch each time something changes.
static void scr_set_status(Screen *s, const char *left, const char *right) {
    if (s->status_row < 0) return;

    int left_len = (int)strlen(left);
    int right_len = (int)strlen(right);
    int right_start = s->cols - right_len;
    if (right_start < left_len) right_start = left_len;

    for (int x = 0; x < s->cols; x++) {
        Cell *c = scr_cell(s, x, s->status_row);
        unsigned char ch = ' ';
        if (x < left_len) {
            ch = (unsigned char)left[x];
        } else if (x >= right_start && (x - right_start) < right_len) {
            ch = (unsigned char)right[x - right_start];
        }
        c->ch = ch;
        c->charset = CHARSET_NORMAL;
        c->fg = 0;
        c->bg = 7;
        c->attr = 0;
    }
    s->dirty = 1;
}

#endif // RUNVT_SCREEN_H
