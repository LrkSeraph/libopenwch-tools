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

#include "linke.h"
#include "log.h"
#include "usb.h"

/*
 * Finding and opening a programmer, which is the one part of the protocol
 * layer that needs the host's USB stack.  Keeping it here rather than in
 * linke.c is what lets the tests link the protocol, the debug module and the
 * flash algorithms without libusb: everything they exercise is transport
 * agnostic, and this file is the only place that is not.
 */

/** Enough for every WCH programmer on one machine. */
#define WL_MAX_LINKS 8

enum wl_status wl_linke_open(wl_linke_t *link, const char *serial_filter) {
	wl_usb_info_t found[WL_MAX_LINKS];
	size_t count = 0;
	size_t i;
	size_t usable = 0;
	wl_usb_link_t *usb = NULL;
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

		if (!found[i].accessible) {
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
			link->info = found[i];
		}

		usable++;
	}

	if (usable == 0) {
		return WL_ERR_ACCESS;
	}

	if (usable > 1 && serial_filter == NULL) {
		wl_warn(
		    "%zu programmers match; using the one at bus %u address %u",
		    usable, link->info.bus, link->info.address);
		wl_warn("pass --serial to choose deliberately");
	}

	rc = wl_usb_open(&link->info, &usb);

	if (rc < 0) {
		wl_error("cannot open the programmer: %s", wl_usb_strerror(rc));
		return wl_usb_error_is_access(rc) ? WL_ERR_ACCESS : WL_ERR_USB;
	}

	link->transport = wl_usb_transport();
	link->ctx = usb;
	link->open = true;

	wl_info("using programmer at bus %u address %u%s%s", link->info.bus,
		link->info.address,
		link->info.serial[0] != '\0' ? ", serial " : "",
		link->info.serial[0] != '\0' ? link->info.serial : "");

	return WL_OK;
}

void wl_linke_close(wl_linke_t *link) {
	wl_usb_link_t *usb = (wl_usb_link_t *)link->ctx;

	wl_linke_detach(link);
	wl_usb_close(usb);
}
