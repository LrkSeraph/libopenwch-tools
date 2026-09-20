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

#include "log.h"
#include "usb.h"

static libusb_context *context;

const char *wl_usb_mode_name(enum wl_usb_mode mode) {
	switch (mode) {
	case WL_USB_MODE_RV:
		return "RISC-V debug";
	case WL_USB_MODE_ARM:
		return "ARM/SWD";
	case WL_USB_MODE_IAP:
		return "USB ISP bootloader";
	default:
		return "unknown";
	}
}

bool wl_usb_mode_is_usable(enum wl_usb_mode mode) {
	return mode == WL_USB_MODE_RV;
}

int wl_usb_global_init(void) {
	int rc;

	if (context != NULL) {
		return 0;
	}

	rc = libusb_init(&context);
	if (rc < 0) {
		context = NULL;
		return rc;
	}

	return 0;
}

void wl_usb_global_exit(void) {
	if (context != NULL) {
		libusb_exit(context);
		context = NULL;
	}
}

static enum wl_usb_mode classify(uint16_t vid, uint16_t pid) {
	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_LINK_RV) {
		return WL_USB_MODE_RV;
	}

	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_LINK_ARM) {
		return WL_USB_MODE_ARM;
	}

	if (vid == WL_USB_VID_IAP && pid == WL_USB_PID_IAP) {
		return WL_USB_MODE_IAP;
	}

	if (vid == WL_USB_VID_WCH && pid == WL_USB_PID_IAP) {
		return WL_USB_MODE_IAP;
	}

	return WL_USB_MODE_UNKNOWN;
}

/*
 * Read a string descriptor, tolerating every way it can fail.  An unreadable
 * serial number is not an error: it only means we cannot tell two programmers
 * apart, and the caller decides what to do about that.
 */
static void read_string(libusb_device_handle *handle,
			uint8_t index,
			char *out,
			size_t out_len) {
	int rc;

	out[0] = '\0';

	if (index == 0) {
		return;
	}

	rc = libusb_get_string_descriptor_ascii(
	    handle, index, (unsigned char *)out, (int)out_len);
	if (rc < 0) {
		out[0] = '\0';
		return;
	}

	out[out_len - 1] = '\0';
}

int wl_usb_scan(wl_usb_link_t *links, size_t max_links, size_t *found) {
	libusb_device **list = NULL;
	ssize_t count;
	ssize_t i;
	size_t n = 0;

	*found = 0;

	if (context == NULL) {
		/*
		 * Not a libusb-defined condition: libusb 1.0 has no
		 * "not initialised" code, so callers get OTHER and the text
		 * below explains it.  Reaching here is a bug in the caller.
		 */
		wl_error("libusb was not initialised; call "
			 "wl_usb_global_init() first");
		return LIBUSB_ERROR_OTHER;
	}

	count = libusb_get_device_list(context, &list);

	if (count < 0) {
		return (int)count;
	}

	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;
		enum wl_usb_mode mode;
		int rc;

		rc = libusb_get_device_descriptor(list[i], &desc);

		if (rc < 0) {
			continue;
		}

		mode = classify(desc.idVendor, desc.idProduct);

		if (mode == WL_USB_MODE_UNKNOWN) {
			continue;
		}

		if (n >= max_links) {
			wl_warn("more than %zu programmers attached; ignoring "
				"the rest",
				max_links);
			break;
		}

		memset(&links[n], 0, sizeof(links[n]));
		links[n].mode = mode;
		links[n].vid = desc.idVendor;
		links[n].pid = desc.idProduct;
		links[n].bus = libusb_get_bus_number(list[i]);
		links[n].address = libusb_get_device_address(list[i]);

		/*
		 * Opening here is what tells us apart "no programmer" from "no
		 * permission".  A failure is recorded by leaving the strings
		 * empty; the caller reports it.
		 */
		if (libusb_open(list[i], &links[n].handle) == 0) {
			read_string(links[n].handle, desc.iSerialNumber,
				    links[n].serial, sizeof(links[n].serial));
			read_string(links[n].handle, desc.iProduct,
				    links[n].product, sizeof(links[n].product));
			libusb_close(links[n].handle);
			links[n].handle = NULL;
		}

		n++;
	}

	libusb_free_device_list(list, 1);

	*found = n;

	return 0;
}

int wl_usb_open(wl_usb_link_t *link) {
	libusb_device **list = NULL;
	ssize_t count;
	ssize_t i;
	int rc = LIBUSB_ERROR_NO_DEVICE;

	if (context == NULL) {
		wl_error("libusb was not initialised; call "
			 "wl_usb_global_init() first");
		return LIBUSB_ERROR_OTHER;
	}

	if (link->opened) {
		return 0;
	}

	count = libusb_get_device_list(context, &list);

	if (count < 0) {
		return (int)count;
	}

	for (i = 0; i < count; i++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[i], &desc) < 0) {
			continue;
		}

		if (desc.idVendor != link->vid || desc.idProduct != link->pid) {
			continue;
		}

		if (libusb_get_bus_number(list[i]) != link->bus ||
		    libusb_get_device_address(list[i]) != link->address) {
			continue;
		}

		rc = libusb_open(list[i], &link->handle);

		if (rc == 0) {
			link->opened = true;
		}

		break;
	}

	libusb_free_device_list(list, 1);

	return rc;
}

void wl_usb_close(wl_usb_link_t *link) {
	if (link->opened && link->handle != NULL) {
		libusb_close(link->handle);
	}

	link->handle = NULL;
	link->opened = false;
}

const char *wl_usb_strerror(int code) {
	if (code < 0) {
		return libusb_error_name(code);
	}

	return "success";
}
