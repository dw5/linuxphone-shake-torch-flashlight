# Build for the phone from the furilabs container: make (arm64, FuriOS).
# CROSS= and PKG_CONFIG_LIBDIR= override for a native or another sysroot build.
CROSS ?= aarch64-linux-gnu-
CC = $(CROSS)gcc
PKG_CONFIG_LIBDIR ?= /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
PKGS = gio-unix-2.0
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell PKG_CONFIG_LIBDIR=$(PKG_CONFIG_LIBDIR) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_LIBDIR=$(PKG_CONFIG_LIBDIR) pkg-config --libs $(PKGS)) -lm

shake-torch: shake-torch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f shake-torch
.PHONY: clean
