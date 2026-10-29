CC ?= cc
CSTD = -std=c11
OPTFLAGS ?= -Os -ffunction-sections -fdata-sections
CFLAGS ?= -Wall -Wextra -pedantic
LDFLAGS ?=

TINY = -flto -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
TINY_LINK = -Wl,--gc-sections -Wl,--build-id=none -s

# version from the latest git tag, if any
VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null)

# install under ~/.local, or /usr/local as root
ifeq ($(shell id -u),0)
BINDIR ?= /usr/local/bin
SHAREDIR ?= /usr/local/share
else
BINDIR ?= $(HOME)/.local/bin
SHAREDIR ?= $(HOME)/.local/share
endif

all: oshi
	@echo "  install: make install        (you,     ~/.local/bin)"
	@echo "           sudo make install   (system,  /usr/local/bin)"

# the default build is already the release build
oshi: oshi.c
	$(CC) $(CSTD) $(OPTFLAGS) $(TINY) $(CFLAGS) $(LDFLAGS) $(TINY_LINK) \
	  $(if $(VERSION),-DOSHI_VERSION=\"$(VERSION)\") -o $@ oshi.c

release: oshi

# refresh desktop and icon caches when available
install: oshi Oshi.desktop Oshi.svg
	mkdir -p $(DESTDIR)$(BINDIR) \
	  $(DESTDIR)$(SHAREDIR)/applications \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps
	install -m755 oshi $(DESTDIR)$(BINDIR)/oshi
	install -m644 Oshi.desktop $(DESTDIR)$(SHAREDIR)/applications/Oshi.desktop
	install -m644 Oshi.svg $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/Oshi.svg
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && \
	  gtk-update-icon-cache -f -t $(DESTDIR)$(SHAREDIR)/icons/hicolor 2>/dev/null; true

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/oshi $(DESTDIR)$(BINDIR)/Oshi \
	  $(DESTDIR)$(SHAREDIR)/applications/Oshi.desktop \
	  $(DESTDIR)$(SHAREDIR)/applications/oshi.desktop \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/Oshi.svg \
	  $(DESTDIR)$(SHAREDIR)/icons/hicolor/scalable/apps/oshi.svg
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(DESTDIR)$(SHAREDIR)/applications 2>/dev/null; true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && \
	  gtk-update-icon-cache -f -t $(DESTDIR)$(SHAREDIR)/icons/hicolor 2>/dev/null; true

clean:
	rm -f oshi Oshi

.PHONY: all release clean install uninstall
