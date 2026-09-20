/*
 * This file is part of the libopenwch-tools project.
 *
 * Copyright (C) 2025 libopenwch contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WCHLINK_USB_H
#define WCHLINK_USB_H

#include <stddef.h>
#include <stdint.h>

#include <stdbool.h>

/*
 * libusb's header lives in <libusb-1.0/libusb.h> when the -dev package is
 * installed, but on a host that only has the runtime library it may have to be
 * supplied another way -- see the LIBUSB_CFLAGS note in the Makefile.  Accept
 * both spellings so the build does not care which one is in reach.
 */
#if defined(__has_include)
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif
#else
#include <libusb-1.0/libusb.h>
#endif

/** WCH's USB vendor ID, used by the programmer itself. */
#define WL_USB_VID_WCH 0x1a86u

/** WCH-LinkE while it is in RISC-V debug mode. */
#define WL_USB_PID_LINK_RV 0x8010u

/** WCH-LinkE while it is in ARM (SWD) mode. */
#define WL_USB_PID_LINK_ARM 0x8012u

/** Vendor ID the IAP bootloader presents. */
#define WL_USB_VID_IAP 0x4348u

/** Product ID of the IAP bootloader. */
#define WL_USB_PID_IAP 0x55e0u

/** How a LinkE is currently presenting itself. */
enum wl_usb_mode {
	WL_USB_MODE_RV = 0, /**< RISC-V debug, the mode we want */
	WL_USB_MODE_ARM,    /**< ARM/SWD mode; needs a mode switch */
	WL_USB_MODE_IAP,    /**< USB ISP bootloader instead of a probe */
	WL_USB_MODE_UNKNOWN,
};

/** Longest serial number we keep, including the terminator. */
#define WL_USB_SERIAL_MAX 64

/** Longest product string we keep, including the terminator. */
#define WL_USB_PRODUCT_MAX 128

/** One programmer, as found on the bus. */
typedef struct {
	libusb_device_handle *handle; /**< set once opened, else NULL */
	enum wl_usb_mode mode;
	uint16_t vid;
	uint16_t pid;
	uint8_t bus;
	uint8_t address;
	bool opened;
	char serial[WL_USB_SERIAL_MAX];
	char product[WL_USB_PRODUCT_MAX];
} wl_usb_link_t;

/** Human-readable mode name, for messages. */
const char *wl_usb_mode_name(enum wl_usb_mode mode);

/** Whether a mode can actually flash a target. */
bool wl_usb_mode_is_usable(enum wl_usb_mode mode);

/**
 * Initialise libusb.  Call once, before anything else here.
 * @return 0 on success, or a libusb error code (negative)
 */
int wl_usb_global_init(void);

/** Release libusb.  Safe to call even if init failed. */
void wl_usb_global_exit(void);

/**
 * Enumerate the bus and collect every WCH programmer found.
 *
 * A device that cannot be opened is still reported -- its strings are simply
 * left empty -- because "your programmer is present but not accessible" is a
 * much better message than "no programmer found", and that is exactly what a
 * missing udev rule looks like.
 *
 * @param links      array to fill
 * @param max_links  its capacity
 * @param found      receives how many were stored
 * @return 0 on success, or a libusb error code
 */
int wl_usb_scan(wl_usb_link_t *links, size_t max_links, size_t *found);

/**
 * Open a programmer and read its string descriptors.
 *
 * The USB interface is not claimed here: that belongs with the protocol
 * layer, which is the only thing that knows when it is finished with it.
 *
 * @return 0 on success, or a libusb error code
 */
int wl_usb_open(wl_usb_link_t *link);

/** Close a programmer opened with wl_usb_open(). */
void wl_usb_close(wl_usb_link_t *link);

/** Short libusb error text for a negative return code. */
const char *wl_usb_strerror(int code);

#endif /* WCHLINK_USB_H */
