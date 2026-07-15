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
//
// The terminal core itself (screen grid, sixel overlay, VT parser, SDL
// render/event pump) lives in runvt_core.h - reusable by anything that
// wants to drive it without a child process, e.g. RunCPM linked
// directly against RunVT instead of talking to it over a pty.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "runvt_core.h"

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
        "usage: %s [--wait] [--codepage=latin1|cp850] [--size=COLSxROWS] [--zoom=N.N]\n"
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
    double zoom = 1.0;
    int codepage = CODEPAGE_LATIN1;
    const char *log_path = NULL;
    FILE *rawlog = NULL;

    RunVT vt;
    RT_Process proc;

    int running = 1;
    int child_alive = 1;
    int wait_prompted = 0;
    char title[256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wait") == 0) {
            wait_flag = 1;
        } else if (strncmp(argv[i], "--codepage=", 11) == 0) {
            const char *v = argv[i] + 11;
            codepage = (strcmp(v, "cp850") == 0) ? CODEPAGE_CP850 : CODEPAGE_LATIN1;
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
        } else if (strncmp(argv[i], "--zoom=", 7) == 0) {
            double z;
            if (sscanf(argv[i] + 7, "%lf", &z) == 1 && z >= 1.0) {
                zoom = z;
            } else {
                fprintf(stderr, "%s: --zoom wants a number >= 1, e.g. 1.5 or 2\n", argv[0]);
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

    if (rt_spawn(&proc, child_argv, cols, rows) != 0) {
        fprintf(stderr, "%s: failed to launch '%s'\n", argv[0], child_argv[0]);
        return 1;
    }

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

    RunVTConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.zoom = zoom;
    cfg.title = title;
    cfg.codepage = codepage;
    cfg.input_cb = pty_reply;
    cfg.input_ctx = &proc;

    if (rt_core_init(&vt, &cfg) != 0) {
        fprintf(stderr, "%s: failed to initialize SDL renderer\n", argv[0]);
        return 1;
    }

    while (running) {
        if (rt_core_pump(&vt)) {
            running = 0;
            break;
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
                        rt_core_feed(&vt, rbuf, n);
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
                rt_core_feed(&vt, (const unsigned char *)msg, (int)strlen(msg));
                rt_core_set_input_enabled(&vt, 0);
            } else {
                running = 0;
            }
            wait_prompted = 1;
        }

        rt_core_maybe_render(&vt);
    }

    if (rawlog) fclose(rawlog);
    rt_core_shutdown(&vt);
    rt_cleanup(&proc);
    free(child_argv);
    SDL_Quit();

    return 0;
}
