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

# Sailfish OS package (aarch64, for the Jolla Phone): the same binary, plus a user
# service, a page in the Settings app, a Top menu toggle and an app grid icon that opens
# the page. Needs rpm and rsvg-convert (librsvg2-bin) in the container.
VERSION = 0.6
RPMDIR = $(CURDIR)/rpm
rpm: shake-torch sailfish/shake-torch.spec sailfish/shake-torch.service sailfish/shake-torch.json sailfish/shake-torch.qml sailfish/ShakeTorchSwitch.qml sailfish/shake-torch.desktop shake-torch.svg
	rm -rf $(RPMDIR) && mkdir -p $(RPMDIR)/SOURCES
	$(CROSS)strip -o $(RPMDIR)/SOURCES/shake-torch shake-torch
	cp sailfish/shake-torch.service sailfish/shake-torch.json sailfish/shake-torch.qml sailfish/ShakeTorchSwitch.qml sailfish/shake-torch.desktop $(RPMDIR)/SOURCES/
	for size in 86 108 128 172; do rsvg-convert -w $$size -h $$size -o $(RPMDIR)/SOURCES/shake-torch-$$size.png shake-torch.svg; done
	rpmbuild -bb --target aarch64 --define '_topdir $(RPMDIR)' --define '_userunitdir /usr/lib/systemd/user' \
		--define 'version $(VERSION)' --define '_bindir /usr/bin' --define '_datadir /usr/share' \
		sailfish/shake-torch.spec
	mv $(RPMDIR)/RPMS/aarch64/*.rpm . && rm -rf $(RPMDIR)

clean:
	rm -f shake-torch *.rpm
.PHONY: clean rpm
.PHONY: clean
