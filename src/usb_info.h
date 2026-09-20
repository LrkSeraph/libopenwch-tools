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

#ifndef WCHLINK_USB_INFO_H
#define WCHLINK_USB_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What a programmer looks like from the outside.
 *
 * This header deliberately does not include libusb: the protocol layer and the
 * tests need to describe and report a programmer, and neither of them should
 * have to link a USB stack to do it.  The handle itself stays private to
 * usb.c.
 */

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

/** The programmer's debug interface.  It is the only one it offers. */
#define WL_USB_INTERFACE 0

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
	enum wl_usb_mode mode;
	uint16_t vid;
	uint16_t pid;
	uint8_t bus;
	uint8_t address;
	bool accessible; /**< it could be opened, so it is not a permission
			    problem */
	char serial[WL_USB_SERIAL_MAX];
	char product[WL_USB_PRODUCT_MAX];
} wl_usb_info_t;

/** Human-readable mode name, for messages. */
const char *wl_usb_mode_name(enum wl_usb_mode mode);

/** Whether a mode can actually flash a target. */
bool wl_usb_mode_is_usable(enum wl_usb_mode mode);

#endif /* WCHLINK_USB_INFO_H */
