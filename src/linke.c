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

#include <stdio.h>
#include <string.h>

#include "dm.h"
#include "flash.h"
#include "linke.h"
#include "log.h"

/** Enough for every WCH programmer on one machine. */
#define WL_MAX_LINKS 8

/* --- the request layer -------------------------------------------------- */

/*
 * The command bytes.  They are observations of the programmer, not of any
 * published specification: WCH documents the *target* side of its parts, but
 * the LinkE's own protocol exists only in its firmware and in the tools that
 * speak it.
 */
#define WL_CMD_REG 0x08	    /**< debug-module register access */
#define WL_CMD_IFACE 0x0c   /**< select the target type and clock */
#define WL_CMD_CONTROL 0x0d /**< attach, release, reset, power */
#define WL_CMD_RUN 0x0b	    /**< leave reset and run */

/** Control sub-commands (the 4th byte of a WL_CMD_CONTROL request). */
#define WL_CTL_STATUS 0x01    /**< reply carries the programmer's version */
#define WL_CTL_ATTACH 0x02    /**< hold the target in reset and halt it */
#define WL_CTL_RESET_LOW 0x13 /**< drive the reset line low */
#define WL_CTL_RELEASE 0xff   /**< detach and let the target run */

/** Register access operations (the last byte of a WL_CMD_REG request). */
#define WL_REG_OP_READ 0x01
#define WL_REG_OP_WRITE 0x02

/** Reply framing: 0x82 echoes the request's 0x81. */
#define WL_REPLY_HEADER 0x82

/** A reply long enough for any command here. */
#define WL_REPLY_MAX 64

const char *wl_status_str(enum wl_status status) {
	switch (status) {
	case WL_OK:
		return "ok";
	case WL_ERR_USAGE:
		return "bad command line";
	case WL_ERR_NO_DEVICE:
		return "no programmer found";
	case WL_ERR_ACCESS:
		return "programmer not accessible";
	case WL_ERR_USB:
		return "USB error";
	case WL_ERR_IO:
		return "programmer error";
	case WL_ERR_TARGET:
		return "target error";
	case WL_ERR_VERIFY:
		return "verification failed";
	case WL_ERR_NOT_IMPLEMENTED:
		return "not implemented yet";
	default:
		return "unknown error";
	}
}

enum wl_status wl_linke_command(wl_linke_t *link,
				const uint8_t *request,
				size_t request_len,
				uint8_t *reply,
				size_t reply_max,
				size_t *reply_len) {
	uint8_t scratch[WL_REPLY_MAX];
	size_t got = 0;
	int rc;

	if (link == NULL || !link->open || link->transport == NULL) {
		wl_error("internal error: no programmer is open");
		return WL_ERR_USB;
	}

	if (reply == NULL || reply_max == 0) {
		reply = scratch;
		reply_max = sizeof(scratch);
	}

	rc = link->transport->command(link->ctx, request, request_len, reply,
				      reply_max, &got);

	if (rc < 0) {
		wl_error("the programmer did not answer (transport error %d)",
			 rc);
		return WL_ERR_USB;
	}

	if (reply_len != NULL) {
		*reply_len = got;
	}

	return WL_OK;
}

/** Whether a reply is the programmer's acknowledgement rather than an error. */
static bool reply_ok(const uint8_t *reply, size_t length) {
	if (length == 0) {
		return false;
	}

	/*
	 * A refusal is either a short reply -- the programmer answers a
	 * request it cannot satisfy with a four-byte "no" -- or a full-length
	 * reply whose status byte says so.  The status values 0x02 and 0x03
	 * are the two failure codes the register command uses.
	 */
	if (reply[0] != WL_REPLY_HEADER) {
		return false;
	}

	if (length == 9 && (reply[8] == 0x02 || reply[8] == 0x03)) {
		return false;
	}

	return true;
}

/**
 * Send a one-argument control command.
 *
 * The reply's contents are the caller's business: the control commands answer
 * with a status block whose shape differs between them, and the tool acts on
 * the transport result plus whatever it specifically asked for.
 */
static enum wl_status send_control(wl_linke_t *link,
				   uint8_t command,
				   uint8_t argument,
				   uint8_t *reply,
				   size_t reply_max,
				   size_t *reply_len) {
	uint8_t request[4];

	request[0] = 0x81;
	request[1] = command;
	request[2] = 0x01;
	request[3] = argument;

	return wl_linke_command(link, request, sizeof(request), reply,
				reply_max, reply_len);
}

/**
 * The programmer already holds the target; use that state for everything.
 *
 * Attaching is what stops the target's core, and the debug module is only
 * reachable while it is stopped.  Every command below therefore starts by
 * making sure this has happened.
 */
enum wl_status wl_linke_halt(wl_linke_t *link) {
	return send_control(link, WL_CMD_CONTROL, WL_CTL_ATTACH, NULL, 0, NULL);
}

enum wl_status wl_linke_resume(wl_linke_t *link) {
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;
	enum wl_status status;

	/* Leave reset, attach, release: the sequence that lets the part run. */
	status =
	    send_control(link, WL_CMD_RUN, 0x01, reply, sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	status = send_control(link, WL_CMD_CONTROL, WL_CTL_ATTACH, reply,
			      sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	return send_control(link, WL_CMD_CONTROL, WL_CTL_RELEASE, reply,
			    sizeof(reply), &length);
}

enum wl_status wl_linke_reset(wl_linke_t *link) {
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;
	enum wl_status status;

	/* Take the reset line low, then let go of it and run. */
	status = send_control(link, WL_CMD_CONTROL, WL_CTL_RESET_LOW, reply,
			      sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	return wl_linke_resume(link);
}

enum wl_status wl_linke_unbrick(wl_linke_t *link) {
	wl_warn("unbrick holds the reset line low and power-cycles the target");
	wl_warn("it clears the part's flash; this is not reversible");

	return wl_linke_reset(link);
}

/* --- version and interface ---------------------------------------------- */

/** The programmer models the LinkE has reported, for the version string. */
static const char *programmer_name(unsigned type) {
	switch (type) {
	case 1:
		return "WCH-Link (CH549)";
	case 2:
		return "WCH-Link (CH32V307)";
	case 3:
		return "WCH-Link (CH32V203)";
	case 4:
		return "WCH-LinkB";
	case 5:
		return "WCH-LinkW";
	case 18:
		return "WCH-LinkE";
	default:
		return "WCH programmer";
	}
}

enum wl_status
wl_linke_get_version(wl_linke_t *link, char *out, size_t out_len) {
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;
	enum wl_status status;

	if (out == NULL || out_len == 0) {
		return WL_ERR_USAGE;
	}

	out[0] = '\0';

	status = send_control(link, WL_CMD_CONTROL, WL_CTL_STATUS, reply,
			      sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	if (length < 6) {
		wl_error("the programmer's version reply was %zu bytes; "
			 "expected at least 6",
			 length);
		return WL_ERR_IO;
	}

	/*
	 * reply[3] and reply[4] are the firmware version, reply[5] the model.
	 * They are reported, never trusted: nothing here branches on them.
	 */
	snprintf(out, out_len, "%s %u.%u", programmer_name(reply[5]), reply[3],
		 reply[4]);

	return WL_OK;
}

enum wl_status wl_linke_set_interface(wl_linke_t *link, const wl_chip_t *chip) {
	uint8_t request[5];
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;
	enum wl_status status;

	if (chip == NULL) {
		return WL_ERR_USAGE;
	}

	if (link->configured && link->chip_id == chip->linke_id) {
		return WL_OK;
	}

	request[0] = 0x81;
	request[1] = WL_CMD_IFACE;
	request[2] = 0x02;
	request[3] = chip->linke_id;
	request[4] = chip->interface_speed;

	status = wl_linke_command(link, request, sizeof(request), reply,
				  sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	if (!reply_ok(reply, length)) {
		wl_error("the programmer refused to talk to %s (id 0x%02x)",
			 chip->name, chip->linke_id);
		return WL_ERR_TARGET;
	}

	link->chip_id = chip->linke_id;
	link->configured = true;

	wl_debug("programmer set to %s (id 0x%02x, speed 0x%02x)", chip->name,
		 chip->linke_id, chip->interface_speed);

	return WL_OK;
}

/* --- debug-module registers --------------------------------------------- */

static enum wl_status reg_access(wl_linke_t *link,
				 uint8_t reg,
				 uint32_t value,
				 uint8_t operation,
				 uint32_t *result) {
	uint8_t request[9];
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;
	enum wl_status status;

	request[0] = 0x81;
	request[1] = WL_CMD_REG;
	request[2] = 0x06;
	request[3] = reg;
	request[4] = (uint8_t)(value >> 24);
	request[5] = (uint8_t)(value >> 16);
	request[6] = (uint8_t)(value >> 8);
	request[7] = (uint8_t)value;
	request[8] = operation;

	status = wl_linke_command(link, request, sizeof(request), reply,
				  sizeof(reply), &length);

	if (status != WL_OK) {
		return status;
	}

	if (length != 9) {
		wl_error("register 0x%02x: the programmer answered %zu bytes, "
			 "expected 9 (is the target connected and halted?)",
			 reg, length);
		return WL_ERR_TARGET;
	}

	if (!reply_ok(reply, length)) {
		wl_error("register 0x%02x: the programmer reported an error "
			 "(status 0x%02x)",
			 reg, reply[8]);
		return WL_ERR_TARGET;
	}

	if (result != NULL) {
		*result = ((uint32_t)reply[4] << 24) |
			  ((uint32_t)reply[5] << 16) |
			  ((uint32_t)reply[6] << 8) | (uint32_t)reply[7];
	}

	return WL_OK;
}

enum wl_status
wl_linke_dmi_read(wl_linke_t *link, uint8_t reg, uint32_t *value) {
	if (value == NULL) {
		return WL_ERR_USAGE;
	}

	return reg_access(link, reg, 0, WL_REG_OP_READ, value);
}

enum wl_status
wl_linke_dmi_write(wl_linke_t *link, uint8_t reg, uint32_t value) {
	return reg_access(link, reg, value, WL_REG_OP_WRITE, NULL);
}

/* --- memory and flash --------------------------------------------------- */

enum wl_status wl_linke_read_memory(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    void *buffer,
				    size_t length) {
	enum wl_status status;

	if (chip != NULL) {
		status = wl_linke_set_interface(link, chip);

		if (status != WL_OK) {
			return status;
		}

		status = wl_linke_halt(link);

		if (status != WL_OK) {
			return status;
		}
	}

	return wl_dm_read_block(link, address, buffer, length);
}

bool wl_linke_can_flash(const wl_chip_t *chip) {
	return chip != NULL && chip->algo != WL_FLASH_UNSUPPORTED;
}

enum wl_status wl_linke_write_flash(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    const void *image,
				    size_t length,
				    bool verify,
				    void (*progress)(size_t done,
						     size_t total)) {
	enum wl_status status;

	if (chip == NULL) {
		return WL_ERR_USAGE;
	}

	status = wl_linke_set_interface(link, chip);

	if (status != WL_OK) {
		return status;
	}

	status = wl_linke_halt(link);

	if (status != WL_OK) {
		return status;
	}

	status = wl_flash_write(link, chip, address, image, length, progress);

	if (status != WL_OK) {
		return status;
	}

	if (verify) {
		status = wl_flash_verify(link, chip, address, image, length);
	}

	/*
	 * Let the target run whether or not verification passed: leaving it
	 * halted in reset after a failure helps nobody, and the user has the
	 * error message either way.
	 */
	(void)wl_linke_resume(link);

	return status;
}

void wl_linke_attach_transport(wl_linke_t *link,
			       const struct wl_transport *transport,
			       void *ctx) {
	memset(link, 0, sizeof(*link));
	link->transport = transport;
	link->ctx = ctx;
	link->open = true;
}

void wl_linke_detach(wl_linke_t *link) {
	uint8_t reply[WL_REPLY_MAX];
	size_t length = 0;

	if (link->open && link->transport != NULL) {
		/*
		 * Release before anything else: a programmer left attached
		 * keeps the target halted, which looks like a dead board to
		 * whoever picks it up next.
		 */
		(void)send_control(link, WL_CMD_CONTROL, WL_CTL_RELEASE, reply,
				   sizeof(reply), &length);
	}

	link->open = false;
	link->transport = NULL;
	link->ctx = NULL;
}
