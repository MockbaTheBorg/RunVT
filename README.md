# RunVT

A small, simple VT100/ANSI terminal emulator with its own SDL2 window. I
built it as a console for [RunCPM](https://github.com/MockbaTheBorg/RunCPM),
my CP/M emulator, but it doesn't know anything about CP/M specifically - it
just spawns whatever program you give it on a pty and becomes its terminal.
Run RunCPM in it, run `bash` in it, run `vi` in it. If it is able to talk
VT100/ANSI over a pty, RunVT should be able to host it.

## Why this exists

RunCPM normally runs inside whatever terminal you already have open, which
means its VT100-flavored screen handling is at the mercy of your terminal's
config, your `TERM` setting, your font, whatever. I wanted something
purpose-built: a fixed-size, predictable, minimalistic VT100/ANSI
screen with a real bitmap font, that opens as its own window and does one
job. No scrollback, no tabs, no smooth scrolling, no configuration files.
Just spawn a program and be its terminal.

## What it does

- **80x25 by default** (or any size you want, see `--size` below), rendered
  with an embedded 8x16 bitmap font at native pixel resolution - the window
  is exactly `cols*8` by `(rows+1)*16` pixels, no scaling.
- **A status bar** along the bottom, always there, showing the app name,
  RunVT's version, and the cursor's current row/col and Caps Lock state.
  It's RunVT's own overlay, not part of the addressable screen the app
  writes to - same idea as a VT220's status line, just simpler (not
  host-writable; nobody running CP/M software needs DECSASD).
- **A real VT100/ANSI control sequence parser**: cursor movement, scrolling
  regions, insert/delete character and line, save/restore cursor (both the
  real VT100 `ESC 7`/`8` form and the ANSI.SYS `CSI s`/`u` form), SGR colors
  and attributes, DEC Special Graphics line-drawing, device status/attribute
  queries (so apps that ask "where's the cursor" or "what terminal is this"
  get an answer instead of hanging).
- **Two codepages** for the upper 128 characters: DEC Multinational
  (≈ISO-8859-1, the default) and CP850, picked with `--codepage`. Covers
  accented Latin characters properly - I'm Brazilian, so ã and ç need to work.
- **Sixel graphics** - real DEC/VT240 bitmap graphics, positioned wherever
  the cursor is (just like text), composited with the text layer, and
  scrolled along with everything else when the screen scrolls.
- **Keyboard passthrough**: arrows, function keys (PF1-4), Ctrl combos, and
  proper UTF-8-to-codepage encoding so typing accented characters on your
  keyboard sends the right byte to the app.
- **Clipboard paste** - Ctrl+Shift+V pastes the system clipboard, run
  through the same UTF-8-to-codepage path as typed keys.
- **Window title updates** - apps that send the OSC title sequence (vim,
  tmux, plenty of shells) get to rename the window, same as any other
  terminal would let them.
- **Bell** - BEL fills the window with a solid color for a beat instead
  of doing nothing. Color's configurable (`--bell`).
- **Customizable colors** - `--normal`/`--bold` override the default
  text color (the 16-color SGR palette itself stays put).

## What it deliberately doesn't do

This is meant to be simple and predictable, not a feature-complete xterm
clone:

- No window resizing - size is fixed at launch via `--size`.
- No scrollback buffer.
- No alternate screen buffer (`smcup`/`rmcup`) - it's a single screen the
  whole time, matching plain `TERM=vt100` semantics.
- No ReGIS graphics (sixel only) - ReGIS is a whole vector graphics
  language and would roughly double this project's size for a feature
  almost nothing still emits.
- No host-writable status line (DECSASD) - the status bar is RunVT's own,
  not addressable by the app.

## Building

You need a C compiler and SDL2's development package (`libsdl2-dev` on
Debian/Ubuntu). Then:

```
make
```

That's it - one binary, `runvt`, no install step. The build is strict C99
(`-std=c99 -pedantic`), and the only OS-specific code lives in
`abstraction_posix.h` / `abstraction_windows.h` - see **Architecture**
below if you're porting it somewhere.

### Building for Windows

The Windows build (`abstraction_windows.h`, ConPTY-based) is cross-compiled
from Linux with MinGW-w64 - I don't have a native Windows toolchain, and
honestly this turned out to be less painful than expected. You need:

- `gcc-mingw-w64-x86-64` (Ubuntu/Debian package name) for the cross-compiler.
- SDL2's official **mingw** devel package (not the regular `libsdl2-dev` -
  that's Linux-only). Grab `SDL2-devel-<version>-mingw.tar.gz` from
  [SDL's GitHub releases](https://github.com/libsdl-org/SDL/releases) and
  install it into the mingw sysroot:
  ```
  tar xzf SDL2-devel-2.32.10-mingw.tar.gz
  cd SDL2-2.32.10
  sudo make install-package arch=x86_64-w64-mingw32 prefix=/usr/x86_64-w64-mingw32
  ```

Then:

```
make windows
```

This produces `runvt.exe` and copies `SDL2.dll` alongside it (dynamic
linking against SDL2, so that DLL has to travel with the exe). Both files
need to sit in the same folder on the Windows side.

First tried testing this under Wine, since it saves a trip to an actual
Windows box - turned out to be a dead end. Wine's ConPTY support is
incomplete (the child's console output doesn't get routed through the
pipes the way it should), so `runvt.exe` under Wine just opens a blank
window and the child appears to exit instantly. Went and tested on a real
Windows 10 VM instead - launched `cmd.exe` inside it and ran a few
commands (`dir`, `cls`), all worked as expected. So: if you're
cross-compiling and want a sanity check, don't bother with Wine for this
one, it'll give you a false negative. Test on real Windows (10, build
1809+, or 11).

## Usage

```
runvt [--wait] [--codepage=latin1|cp850] [--size=COLSxROWS]
      [--bell=0xRRGGBB] [--normal=0xRRGGBB] [--bold=0xRRGGBB]
      [--log=FILE] app [args...]
```

- `app [args...]` - the program to run, and its arguments. Required.
- `--wait` - after the app exits, show "Press any key to close... (exit
  code N)" instead of closing the window immediately. Handy for
  double-click-launched use.
- `--codepage=latin1|cp850` - which table backs the upper 128 characters.
  Defaults to `latin1`. Doesn't live-switch mid-session (real apps don't
  send codepage-switch sequences, they just assume the terminal is already
  configured) - pick it at launch to match what the app expects.
- `--size=COLSxROWS` - screen size, e.g. `--size=132x43`. Defaults to
  `80x25`. That's the *addressable* area the app gets; the status bar is
  one extra row on top of that.
- `--bell=0xRRGGBB` - color the visual bell flashes to (see **Bell**
  below). Defaults to `0x505050`, a dim gray - a full white/inverted
  flash turned out to be pretty jarring on a black background.
- `--normal=0xRRGGBB` / `--bold=0xRRGGBB` - override the default text
  color and its bold variant (what text renders as when nothing sets an
  explicit SGR color, or resets with `CSI 0m`/`CSI 39m`). Explicit ANSI
  colors (`CSI 30-37m`, `CSI 90-97m`) are untouched by these - only the
  "no color specified" case changes.
- `--log=FILE` - dumps the raw bytes coming from the child to a file.
  Debugging aid, not something you need for normal use.

Examples:

```
runvt ./RunCPM
runvt --size=132x43 bash
runvt --wait --codepage=cp850 ./RunCPM
runvt --bell=0x301010 --normal=0x00FF00 ./RunCPM
```

### Keyboard

Standard stuff works as you'd expect - typing, arrows, Ctrl+letter sends
the matching control byte, Home/End send `CSI H`/`CSI F`, F1-F4 send the
real VT100 PF1-PF4 codes (`ESC O P/Q/R/S`). Backspace sends `BS` (0x08),
not `DEL` - that's the CP/M-era convention (WordStar and friends expect
it), not what most modern terminals default to. Worth knowing if an app
seems to eat the wrong character on backspace.

Ctrl+Shift+V pastes whatever's on the system clipboard.

### Bell

BEL (`CHR$(7)` from MBASIC, `printf '\a'` from a shell) fills the window
with a solid color for a beat and puts the real screen back - a real
flash, not an OS "attention" hint that most window managers only show as
a barely-there taskbar dot. Color's `--bell`, default a dim gray.

## Testing it

`examples/RUNVT.BAS` is a small MBASIC program that exercises most of
RunVT's features from inside CP/M: the 16-color palette, bold/underline/
reverse, accented characters, DEC Special Graphics box-drawing, and two
sixel images (one placed on its own line, one placed beside text via
direct cursor addressing, to show images aren't limited to sitting at the
start of a line). Drop it on a CP/M drive and run it with `MBASIC RUNVT`.

One CP/M-specific gotcha if you're editing it: CP/M text files use CRLF
line endings and end with a Ctrl-Z (0x1A) marker. A plain Unix-saved
`.BAS` file will make MBASIC choke with a `line buffer overflow` error on
the first line, because its line reader doesn't recognize LF-only breaks
as line boundaries in the first place.

## Architecture

The split follows RunCPM's own convention: OS-specific code is confined to
`abstraction_posix.h` (Linux, `forkpty`-based) and `abstraction_windows.h`
(Windows, ConPTY-based - cross-compiled with MinGW-w64, briefly tested on
real Windows 10). Both expose the same small API (`rt_spawn`, `rt_read`,
`rt_write`, `rt_child_alive`, `rt_cleanup`, `rt_poll_readable`), and
nothing outside those two files touches a file descriptor, pty, or process
handle directly. Everything else is plain, portable C in header files:

| File | What it does |
|---|---|
| `screen.h` | Cell grid, cursor state, scrolling, insert/delete char/line |
| `font.h` + `font_*.h` | Embedded bitmap glyphs and codepage lookup (generated - see below) |
| `vtparser.h` | The VT100/ANSI control sequence state machine |
| `sixel.h` | Sixel graphics decoder and pixel overlay |
| `render.h` | Blits the cell grid + sixel overlay to the SDL2 window |
| `main.c` | Argument parsing, the event loop, keyboard handling |

SDL2 is the one dependency, and it's what keeps the OS-specific surface so
small in the first place - it already abstracts window creation, input,
and rendering across platforms, so RunVT doesn't have to.

`font_latin1.h`, `font_cp850.h`, `font_special_graphics.h`, and
`cp850_encode.h` aren't hand-written - `tools/gen_fonts.py` generates them
from GNU Unifont (glyph bitmaps) and glibc's IBM850 charmap (the CP850
byte mapping). Rerun that script if the font tables ever need to change;
don't hand-edit the generated files.

## Known limitations

- Windows build has only had a quick smoke test on one Windows 10 VM
  (launching `cmd.exe`, running basic commands). It works, but it hasn't
  been through anywhere near the amount of poking the Linux build has -
  treat it as "should work" rather than "thoroughly exercised."
- No UTF-8 decoding on incoming child output. ConPTY (Windows) translates
  whatever a console app writes into a UTF-8 stream, but RunVT treats
  every incoming byte as a single character already in the active
  codepage (Latin-1/CP850) - correct for CP/M/RunCPM, which really does
  emit raw single-byte codepage bytes, but wrong for native Windows
  console apps that output non-ASCII via the Unicode console API. Ran
  into this with `tree /F`: its box-drawing characters arrive as 3-byte
  UTF-8 sequences and get misread as three garbled glyphs instead of one
  character. Fixing it properly means a UTF-8-decode-with-raw-byte-fallback
  step in `vtparser.h`'s incoming-byte handling - real scope, not done yet.
  Plain ASCII output from Windows apps (most console tools most of the
  time) is unaffected.
- A handful of obscure DEC Special Graphics control-picture symbols (the
  HT/FF/CR/LF/VT indicator glyphs, not the box-drawing characters
  themselves) render blank - Unifont doesn't have narrow glyphs for them.
  Rare in practice; real box-drawing (corners, tees, lines) is unaffected.
- HLS-mode sixel colors are approximated via lightness only, not fully
  converted to RGB - most real sixel output uses RGB mode anyway.

## Version

1.1.
