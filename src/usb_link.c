#define _POSIX_C_SOURCE 200809L

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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

/** How long to wait for a programmer to re-enumerate after a mode command. */
#define WL_MODE_SWITCH_WAIT_MS 5000u

/** How often to rescan the bus while waiting for re-enumeration. */
#define WL_MODE_SWITCH_POLL_MS 100u

/** Does a discovered programmer satisfy the --serial filter? */
static bool serial_matches(const wl_usb_info_t *info,
			   const char *serial_filter) {
	return serial_filter == NULL || serial_filter[0] == '\0' ||
	       strcmp(info->serial, serial_filter) == 0;
}

/** Sleep for a short poll interval, restarting if a signal interrupts us. */
static void sleep_ms(unsigned ms) {
	struct timespec req;

	req.tv_sec = (time_t)(ms / 1000u);
	req.tv_nsec = (long)(ms % 1000u) * 1000000L;

	while (nanosleep(&req, &req) != 0 && errno == EINTR) {
		/* keep waiting for the remaining interval */
	}
}

/**
 * Wait for a programmer in @p mode to appear after a mode-switch command.
 *
 * The USB device re-enumerates with a new bus/address after the switch, so the
 * only reliable way to find it is to rescan.  The serial filter is applied
 * again: several programmers can be attached, and only the one we asked to
 * switch should be picked up.
 *
 * @return WL_OK, WL_ERR_NO_DEVICE after the timeout, or WL_ERR_USB on scan
 *         failure
 */
static enum wl_status wait_for_mode(enum wl_usb_mode mode,
				    const char *serial_filter,
				    wl_usb_info_t *out) {
	unsigned waited = 0;

	while (waited < WL_MODE_SWITCH_WAIT_MS) {
		wl_usb_info_t found[WL_MAX_LINKS];
		size_t count = 0;
		size_t i;
		int rc;

		sleep_ms(WL_MODE_SWITCH_POLL_MS);
		waited += WL_MODE_SWITCH_POLL_MS;

		rc = wl_usb_scan(found, WL_MAX_LINKS, &count);

		if (rc < 0) {
			return WL_ERR_USB;
		}

		for (i = 0; i < count; i++) {
			if (found[i].accessible && found[i].mode == mode &&
			    serial_matches(&found[i], serial_filter)) {
				*out = found[i];
				return WL_OK;
			}
		}
	}

	return WL_ERR_NO_DEVICE;
}

enum wl_status wl_linke_open(wl_linke_t *link, const char *serial_filter) {
	wl_usb_info_t found[WL_MAX_LINKS];
	wl_usb_info_t arm_candidate;
	wl_usb_info_t iap_candidate;
	bool have_arm = false;
	bool have_iap = false;
	size_t count = 0;
	size_t i;
	size_t usable = 0;
	wl_usb_link_t *usb = NULL;
	enum wl_status status;
	int rc;

	memset(link, 0, sizeof(*link));
	memset(&arm_candidate, 0, sizeof(arm_candidate));
	memset(&iap_candidate, 0, sizeof(iap_candidate));

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

		if (!serial_matches(&found[i], serial_filter)) {
			wl_debug(
			    "ignoring programmer at bus %u address %u: serial "
			    "does not match",
			    found[i].bus, found[i].address);
			continue;
		}

		switch (found[i].mode) {
		case WL_USB_MODE_RV:
			if (usable == 0) {
				link->info = found[i];
			}

			usable++;
			break;

		case WL_USB_MODE_ARM:
			if (!have_arm) {
				arm_candidate = found[i];
				have_arm = true;
			}
			break;

		case WL_USB_MODE_IAP:
			if (!have_iap) {
				iap_candidate = found[i];
				have_iap = true;
			}
			break;

		default:
			wl_warn("programmer at bus %u address %u reports an "
				"unknown mode; ignoring it",
				found[i].bus, found[i].address);
			break;
		}
	}

	/*
 * A WCH-LinkE in ARM/SWD mode is the normal state after it has been
 * used as an ARM debugger.  Switch it back to RISC-V debug mode
 * automatically rather than making the user find another tool.
 */
	if (usable == 0 && have_arm) {
		wl_info("programmer at bus %u address %u is in ARM/SWD mode; "
			"switching to RISC-V debug mode",
			arm_candidate.bus, arm_candidate.address);

		rc = wl_usb_switch_arm_to_rv(&arm_candidate);

		if (rc < 0) {
			wl_error("cannot switch the programmer to RISC-V "
				 "mode: %s",
				 wl_usb_strerror(rc));
			return wl_usb_error_is_access(rc) ? WL_ERR_ACCESS
							  : WL_ERR_USB;
		}

		status =
		    wait_for_mode(WL_USB_MODE_RV, serial_filter, &link->info);

		if (status != WL_OK) {
			wl_error("the mode-switch command was accepted, but no "
				 "RISC-V debug programmer appeared; replug the "
				 "WCH-LinkE and try again");
			return status;
		}

		usable = 1;
		wl_info("programmer switched to RISC-V debug mode");
	}

	/*
 * The IAP bootloader is the other recoverable state.  Eject it and
 * wait for the normal RISC-V debug interface.
 */
	if (usable == 0 && have_iap) {
		wl_info("programmer at bus %u address %u is in IAP mode; "
			"trying to eject it",
			iap_candidate.bus, iap_candidate.address);

		rc = wl_usb_eject_iap(&iap_candidate);

		if (rc < 0) {
			wl_error(
			    "cannot eject the programmer from IAP mode: %s",
			    wl_usb_strerror(rc));
			return wl_usb_error_is_access(rc) ? WL_ERR_ACCESS
							  : WL_ERR_USB;
		}

		status =
		    wait_for_mode(WL_USB_MODE_RV, serial_filter, &link->info);

		if (status != WL_OK) {
			wl_error(
			    "the eject command was accepted, but no RISC-V "
			    "debug programmer appeared; replug the WCH-LinkE "
			    "and try again");
			return status;
		}

		usable = 1;
		wl_info("programmer was ejected from IAP mode and is now in "
			"RISC-V debug mode");
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
