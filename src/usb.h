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

#include "usb_info.h"
#include "transport.h"

/*
 * The USB half of the tool: libusb lives here and nowhere else.
 *
 * A programmer is described by wl_usb_info_t (usb_info.h) so that everything
 * above this file -- and the tests -- can talk about one without linking a USB
 * stack.  wl_usb_link_t is the open handle, and is opaque outside usb.c.
 */

typedef struct wl_usb_link wl_usb_link_t;

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
 * A device that cannot be opened is still reported, with @c accessible false:
 * "your programmer is present but not accessible" is a much better message
 * than "no programmer found", and that is exactly what a missing udev rule
 * looks like.
 *
 * @param found      array to fill
 * @param max_links  its capacity
 * @return 0 on success, or a libusb error code
 */
int wl_usb_scan(wl_usb_info_t *found, size_t max_links, size_t *count);

/**
 * Open a programmer and claim its interface.
 *
 * Claiming is what makes bulk transfers work, and it is done here because this
 * is the layer that knows when the programmer is finished with.
 *
 * @return 0 on success, or a libusb error code
 */
int wl_usb_open(const wl_usb_info_t *info, wl_usb_link_t **out);

/** Release the interface and close a programmer. */
void wl_usb_close(wl_usb_link_t *link);

/** Short libusb error text for a negative return code. */
const char *wl_usb_strerror(int code);

/**
 * Whether an error code means "present but not permitted".
 *
 * Exists so that callers can report a permissions problem without including
 * libusb's header to compare against its constants.
 */
bool wl_usb_error_is_access(int code);

/**
 * The real transport, over the programmer's bulk endpoints.
 *
 * Its context is a wl_usb_link_t, so a transport outlives nothing: closing the
 * link closes the transport with it.
 */
const struct wl_transport *wl_usb_transport(void);

#endif /* WCHLINK_USB_H */
