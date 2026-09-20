# is1ndi - NDI bridge for the virtual IntelliStar.
#
# The NDI SDK is NOT vendored into this repository: it is proprietary, and
# DistroAV's history with Vizrt is a good reason not to inherit that argument.
# Point NDI_SDK at an installed SDK instead. Download it from
# https://downloads.ndi.tv/SDK/NDI_SDK_Linux/ - the installer asks you to
# accept Vizrt's licence, which is between you and them.
#
#   make NDI_SDK="$HOME/NDI SDK for Linux"
#
# The runtime is dlopen()ed at startup, not linked, so this builds against the
# headers alone and runs - reporting the fact - when NDI is not installed.
NDI_SDK ?= $(HOME)/NDI SDK for Linux
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -ldl -lpthread -lm

is1ndi: is1ndi.c
	$(CC) $(CFLAGS) -I"$(NDI_SDK)/include" -o $@ $< $(LDLIBS)

clean:
	rm -f is1ndi

.PHONY: clean
