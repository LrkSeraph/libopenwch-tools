##
## libopenwch-tools — host build
##
## This is a *host* program: it runs on the PC and talks to a WCH-LinkE over
## USB.  It is deliberately not part of libopenwch, whose build is RISC-V-only.
##
## libusb is found through pkg-config when possible.  On a host without the
## -dev package -- or with libusb installed outside pkg-config's search path --
## override both variables instead:
##
##   make LIBUSB_CFLAGS=-I/path/to/include LIBUSB_LIBS=-l:libusb-1.0.so.0
##
## The second form is useful when only the runtime library is present: libusb
## ships libusb-1.0.so.0 in the runtime package and libusb-1.0.so only in the
## -dev package.
##

CC		?= cc
PKG_CONFIG	?= pkg-config

CFLAGS		?= -O2 -g
CFLAGS		+= -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
		   -Wmissing-prototypes -Wredundant-decls

LIBUSB_CFLAGS	?= $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS	?= $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

## pkg-config may know nothing about libusb; fall back to the plain name so
## the failure is a clear linker error rather than a silent empty link line.
ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_LIBS	:= -lusb-1.0
endif

CPPFLAGS	+= $(LIBUSB_CFLAGS)
LDLIBS		+= $(LIBUSB_LIBS)

BUILD_DIR	?= build
PROJECT		= wchlink

## The binary lives under build/ alongside the objects.  Its consumers look for
## it there: libopenwch-template carries this repository as the
## tools/wchlink submodule and runs tools/wchlink/build/wchlink.
TARGET		= $(BUILD_DIR)/$(PROJECT)

SRCS		= src/main.c src/log.c src/target.c src/usb.c src/usb_link.c \
		  src/linke.c src/dm.c src/gdb_server.c src/flash.c \
		  src/flash_ch32v0.c
OBJS		= $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
DEPS		= $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	@printf "  LD      $@\n"
	$(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c
	@printf "  CC      $<\n"
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

## Host-side unit tests.  No hardware: these cover the parts that are pure
## logic (the chip table and the command line), so they run anywhere.
test: $(TARGET)
	@printf "  TEST    tests/\n"
	$(Q)$(MAKE) -C tests run PROJECT=$(abspath $(TARGET))

install: $(TARGET)
	$(Q)install -D -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(PROJECT)
	$(Q)install -D -m 0644 udev/99-wchlink.rules \
		$(DESTDIR)$(PREFIX)/lib/udev/rules.d/99-wchlink.rules

## udev rules are what let an unprivileged user talk to the programmer.
install-udev-rules:
	$(Q)install -m 0644 udev/99-wchlink.rules /etc/udev/rules.d/99-wchlink.rules
	$(Q)udevadm control --reload-rules
	$(Q)udevadm trigger
	@printf "  UDEV    installed; replug the programmer\n"
	@printf "  NOTE    if TAG+=uaccess is not available, add yourself to plugdev\n"
	@printf "          and start a new login session; do not run wchlink with sudo\n"

clean:
	$(Q)$(RM) -r $(BUILD_DIR)
	$(Q)$(MAKE) -C tests clean

PREFIX		?= /usr/local
V		?= 0

## Be silent by default; `make V=1` shows the full command lines.
ifneq ($(V),1)
Q		:= @
endif

-include $(DEPS)

.PHONY: all test install install-udev-rules clean
