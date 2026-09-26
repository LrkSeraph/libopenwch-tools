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
#include "transport.h"
#include "usb_info.h"

/** Result of an operation.  Everything the tool does returns one of these. */
enum wl_status {
	WL_OK = 0,
	WL_ERR_USAGE,	  /**< the command line asked for something odd */
	WL_ERR_NO_DEVICE, /**< no programmer on the bus */
	WL_ERR_ACCESS,	  /**< programmer present but not openable */
	WL_ERR_USB,	  /**< a libusb call failed */
	WL_ERR_IO,	  /**< the programmer answered with an error */
	WL_ERR_TARGET,	  /**< the target misbehaved or is the wrong part */
	WL_ERR_VERIFY,	  /**< what was written did not read back */
	WL_ERR_NOT_IMPLEMENTED, /**< this family or milestone has not got there */
};

/** Human-readable form of a status, for messages. */
const char *wl_status_str(enum wl_status status);

/*
 * The WCH-LinkE speaks a small packet protocol over two bulk endpoints:
 *
 *   EP 0x01   out   a request, 4 bytes or a few more
 *   EP 0x81   in    its reply, which echoes the request with data filled in
 *   EP 0x02   out   bulk payload for the "write a block" command
 *
 * Every request starts with 0x81 and every reply with 0x82.  A request is
 * <0x81> <command> <length> <arguments...>; the reply repeats those first
 * bytes and appends or replaces whatever the command is about.  A reply is
 * read even when it is of no interest: the programmer will not accept the next
 * request until the previous answer has been collected.
 *
 * The transport is behind a vtable (transport.h) so that the same protocol
 * code can be driven by the simulated programmer in the tests.
 */

/** One open programmer. */
typedef struct {
	const struct wl_transport *transport;
	void *ctx;	    /**< transport-private state */
	wl_usb_info_t info; /**< what was found on the bus, for reporting */
	bool open;
	uint8_t chip_id; /**< LinkE chip type last selected, 0 if none */
	bool configured; /**< whether the interface is set up for chip_id */
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

/** Wrap an already chosen transport, for the tests. */
void wl_linke_attach_transport(wl_linke_t *link,
			       const struct wl_transport *transport,
			       void *ctx);

/**
 * Let the target run and forget the transport.
 *
 * Implemented in linke.c, because it is protocol work; wl_linke_close() builds
 * on it and adds the USB half.
 */
void wl_linke_detach(wl_linke_t *link);

/** Close a programmer opened with wl_linke_open(). */
void wl_linke_close(wl_linke_t *link);

/* --- the request layer -------------------------------------------------- */

/**
 * Send a raw request and collect the reply.
 *
 * @param reply      may be NULL when the reply is not wanted
 * @param reply_len  may be NULL when the length is not wanted
 */
enum wl_status wl_linke_command(wl_linke_t *link,
				const uint8_t *request,
				size_t request_len,
				uint8_t *reply,
				size_t reply_max,
				size_t *reply_len);

/** Read the programmer's own version.  Fills "LinkE 1.2" style text. */
enum wl_status
wl_linke_get_version(wl_linke_t *link, char *out, size_t out_len);

/**
 * Identify the part attached to the programmer.
 *
 * The WCH-LinkE answers its attach command with a family byte and a 16-bit
 * model value.  This function matches that pair against the chip table and
 * stores the result in @p chip.  It also leaves the target held in reset,
 * exactly as wl_linke_halt() does, so callers that immediately configure the
 * interface can do so without a second attach.
 *
 * @param chip  receives the matching part on success
 * @return WL_OK, WL_ERR_TARGET when no target answers or the pair is unknown,
 *         or another wl_status on transport failure
 */
enum wl_status wl_linke_identify_chip(wl_linke_t *link, const wl_chip_t **chip);

/**
 * Tell the programmer which part it is talking to, and at what clock.
 *
 * This must happen before any target access: it selects the debug transport
 * timing and the chip-specific behaviour inside the programmer.
 */
enum wl_status wl_linke_set_interface(wl_linke_t *link, const wl_chip_t *chip);

/** Hold the target in reset and stop its core. */
enum wl_status wl_linke_halt(wl_linke_t *link);

/** Let the target run. */
enum wl_status wl_linke_resume(wl_linke_t *link);

/** Reset the target and let it run. */
enum wl_status wl_linke_reset(wl_linke_t *link);

/** Release the target from reset after a power cycle.  Milestone 3. */
enum wl_status wl_linke_unbrick(wl_linke_t *link);

/* --- the debug-module layer --------------------------------------------- */

/**
 * Read one debug-module register.
 *
 * @param reg  the 7-bit DMI address, e.g. WL_DMI_DATA0
 */
enum wl_status
wl_linke_dmi_read(wl_linke_t *link, uint8_t reg, uint32_t *value);

/** Write one debug-module register. */
enum wl_status
wl_linke_dmi_write(wl_linke_t *link, uint8_t reg, uint32_t value);

/* --- target memory and flash ------------------------------------------- */

/** Read target memory. */
enum wl_status wl_linke_read_memory(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    void *buffer,
				    size_t length);

/**
 * Erase, write and optionally verify a flash image.
 *
 * The image is written at @p address, which is an address in the flash
 * mapping the programmer uses (0x08000000 on every part here).
 *
 * @param progress  called with (done, total) bytes, or NULL
 */
enum wl_status wl_linke_write_flash(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    const void *image,
				    size_t length,
				    bool verify,
				    void (*progress)(size_t done,
						     size_t total));

/** Whether this build can write flash for @p chip. */
bool wl_linke_can_flash(const wl_chip_t *chip);

#endif /* WCHLINK_LINKE_H */
