OBS_SDK ?= /tmp/obs-studio-32.2.2-sdk/libobs
OBS_APP ?= /Applications/OBS.app
SIMDE_INC ?= /tmp/is1obs-simde
PLUGIN = build/is1obs.plugin
BINARY = $(PLUGIN)/Contents/MacOS/is1obs

.PHONY: all clean
all: $(BINARY)

$(BINARY): is1obs.c obsconfig.h Info.plist
	mkdir -p $(PLUGIN)/Contents/MacOS
	cp Info.plist $(PLUGIN)/Contents/Info.plist
	clang -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 \
		-I. -I$(OBS_SDK) -I$(SIMDE_INC) -F$(OBS_APP)/Contents/Frameworks \
		-bundle -framework libobs \
		-Wl,-rpath,@executable_path/../Frameworks \
		-o $(BINARY) is1obs.c
	codesign --force --sign - $(PLUGIN)

clean:
	rm -f $(BINARY)
