// RunVT - a simple, ANSI/VT100-compatible graphical terminal emulator.
//
// Usage: runvt [--wait] [--codepage=latin1|cp850] [--size=COLSxROWS] [--log=FILE] app [args...]
//
// Spawns app as a child attached to a pty, renders its output as an
// 80x25 (by default) VT100-style screen using an embedded bitmap font,
// plus a status bar RunVT draws itself, and forwards keyboard input
// back to it. That's the whole job - no scrollback, no smooth
// scrolling. Just a compatible, no-frills console for RunCPM and
// friends.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "version.h"
#include "screen.h"
#include "font.h"
#include "cp850_encode.h"
#include "sixel.h"
#include "vtparser.h"
#include "render.h"

#if defined(_WIN32)
#include "abstraction_windows.h"
#else
#include "abstraction_posix.h"
#endif

#define TERM_COLS 80

static int pty_reply(void *ctx, const unsigned char *buf, int len) {
    RT_Process *proc = (RT_Process *)ctx;
    return rt_write(proc, buf, len);
}

// Minimal UTF-8 decoder for SDL_TEXTINPUT text. SDL hands us composed
// keyboard input as UTF-8 regardless of what codepage we're emulating,
// so this has to happen before we can encode down to Latin-1/CP850.
static int utf8_decode(const char *s, unsigned int *out_cp) {
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

static unsigned char encode_char(unsigned int cp) {
    if (cp < 0x80) return (unsigned char)cp;
    if (g_active_codepage == CODEPAGE_CP850) return cp850_encode(cp);
    return cp < 0x100 ? (unsigned char)cp : '?';
}

static void send_str(RT_Process *proc, const char *s) {
    rt_write(proc, (const unsigned char *)s, (int)strlen(s));
}

static void handle_textinput(RT_Process *proc, const char *text);

static void set_window_title(void *ctx, const char *title) {
    SDL_SetWindowTitle(((Renderer *)ctx)->win, title);
}

// Only the keys that don't show up as SDL_TEXTINPUT: control keys,
// arrows, function keys, ctrl-combos. Printable characters (including
// accented ones) go through handle_textinput() instead, further down -
// doing both here would double-send every normal keystroke.
static void handle_keydown(RT_Process *proc, SDL_Keysym *ks) {
    SDL_Keycode sym = ks->sym;
    Uint16 mod = ks->mod;

    // Ctrl+Shift+V pastes - checked ahead of the plain ctrl-letter case
    // below, or this would just send a literal Ctrl+V (0x16) instead.
    if ((mod & KMOD_CTRL) && (mod & KMOD_SHIFT) && sym == SDLK_v) {
        if (SDL_HasClipboardText()) {
            char *text = SDL_GetClipboardText();
            handle_textinput(proc, text);
            SDL_free(text);
        }
        return;
    }

    if ((mod & KMOD_CTRL) && sym >= SDLK_a && sym <= SDLK_z) {
        unsigned char b = (unsigned char)(sym - SDLK_a + 1);
        rt_write(proc, &b, 1);
        return;
    }

    switch (sym) {
        // CP/M-era apps (WordStar and friends) expect BS here, not DEL.
        // SDL gives the numpad Enter its own keycode, separate from
        // the main one - both should just mean "Enter" to the app.
        case SDLK_RETURN:
        case SDLK_KP_ENTER:  send_str(proc, "\r"); break;
        case SDLK_BACKSPACE: send_str(proc, "\x08"); break;
        case SDLK_TAB:       send_str(proc, "\x09"); break;
        case SDLK_ESCAPE:    send_str(proc, "\x1B"); break;
        case SDLK_DELETE:    send_str(proc, "\x7F"); break;
        case SDLK_UP:        send_str(proc, "\x1B[A"); break;
        case SDLK_DOWN:      send_str(proc, "\x1B[B"); break;
        case SDLK_RIGHT:     send_str(proc, "\x1B[C"); break;
        case SDLK_LEFT:      send_str(proc, "\x1B[D"); break;
        case SDLK_HOME:      send_str(proc, "\x1B[H"); break;
        case SDLK_END:       send_str(proc, "\x1B[F"); break;
        // real VT100 PF1-PF4 codes, in case anything still cares
        case SDLK_F1:        send_str(proc, "\x1BOP"); break;
        case SDLK_F2:        send_str(proc, "\x1BOQ"); break;
        case SDLK_F3:        send_str(proc, "\x1BOR"); break;
        case SDLK_F4:        send_str(proc, "\x1BOS"); break;
        default: break;
    }
}

static void handle_textinput(RT_Process *proc, const char *text) {
    const char *p = text;
    while (*p) {
        unsigned int cp;
        int n = utf8_decode(p, &cp);
        unsigned char b = encode_char(cp);
        rt_write(proc, &b, 1);
        p += n;
    }
}

// Accepts "0xRRGGBB" (or plain decimal, but nobody's going to type
// that for a color) - strtoul's base-0 already handles the 0x prefix.
static int parse_hex_color(const char *s, unsigned char rgb[3]) {
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return -1;
    rgb[0] = (unsigned char)((v >> 16) & 0xFF);
    rgb[1] = (unsigned char)((v >> 8) & 0xFF);
    rgb[2] = (unsigned char)(v & 0xFF);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--wait] [--codepage=latin1|cp850] [--size=COLSxROWS]\n"
        "       [--bell=0xRRGGBB] [--normal=0xRRGGBB] [--bold=0xRRGGBB]\n"
        "       [--log=FILE] app [args...]\n",
        prog);
}

int main(int argc, char **argv) {
    rt_attach_console();

    int wait_flag = 0;
    int rows = 25;
    int app_start = -1;
    int cols = TERM_COLS;
    const char *log_path = NULL;
    FILE *rawlog = NULL;

    Screen scr;
    SixelLayer sixel;
    VTParser vp;
    Renderer ren;
    RT_Process proc;

    int running = 1;
    int child_alive = 1;
    int wait_prompted = 0;
    char title[256];

    // No cross-platform "mouse released" event exists for a window
    // resize, so we fake it: wait for a gap in resize events instead.
    #define RESIZE_SETTLE_MS 150
    int resize_pending = 0;
    Uint32 last_resize_tick = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wait") == 0) {
            wait_flag = 1;
        } else if (strncmp(argv[i], "--codepage=", 11) == 0) {
            const char *v = argv[i] + 11;
            if (strcmp(v, "cp850") == 0) g_active_codepage = CODEPAGE_CP850;
            else g_active_codepage = CODEPAGE_LATIN1;
        } else if (strncmp(argv[i], "--log=", 6) == 0) {
            log_path = argv[i] + 6;
        } else if (strncmp(argv[i], "--bell=", 7) == 0) {
            if (parse_hex_color(argv[i] + 7, g_bell_color) != 0) {
                fprintf(stderr, "%s: --bell wants 0xRRGGBB, e.g. 0x505050\n", argv[0]);
                return 1;
            }
        } else if (strncmp(argv[i], "--normal=", 9) == 0) {
            if (parse_hex_color(argv[i] + 9, render_palette[7]) != 0) {
                fprintf(stderr, "%s: --normal wants 0xRRGGBB, e.g. 0xAAAAAA\n", argv[0]);
                return 1;
            }
        } else if (strncmp(argv[i], "--bold=", 7) == 0) {
            if (parse_hex_color(argv[i] + 7, render_palette[15]) != 0) {
                fprintf(stderr, "%s: --bold wants 0xRRGGBB, e.g. 0xFFFFFF\n", argv[0]);
                return 1;
            }
        } else if (strncmp(argv[i], "--size=", 7) == 0) {
            int c, r;
            if (sscanf(argv[i] + 7, "%dx%d", &c, &r) == 2 && c > 0 && r > 0) {
                cols = c;
                rows = r;
            } else {
                fprintf(stderr, "%s: --size wants COLSxROWS, e.g. 80x25\n", argv[0]);
                return 1;
            }
        } else if (strncmp(argv[i], "--", 2) == 0 && strlen(argv[i]) > 2) {
            print_usage(argv[0]);
            return 1;
        } else {
            app_start = i;
            break;
        }
    }

    if (app_start < 0) {
        print_usage(argv[0]);
        return 1;
    }

    int child_argc = argc - app_start;
    char **child_argv = (char **)malloc(sizeof(char *) * (size_t)(child_argc + 1));
    for (int i = 0; i < child_argc; i++) child_argv[i] = argv[app_start + i];
    child_argv[child_argc] = NULL;

    // The status bar is always present now, on top of whatever --size
    // gives the app - not an optional 25th row anymore. The app itself
    // only ever gets `rows` addressable rows; the bar is RunVT's own
    // extra row underneath, same idea as a VT220's status line row.
    scr_init(&scr, cols, rows, 1);
    scr_set_cell_px(&scr, FONT_W, FONT_H);
    sixel_init(&sixel, cols * FONT_W, scr.total_rows * FONT_H);

    if (rt_spawn(&proc, child_argv, cols, rows) != 0) {
        fprintf(stderr, "%s: failed to launch '%s'\n", argv[0], child_argv[0]);
        return 1;
    }

    vt_init(&vp, FONT_W, FONT_H, pty_reply, &proc, set_window_title, &ren);

    // --log is a debugging aid, not something normal use needs: dumps
    // the raw bytes coming from the child so a misbehaving control
    // sequence can be seen exactly as sent, not just its rendered effect.
    if (log_path) {
        rawlog = fopen(log_path, "wb");
        if (!rawlog) {
            fprintf(stderr, "%s: couldn't open log file '%s'\n", argv[0], log_path);
        }
    }

    sprintf(title, "RunVT %s - %s", RUNVT_VERSION, child_argv[0]);
    if (render_init(&ren, cols, scr.total_rows, title) != 0) {
        fprintf(stderr, "%s: failed to initialize SDL renderer\n", argv[0]);
        return 1;
    }

    char status_left[192];
    sprintf(status_left, " RunVT %s - %s", RUNVT_VERSION, child_argv[0]);
    int last_caps = -1;
    scr_set_status(&scr, status_left, "");

    SDL_StartTextInput();

    while (running) {
        SDL_Event ev;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
                continue;
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                // Let it sit wherever the drag put it - linear filter
                // so a non-integer scale blurs instead of looking
                // jagged. The settle check below cleans it up once the
                // drag stops.
                render_set_scale_mode(&ren, SDL_ScaleModeLinear);
                resize_pending = 1;
                last_resize_tick = SDL_GetTicks();
                scr.dirty = 1; // redraw at the new size right away, not on the next unrelated change
                continue;
            }
            if (!child_alive) {
                if (wait_flag && (ev.type == SDL_KEYDOWN || ev.type == SDL_TEXTINPUT)) {
                    running = 0;
                }
                continue;
            }
            if (ev.type == SDL_KEYDOWN) {
                handle_keydown(&proc, &ev.key.keysym);
            } else if (ev.type == SDL_TEXTINPUT) {
                handle_textinput(&proc, ev.text.text);
            }
        }
        if (!running) break;

        // Drag's over - square up both dimensions to one scale factor
        // (kills the letterbox either way). Close to a whole number
        // (within 0.1)? Snap to it and go crisp. Otherwise leave it
        // fractional and blurry - beats a jagged in-between scale.
        if (resize_pending && SDL_GetTicks() - last_resize_tick >= RESIZE_SETTLE_MS) {
            int w, h;
            SDL_GetWindowSize(ren.win, &w, &h);
            double kx = (double)w / ren.pw;
            double ky = (double)h / ren.ph;
            double k = kx > ky ? kx : ky;
            if (k < 1.0) k = 1.0;

            int nearest = (int)(k + 0.5);
            if (nearest < 1) nearest = 1;
            double diff = k - nearest;
            if (diff < 0) diff = -diff;

            if (diff <= 0.1) {
                k = nearest;
                render_set_scale_mode(&ren, SDL_ScaleModeNearest);
            } else {
                render_set_scale_mode(&ren, SDL_ScaleModeLinear);
            }

            int nw = (int)(ren.pw * k + 0.5);
            int nh = (int)(ren.ph * k + 0.5);
            if (nw != w || nh != h) SDL_SetWindowSize(ren.win, nw, nh);
            scr.dirty = 1;
            resize_pending = 0;
        }

        if (child_alive) {
            if (rt_child_alive(&proc)) {
                int r = rt_poll_readable(&proc, 10);
                if (r == 1) {
                    unsigned char rbuf[4096];
                    int n, reads = 0;
                    // drain what's waiting, but cap it so a chatty child
                    // (cat on a big file, say) can't starve the UI loop
                    while ((n = rt_read(&proc, rbuf, sizeof(rbuf))) > 0) {
                        if (rawlog) { fwrite(rbuf, 1, (size_t)n, rawlog); fflush(rawlog); }
                        vt_process(&scr, &vp, &sixel, rbuf, n);
                        reads++;
                        if (n < (int)sizeof(rbuf) || reads > 16) break;
                    }
                    if (n < 0) child_alive = 0;
                }
            } else {
                child_alive = 0;
            }
        } else if (!wait_prompted) {
            // reuse the vt pipeline to paint the exit prompt, rather than
            // poking cells directly - keeps this one code path for text
            char msg[96];
            sprintf(msg, "\r\n\x1B[7m Press any key to close... (exit code %d) \x1B[0m",
                    proc.exit_code);
            if (wait_flag) {
                vt_process(&scr, &vp, &sixel, (const unsigned char *)msg, (int)strlen(msg));
            } else {
                running = 0;
            }
            wait_prompted = 1;
        }

        // SDL tracks capslock as toggle state, not a press/release
        // event we can just forward - poll it and force a status
        // refresh on change so it doesn't wait for some unrelated
        // redraw to catch up.
        int caps = (SDL_GetModState() & KMOD_CAPS) ? 1 : 0;
        if (caps != last_caps) {
            last_caps = caps;
            scr.dirty = 1;
        }

        // a bell with nothing else to redraw still needs a fresh frame -
        // render_flash inverts whatever's currently in the framebuffer,
        // so it has to be this tick's frame, not a stale one
        if (vp.bell) scr.dirty = 1;

        if (running && scr.dirty) {
            char status_right[64];
            sprintf(status_right, "Row:%02d Col:%02d %s ", scr.cur_y + 1, scr.cur_x + 1,
                    caps ? "CAPS" : "    ");
            scr_set_status(&scr, status_left, status_right);
            render_frame(&ren, &scr, &sixel);
            scr.dirty = 0;
        }

        if (vp.bell) {
            render_flash(&ren);
            vp.bell = 0;
        }
    }

    if (rawlog) fclose(rawlog);
    SDL_StopTextInput();
    render_destroy(&ren);
    vt_free(&vp);
    sixel_free(&sixel);
    scr_free(&scr);
    rt_cleanup(&proc);
    free(child_argv);
    SDL_Quit();

    return 0;
}
