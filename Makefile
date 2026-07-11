CC      = cc
CFLAGS  = -std=c99 -pedantic -Wall -Wextra -O2 -D_DEFAULT_SOURCE $(shell pkg-config --cflags sdl2)
LDFLAGS = $(shell pkg-config --libs sdl2) -lutil

SRC     = src/main.c
BIN     = RunVT

# Windows cross-build via MinGW-w64. Needs SDL2's official mingw devel
# package installed into the mingw sysroot (see README) - regular apt
# SDL2 packages are Linux-only and won't show up here.
#
# Statically linked: we link libSDL2.a directly by path (not -lSDL2,
# which the linker would resolve to the dynamic import lib instead) and
# pull in sdl2.pc's Libs.private - the Windows system libs SDL2's own
# DLL build normally hides internally, needed explicitly once SDL2 is
# static. The only DLLs RunVT.exe ends up depending on are ones every
# Windows install already has (kernel32, user32, gdi32, etc) - no SDL2.dll
# needs to travel with it.
WINCC        = x86_64-w64-mingw32-gcc
WINSYSROOT   = /usr/x86_64-w64-mingw32
WIN_PKGENV   = PKG_CONFIG_LIBDIR=$(WINSYSROOT)/lib/pkgconfig PKG_CONFIG_PATH=
WINCFLAGS    = -std=c99 -pedantic -Wall -Wextra -O2 $(shell $(WIN_PKGENV) pkg-config --cflags sdl2)
WINSDL2LIB   = $(WINSYSROOT)/lib/libSDL2.a
# from sdl2.pc's Libs.private - the system libs SDL2's own DLL build
# normally pulls in internally, needed explicitly once SDL2 is static
WINSYSLIBS   = -lm -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm \
               -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid
WINLDFLAGS   = -L$(WINSYSROOT)/lib -static -static-libgcc -lmingw32 -lSDL2main \
               $(WINSDL2LIB) -mwindows $(WINSYSLIBS)
WINBIN       = RunVT.exe

.PHONY: all clean windows

all: $(BIN)

$(BIN): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

windows: $(WINBIN)

$(WINBIN): $(SRC) src/*.h
	$(WINCC) $(WINCFLAGS) -o $(WINBIN) $(SRC) $(WINLDFLAGS)

clean:
	rm -f $(BIN) $(WINBIN) SDL2.dll
