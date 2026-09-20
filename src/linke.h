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

#ifndef WCHLINK_LINKE_H
#define WCHLINK_LINKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "target.h"
#include "usb.h"

/** Result of an operation.  Everything the tool does returns one of these. */
enum wl_status {
	WL_OK = 0,
	WL_ERR_USAGE,	  /**< the command line asked for something odd */
	WL_ERR_NO_DEVICE, /**< no programmer on the bus */
	WL_ERR_ACCESS,	  /**< programmer present but not openable */
	WL_ERR_USB,	  /**< a libusb call failed */
	WL_ERR_IO,	  /**< the programmer answered with an error */
	WL_ERR_TARGET,	  /**< the target misbehaved or is the wrong part */
	WL_ERR_NOT_IMPLEMENTED, /**< this milestone has not got there yet */
};

/** Human-readable form of a status, for messages. */
const char *wl_status_str(enum wl_status status);

/** One open programmer. */
typedef struct {
	wl_usb_link_t usb;
	bool open;
} wl_linke_t;

/**
 * Fill @p link with the first usable programmer found, and open it.
 *
 * When several are attached and @p serial is not NULL, the one whose serial
 * number matches is chosen; if none matches, WL_ERR_NO_DEVICE is returned
 * rather than silently using a different probe.
 *
 * @param serial_filter  wanted serial number, or NULL for "any"
 */
enum wl_status wl_linke_open(wl_linke_t *link, const char *serial_filter);

/** Close a programmer opened with wl_linke_open(). */
void wl_linke_close(wl_linke_t *link);

/**
 * Read the programmer's own version string.
 *
 * Milestone 2.  The WCH-LinkE reports this through its debug registers, so it
 * cannot be answered from the USB descriptors alone.
 */
enum wl_status
wl_linke_get_version(wl_linke_t *link, char *out, size_t out_len);

/** Halt or resume the target.  Milestone 2. */
enum wl_status wl_linke_halt(wl_linke_t *link, bool halt);

/** Read target memory.  Milestone 2. */
enum wl_status wl_linke_read_memory(wl_linke_t *link,
				    uint32_t address,
				    void *buffer,
				    size_t length);

/** Erase, write and verify a flash image.  Milestone 3. */
enum wl_status wl_linke_write_flash(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    const void *image,
				    size_t length,
				    bool verify);

/** Reset the target.  Milestone 3. */
enum wl_status wl_linke_reset(wl_linke_t *link);

/** Clear all code flash by power-cycling the target.  Milestone 3. */
enum wl_status wl_linke_unbrick(wl_linke_t *link);

#endif /* WCHLINK_LINKE_H */
