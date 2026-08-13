CC      ?= cc
PREFIX  ?= /usr/local
PKGCONF ?= pkg-config

# Auto-detect neuswc prefix so 'make' works without setting PKG_CONFIG_PATH
_SUDO_HOME   := $(shell if [ -n "$$SUDO_USER" ]; then getent passwd "$$SUDO_USER" | cut -d: -f6; else printf '%s' "$$HOME"; fi)
_NEU_PREFIX  ?= $(_SUDO_HOME)/.local/inis-neu-prefix
_NEU_PC      = $(_NEU_PREFIX)/lib64/pkgconfig
ifneq ($(wildcard $(_NEU_PC)/swc.pc),)
PKG_CONFIG_PATH := $(_NEU_PC)$(if $(PKG_CONFIG_PATH),:$(PKG_CONFIG_PATH))
export PKG_CONFIG_PATH
endif

CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wpedantic -Wmissing-prototypes -Wstrict-prototypes
STD      = -std=c11

PKGS     = swc wayland-server
CPPFLAGS += -DINIS_HAVE_NEUSWC=1 -DINIS_HAVE_SWC=1 \
            $(shell $(PKGCONF) --cflags $(PKGS))
LDLIBS  += $(shell $(PKGCONF) --libs $(PKGS))
LDFLAGS += $(shell $(PKGCONF) --libs-only-L $(PKGS) | sed 's/-L/-Wl,-rpath,/g')

BIN = inis
CTL = inisctl
TEST_LAYOUT = test_layout
SRC = \
	src/main.c \
	src/log.c \
	src/server.c \
	src/backend.c \
	src/backend_swc.c \
	src/window.c \
	src/monitor.c \
	src/workspace.c \
	src/layout.c \
	src/dispatch.c \
	src/bind.c \
	src/config.c \
	src/rules.c \
	src/render.c \
	src/damage.c \
	src/ipc.c

CTL_SRC = src/inisctl.c
OBJ     = $(SRC:.c=.o)
CTL_OBJ = $(CTL_SRC:.c=.o)

all: $(BIN) $(CTL)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(CTL): $(CTL_OBJ)
	$(CC) -o $@ $(CTL_OBJ)

$(TEST_LAYOUT): test_layout.c src/layout.c
	$(CC) $(CFLAGS) $(WARN) $(STD) -I. -Iinclude -Isrc \
	    -o $@ test_layout.c src/layout.c

DEPDIR = .deps
DEPFLAGS = -MMD -MP -MF $(DEPDIR)/$(basename $(notdir $<)).d

.c.o:
	@mkdir -p $(DEPDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(STD) $(DEPFLAGS) -I. -Iinclude -Isrc -c -o $@ $<

-include $(wildcard $(DEPDIR)/*.d)

clean:
	rm -f $(BIN) $(CTL) $(TEST_LAYOUT) $(OBJ) $(CTL_OBJ)
	rm -rf $(DEPDIR)

SWC_LAUNCH_SRC ?= $(shell $(PKGCONF) --variable=prefix swc 2>/dev/null)/bin/swc-launch

install: $(BIN) $(CTL)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	cp -f $(CTL) $(DESTDIR)$(PREFIX)/bin/$(CTL)
	cp -f inis-session $(DESTDIR)$(PREFIX)/bin/inis-session
	chmod 755 $(DESTDIR)$(PREFIX)/bin/inis-session
	mkdir -p $(DESTDIR)/usr/share/wayland-sessions
	cp -f inis.desktop $(DESTDIR)/usr/share/wayland-sessions/inis.desktop

install-launch:
	@if [ ! -f "$(SWC_LAUNCH_SRC)" ]; then \
	    echo "error: swc-launch not found at $(SWC_LAUNCH_SRC)"; \
	    echo "       set SWC_LAUNCH_SRC=<path> or check PKG_CONFIG_PATH"; \
	    exit 1; fi
	install -o root -m 4755 "$(SWC_LAUNCH_SRC)" $(DESTDIR)$(PREFIX)/bin/swc-launch

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(CTL)
	rm -f $(DESTDIR)$(PREFIX)/bin/inis-session
	rm -f $(DESTDIR)$(PREFIX)/bin/swc-launch
	rm -f $(DESTDIR)/usr/share/wayland-sessions/inis.desktop

.PHONY: all clean install install-launch uninstall
