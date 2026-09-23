# is1view - watch the IntelliStar's output in a window on the host.
#
# Needs SDL2 and its pkg-config file (libsdl2-dev on Debian).
#
#   make
#   ./is1view ~/i1/is1-output
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2
CFLAGS     += $(shell $(PKG_CONFIG) --cflags sdl2)
LDLIBS      = $(shell $(PKG_CONFIG) --libs sdl2)

is1view: is1view.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f is1view

.PHONY: clean
