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

#include <string.h>

#include "linke.h"
#include "log.h"

/*
 * This file is where the WCH-LinkE wire protocol will live (milestones M2 and
 * M3).  What is here now is the part that can be written and checked without
 * the hardware: picking a programmer, opening it, and reporting clearly what
 * is and is not implemented yet.
 *
 * The stubs below deliberately return WL_ERR_NOT_IMPLEMENTED rather than
 * pretending to succeed.  A flasher that reports success without writing is
 * worse than one that admits it cannot write.
 */

/** Enough for every WCH programmer on one machine. */
#define WL_MAX_LINKS 8

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
	case WL_ERR_NOT_IMPLEMENTED:
		return "not implemented yet";
	default:
		return "unknown error";
	}
}

enum wl_status wl_linke_open(wl_linke_t *link, const char *serial_filter) {
	wl_usb_link_t found[WL_MAX_LINKS];
	size_t count = 0;
	size_t i;
	size_t usable = 0;
	int rc;

	memset(link, 0, sizeof(*link));

	rc = wl_usb_scan(found, WL_MAX_LINKS, &count);

	if (rc < 0) {
		wl_error("cannot enumerate USB devices: %s",
			 wl_usb_strerror(rc));
		return WL_ERR_USB;
	}

	if (count == 0) {
		wl_error("no WCH programmer found on the USB bus");
		wl_error(
		    "check the programmer is plugged in, and that the udev "
		    "rules are installed (make install-udev-rules)");
		return WL_ERR_NO_DEVICE;
	}

	for (i = 0; i < count; i++) {
		wl_debug("found %04x:%04x at bus %u address %u, mode %s%s%s",
			 found[i].vid, found[i].pid, found[i].bus,
			 found[i].address, wl_usb_mode_name(found[i].mode),
			 found[i].product[0] != '\0' ? ", product \"" : "",
			 found[i].product[0] != '\0' ? found[i].product : "");

		if (found[i].handle == NULL) {
			/*
			 * libusb could not open it, which almost always means
			 * permissions rather than the device being absent.  Say
			 * so: "no programmer found" would send the user looking
			 * for a cable problem that is not there.
			 */
			wl_warn(
			    "programmer at bus %u address %u is present but "
			    "could not be opened -- usually a permissions "
			    "problem",
			    found[i].bus, found[i].address);
			continue;
		}

		if (!wl_usb_mode_is_usable(found[i].mode)) {
			wl_warn(
			    "programmer at bus %u address %u is in %s mode; "
			    "only RISC-V debug mode can flash a target",
			    found[i].bus, found[i].address,
			    wl_usb_mode_name(found[i].mode));
			continue;
		}

		if (serial_filter != NULL && serial_filter[0] != '\0' &&
		    strcmp(found[i].serial, serial_filter) != 0) {
			continue;
		}

		if (usable == 0) {
			link->usb = found[i];
		}

		usable++;
	}

	if (usable == 0) {
		return WL_ERR_ACCESS;
	}

	if (usable > 1 && serial_filter == NULL) {
		wl_warn(
		    "%zu programmers match; using the one at bus %u address %u",
		    usable, link->usb.bus, link->usb.address);
		wl_warn("pass --serial to choose deliberately");
	}

	rc = wl_usb_open(&link->usb);

	if (rc < 0) {
		wl_error("cannot open the programmer: %s", wl_usb_strerror(rc));
		return rc == LIBUSB_ERROR_ACCESS ? WL_ERR_ACCESS : WL_ERR_USB;
	}

	link->open = true;

	wl_info("using programmer at bus %u address %u%s%s", link->usb.bus,
		link->usb.address,
		link->usb.serial[0] != '\0' ? ", serial " : "",
		link->usb.serial[0] != '\0' ? link->usb.serial : "");

	return WL_OK;
}

void wl_linke_close(wl_linke_t *link) {
	if (link->open) {
		wl_usb_close(&link->usb);
	}

	link->open = false;
}

enum wl_status
wl_linke_get_version(wl_linke_t *link, char *out, size_t out_len) {
	(void)link;
	(void)out;
	(void)out_len;

	return WL_ERR_NOT_IMPLEMENTED;
}

enum wl_status wl_linke_halt(wl_linke_t *link, bool halt) {
	(void)link;
	(void)halt;

	return WL_ERR_NOT_IMPLEMENTED;
}

enum wl_status wl_linke_read_memory(wl_linke_t *link,
				    uint32_t address,
				    void *buffer,
				    size_t length) {
	(void)link;
	(void)address;
	(void)buffer;
	(void)length;

	return WL_ERR_NOT_IMPLEMENTED;
}

enum wl_status wl_linke_write_flash(wl_linke_t *link,
				    const wl_chip_t *chip,
				    uint32_t address,
				    const void *image,
				    size_t length,
				    bool verify) {
	(void)link;
	(void)chip;
	(void)address;
	(void)image;
	(void)length;
	(void)verify;

	return WL_ERR_NOT_IMPLEMENTED;
}

enum wl_status wl_linke_reset(wl_linke_t *link) {
	(void)link;

	return WL_ERR_NOT_IMPLEMENTED;
}

enum wl_status wl_linke_unbrick(wl_linke_t *link) {
	(void)link;

	return WL_ERR_NOT_IMPLEMENTED;
}
