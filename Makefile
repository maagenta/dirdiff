# DirDiff - Makefile for Linux (generic SWELL on GDK 3)
#
# Dependencies (Debian/Ubuntu):
#   sudo apt install build-essential pkg-config perl libgtk-3-dev libxi-dev \
#                    libgl-dev libfreetype-dev libfontconfig-dev
#
# Usage: make            -> ./dirdiff
#        make DEBUG=1
#        make clean
#
# WDL is a git submodule in WDL/ (https://github.com/justinfrankel/WDL);
# the sources are in WDL/WDL/. After cloning: git submodule update --init

APPNAME = dirdiff
WDL_PATH = WDL/WDL

vpath swell%.cpp $(WDL_PATH)/swell
vpath lice%.cpp $(WDL_PATH)/lice
vpath %.cpp $(WDL_PATH) $(WDL_PATH)/wingui

SWELL_OBJS = swell.o swell-ini.o swell-miscdlg-generic.o swell-wnd-generic.o \
             swell-menu-generic.o swell-kb-generic.o swell-dlg-generic.o \
             swell-gdi-generic.o swell-misc-generic.o swell-gdi-lice.o \
             swell-generic-gdk.o swell-appstub-generic.o
LICE_OBJS = lice_image.o lice_arc.o lice_line.o lice_text.o lice_textnew.o \
            lice.o lice_colorspace.o
OTHER_OBJS = wndsize.o
APP_OBJS = main.o diffengine.o
OBJS = $(APP_OBJS) $(SWELL_OBJS) $(LICE_OBJS) $(OTHER_OBJS)

RESFILES = dirdiff.rc_mac_dlg dirdiff.rc_mac_menu

PKG_CONFIG ?= pkg-config
ARCH := $(shell uname -m)

CFLAGS += -pipe -fvisibility=hidden -fno-math-errno -fPIC -DPIC -D_FILE_OFFSET_BITS=64 \
          -Wall -Wno-unused-function -Wno-multichar -Wno-unused-result
# the sources include "../WDL/..."; with this directory it resolves to WDL/WDL/...
CFLAGS += -I$(WDL_PATH)
CFLAGS += -DSWELL_TARGET_GDK=3 -DSWELL_LICE_GDI -DSWELL_FREETYPE -DSWELL_FONTCONFIG
CFLAGS += $(shell $(PKG_CONFIG) --cflags gdk-3.0 freetype2 fontconfig)

# WDL assumes signed char (e.g. when sorting UTF-8 strings)
ifneq ($(filter arm% aarch64,$(ARCH)),)
  CFLAGS += -fsigned-char
endif

ifdef DEBUG
  CFLAGS += -O0 -g -D_DEBUG
else
  CFLAGS += -O2 -DNDEBUG
endif

CXXFLAGS = $(CFLAGS)
LINKEXTRA = -lpthread -ldl $(shell $(PKG_CONFIG) --libs gdk-3.0 freetype2 fontconfig) -lX11 -lXi -lGL

# strict warnings only for the application code (not for WDL)
$(APP_OBJS): CXXFLAGS += -Wextra -Wshadow -Wno-unused-parameter

all: $(APPNAME)

$(RESFILES): dirdiff.rc
	perl $(WDL_PATH)/swell/swell_resgen.pl dirdiff.rc

main.o: main.cpp $(RESFILES) resource.h diffengine.h
diffengine.o: diffengine.cpp diffengine.h

$(APPNAME): $(OBJS)
	$(CXX) -o $@ $(CXXFLAGS) $(OBJS) $(LINKEXTRA)

run: $(APPNAME)
	./$(APPNAME)

clean:
	-rm -f $(OBJS) $(APPNAME)

.PHONY: all run clean
