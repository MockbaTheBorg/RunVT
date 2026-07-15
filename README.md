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

This produces a single `runvt.exe` - SDL2 is statically linked in, so
there's no `SDL2.dll` to carry around. Just copy the exe over.

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

## Embedding in RunCPM (no PTY at all)

This started as a Windows bug: sixel graphics and DEC Special Graphics
box-drawing worked fine on Linux but silently didn't on Windows. Turned
out ConPTY hosts a real hidden `conhost.exe` that parses everything the
child writes, then hands RunVT a *reserialized* stream based on its own
internal screen-buffer model - not the raw bytes. Anything conhost's
parser doesn't know about (sixel DCS, some charset designations) just
gets dropped before RunVT ever sees it. There's no "passthrough mode" to
turn that off - it's how ConPTY works.

The fix ended up being bigger than a workaround: link RunVT directly
into RunCPM instead of spawning it as a child over a pty at all. One
executable, RunCPM's console I/O calls go straight into RunVT's
screen/vtparser/render core as plain function calls in the same
process. No pty, pipe, or ConPTY in between, so there's nothing left to
reinterpret the stream - and since it's just function calls either way,
it works identically on Linux and Windows.

The reusable core (`runvt_core.h`) is what makes this possible - it's
the same screen/parser/render/event-pump code the standalone `runvt`
binary uses, just without anything that assumes a child process exists.

This needs a handful of small changes on RunCPM's own side too (not
part of this repo, since RunVT doesn't touch RunCPM's source directly):

- `globals.h` gets a `RUNVT_EMBED` flag, same pattern as the existing
  `STREAMIO` flag - passed externally with `-DRUNVT_EMBED`.
- `abstraction_posix.h` / `abstraction_windows.h` get their console
  block (just `_console_init`/`_console_reset`/`_kbhit`/`_getch`/
  `_putch`/`_getche`/`_clrscr`) wrapped in `#ifndef RUNVT_EMBED`. Disk,
  host init, and hardware functions are left alone.
- `main.c` gets one more branch next to the existing `_WIN32`/posix
  check, picking a new `abstraction_runvt.h` when `RUNVT_EMBED` is set.
- `abstraction_runvt.h` (new file) implements those same console
  primitives on top of `runvt_core.h`. It's also where the SDL event
  pump happens - inside `_putch`/`_kbhit`/`_getch`/`_clrscr`, since
  every console byte in or out passes through one of those regardless
  of whether the running program used a BIOS or BDOS call. Turned out
  to be a reliable pump point without needing to touch `cpm.h` or any
  of the `cpu*.h` CPU cores. On Windows it also has to dodge conio.h's
  own `_getch`/`_putch`/`_getche` declarations - different signatures
  than the ones defined here, so a real compiler error rather than
  just a redeclaration warning - by renaming them out of the way for
  the duration of the `abstraction_windows.h` include.
- A new `Makefile.runvt`, registered as a `runvt` platform in the top
  `Makefile`. There's also a `Makefile.runvtwin` (`runvtwin` platform)
  that cross-compiles the same embed into a Windows `.exe` from Linux
  via mingw-w64, statically linked the same way RunVT's own `make
  windows` target is - handy for testing the Windows build without a
  Windows box.

### Building (Linux, or Windows via MSYS2/mingw)

RunCPM's repo nests a `RunCPM/RunCPM/` folder - same reason the Arduino
sketch needs its `.ino` sitting in a same-named folder. That inner one
is where `main.c`, `Makefile.runvt`, and the other platform Makefiles
actually live, and it's where `make runvt build` needs to run from.
Clone RunVT in there:

```
RunCPM/                <- outer repo folder
  RunCPM/               <- inner folder: main.c, Makefile, Makefile.runvt, ...
    RunVT/               <- clone RunVT here
      src/
    main.c
    Makefile.runvt
    ...
```

Then, from that inner `RunCPM/RunCPM/` folder:

```
make runvt build
```

Same `CCP=`/`CPU=`/`CPM3=` overrides as the other platforms work here
too, e.g. `make runvt CPU=cpu2 build`. If RunVT is cloned somewhere else
instead, point `RUNVT_DIR` at it (relative to that same inner folder, or
absolute):

```
make runvt build RUNVT_DIR=/path/to/RunVT
```

This needs SDL2's dev package on the include/link path (`pkg-config
sdl2` must resolve) - same requirement as building `runvt` itself, see
**Building** above. Bare Windows mingw toolchains without MSYS2 often
don't have `pkg-config` at all, though - if that's you, point
`SDL2DIR` at wherever SDL2's `include`/`lib` folders live instead and
it skips pkg-config entirely:

```
mingw32-make -f Makefile.runvt SDL2DIR=C:/mingw64/x86_64-w64-mingw32 build
```

To cross-compile a Windows `.exe` from Linux instead of building
natively, use `runvtwin` in place of `runvt` - same folder layout,
same `RUNVT_DIR`/`CCP`/`CPU`/`CPM3` overrides, just needs SDL2's
mingw-w64 dev package installed in the mingw sysroot (see **Building
for Windows** above):

```
make runvtwin build
```

### Building with Visual Studio

RunCPM's `.vcxproj` doesn't have a configuration for this wired in -
neither does `STREAMIO`, for what it's worth, so this isn't a new gap.
Both are meant to be turned on by hand when you actually want them. To
build RunCPM with RunVT embedded from Visual Studio:

1. Clone RunVT somewhere reachable from the project (e.g. into
   RunCPM's folder).
2. Project Properties → C/C++ → Preprocessor → **Preprocessor
   Definitions**: add `RUNVT_EMBED`.
3. Project Properties → C/C++ → General → **Additional Include
   Directories**: add the path to RunVT's `src` folder.
4. Project Properties → Linker → General → **Additional Library
   Directories**: add SDL2's `lib\x64` (or `x86`) folder from the
   [SDL2 development package](https://github.com/libsdl-org/SDL/releases)
   (VC variant).
5. Project Properties → Linker → Input → **Additional Dependencies**:
   add `SDL2.lib;SDL2main.lib`.
6. Copy `SDL2.dll` next to the built executable (or add a post-build
   step to do it) - this VS route links SDL2 dynamically, unlike the
   Makefile route which links it statically.

No source changes needed - `main.c`'s existing `#ifdef RUNVT_EMBED`
branch picks up `abstraction_runvt.h` once the define is set.

### Notes

- Needs a real display/window session - it won't run headless. Same
  requirement standalone RunVT always had, just easier to forget here
  since there's no separate terminal window around to make it obvious.
- First pass at this pumped and redrew on every single character out,
  and console text got painfully slow - turns out `render.h` presents
  with vsync on, so a redraw per byte caps output at the monitor's
  refresh rate, a handful of characters a frame at best. Fixed by
  time-gating the actual redraw to roughly 15ms; the screen buffer
  itself still updates immediately per character, only the on-screen
  redraw gets batched.
- Clicking the window's close button ends the process immediately,
  right from inside the SDL pump - it doesn't wait for RunCPM's own
  Status flag to unwind the Z80 core gracefully. Tried that first and
  it didn't work reliably: cpu1-4.h dispatch opcodes via computed
  goto, so a program idling in a tight console-poll loop (the CCP at
  its prompt, most of the time) can go a long while without ever
  rechecking Status. The old pty setup never had this problem since
  closing the window just killed the child process outright - there's
  no separate process here to kill out from under it, so this just
  exits directly instead.

## Usage

```
runvt [--wait] [--codepage=latin1|cp850] [--size=COLSxROWS] [--zoom=N.N]
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
- `--zoom=N.N` - start the window at N times the native pixel size, e.g.
  `--zoom=2` or `--zoom=1.5`. Just picks the starting size - the window's
  still free to be dragged to any size afterward, same as if you'd
  landed there by hand. A fractional zoom starts blurred (linear
  filtering) same as a live resize does at a non-whole scale; close
  enough to a whole number (within 0.1) and it starts crisp instead.
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
runvt --zoom=2 ./RunCPM
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
| `runvt_core.h` | The reusable terminal core (screen/sixel/parser/render/event pump) - what `main.c` and RunCPM's embedded build both drive |
| `main.c` | Argument parsing, spawning the child over a pty, and the standalone event loop |

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
